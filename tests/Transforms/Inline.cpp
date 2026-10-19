/* Licensed under the MIT License. Native inlining without the C oracle. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Inline.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/Calls.hpp"
#include "../IR/DataUses.hpp"
#include "InlineFixtures.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
void optimize(Context &context, transforms::PassObserver observer = {}) {
  transforms::ModulePassManager passes; passes.add_function<transforms::SimplifyPass>(); passes.add_function<transforms::InlinePass>();
  auto result = passes.run(context, {}, std::move(observer)); INFO((result ? "" : result.error().message)); REQUIRE(result); REQUIRE(context.verify());
}
void roundtrip(const Context &context) {
  test::check_data_uses(context);
  auto text = serialization::write_text(context); INFO((text ? "" : text.error().message)); REQUIRE(text);
  auto parsed = serialization::parse_text(*text); REQUIRE(parsed); REQUIRE((*parsed)->verify());
  auto binary = serialization::write_binary(context); REQUIRE(binary); auto decoded = serialization::read_binary(*binary); REQUIRE(decoded);
  auto again = serialization::write_binary(**decoded); REQUIRE(again); CHECK(*again == *binary);
}
std::size_t calls(const Function &function) {
  std::size_t count = 0;
  for (auto &block : function.blocks()) for (auto &i : block.instructions()) count += instruction_descriptor(i.opcode())->count == OperandCount::Call;
  return count;
}
}
TEST_CASE("inline queries live label data uses rather than a stage-entry snapshot") {
  Context context; auto m = context.create_module("live_uses"); REQUIRE(m);
  auto callee = (*m)->create_function("callee", {{Type::i64()}, {}}); REQUIRE(callee);
  auto b = (*callee)->create_block(); REQUIRE(b); REQUIRE(IRBuilder(**b).ret(Operand(42)));
  auto caller = (*m)->create_function("caller", {{Type::i64()}, {}}); REQUIRE(caller); b = (*caller)->create_block(); REQUIRE(b);
  IRBuilder builder(**b); auto call = builder.call(**callee); REQUIRE(call); auto result = call->value(); REQUIRE(result); REQUIRE(builder.ret(Operand(**result)));
  transforms::FunctionPassManager simplify; simplify.add<transforms::SimplifyPass>(); REQUIRE(simplify.run(context));
  transforms::InlinePass inliner; REQUIRE(inliner.initialize(context));
  auto label = (*callee)->create_label(*(*callee)->blocks().begin()); REQUIRE(label);
  REQUIRE((*m)->create_data("address", LabelReference{*label, *label, 1}));
  analysis::FunctionAnalysisManager analyses(**caller); auto inlined = inliner.run(**caller, analyses); inliner.finalize();
  REQUIRE(inlined); CHECK(inlined->transformations == 0); CHECK(calls(**caller) == 1); roundtrip(context);
}
TEST_CASE("native inline preserves source handles and owns all cloned nodes independently") {
  for (unsigned n = 0; n <= static_cast<unsigned>(test::CallExample::Inline); ++n) {
    CAPTURE(n); auto source = test::call_example(static_cast<test::CallExample>(n)); auto original = dump(*source);
    auto &source_function = *source->modules().begin()->functions().begin(); auto identity = &*source_function.blocks().begin()->instructions().begin();
    auto copy = clone(*source); REQUIRE(copy); bool inline_started = false; std::size_t simplified = 0;
    optimize(**copy, [&](const transforms::PassEvent &event) -> Result<void> {
      if (event.pass == "simplify" && event.phase == transforms::PassEvent::Phase::After) { CHECK_FALSE(inline_started); ++simplified; }
      if (event.pass == "inline") { inline_started = true; CHECK(simplified == 2); }
      return {};
    });
    CHECK(dump(*source) == original); CHECK(identity == &*source_function.blocks().begin()->instructions().begin());
    auto &entry = *std::prev((*copy)->modules().begin()->functions().end());
    bool forbidden = n == unsigned(test::CallExample::Indirect) || n == unsigned(test::CallExample::Variadic) || n == unsigned(test::CallExample::VariadicBlock);
    CHECK(calls(entry) == unsigned(forbidden)); source.reset(); REQUIRE((*copy)->verify()); roundtrip(**copy);
    transforms::FunctionPassManager repeat; repeat.add<transforms::InlinePass>(); REQUIRE(repeat.run(**copy)); roundtrip(**copy);
  }
}
TEST_CASE("native inline roundtrips all threshold frame cold and relocation fixtures") {
  unsigned n = 0;
  for (auto &text : test::inline_fixtures()) {
    CAPTURE(n); ++n; auto context = serialization::parse_text(text); INFO((context ? "" : context.error().message)); REQUIRE(context);
    auto copy = clone(**context); REQUIRE(copy); auto original = dump(**context); optimize(**copy);
    CHECK(dump(**context) == original); context->reset(); roundtrip(**copy);
    for (auto &module : (*copy)->modules()) for (auto &function : module.functions()) {
      auto graph = analysis::ControlFlowAnalysis::run(function); REQUIRE(graph);
      for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) {
        for (auto &operand : instruction.operands()) {
          if (auto reg = operand.get<const Register *>()) CHECK(&(*reg)->function() == &function);
          if (auto label = operand.get<const Label *>()) CHECK(&(*label)->function() == &function);
          if (auto symbol = operand.get<SymbolReference>()) CHECK(&symbol->symbol->module() == &module);
        }
      }
    }
  }
}
TEST_CASE("native inline keeps hardware identity across assignment and nested calls") {
  Context context; auto m = context.create_module("hardware"); REQUIRE(m);
  auto leaf = (*m)->create_function("leaf", {}); REQUIRE(leaf); auto b = (*leaf)->create_block(); REQUIRE(b);
  auto bound = (*leaf)->create_register(Type::i64(), "bound", "r5"); REQUIRE(bound); IRBuilder callee(**b);
  REQUIRE(callee.assign(**bound, 0)); REQUIRE(callee.ret());
  auto mid = (*m)->create_function("mid", {}); REQUIRE(mid); b = (*mid)->create_block(); REQUIRE(b); IRBuilder middle(**b);
  REQUIRE(middle.call(**leaf)); REQUIRE(middle.call(**leaf)); REQUIRE(middle.ret());
  auto entry = (*m)->create_function("entry", {{Type::i64()}, {}}); REQUIRE(entry); b = (*entry)->create_block(); REQUIRE(b);
  auto caller_bound = (*entry)->create_register(Type::i64(), "caller_bound", "r5"); REQUIRE(caller_bound); IRBuilder caller(**b);
  auto assign = caller.assign(**caller_bound, 42); REQUIRE(assign); REQUIRE(caller.call(**mid)); auto ret = caller.ret(Operand(**caller_bound)); REQUIRE(ret);
  auto assignment_identity = *assign, return_identity = *ret; optimize(context);
  CHECK(*assign == assignment_identity); CHECK(*ret == return_identity); CHECK((*entry)->find_register("caller_bound") == *caller_bound);
  unsigned bindings = 0, overwrites = 0;
  for (auto &r : (*entry)->registers()) bindings += !r.hardware_binding().empty(); CHECK(bindings == 1);
  for (auto &block : (*entry)->blocks()) for (auto &i : block.instructions())
    if (&i != assignment_identity && i.opcode() == Opcode::Mov && i.operands()[0] == Operand(**caller_bound)) ++overwrites;
  CHECK(overwrites == 3); CHECK(calls(**entry) == 0); roundtrip(context);
}
TEST_CASE("native inline errors name the function and stage and reset scoped state") {
  auto context = test::call_example(test::CallExample::Direct); transforms::FunctionPassManager passes; passes.add<transforms::InlinePass>();
  auto result = passes.run(*context); CHECK_FALSE(result);
  CHECK(result.error().message.find("[inline]") != std::string::npos);
  CHECK(result.error().message.find("calls/") != std::string::npos);
  optimize(*context); REQUIRE(passes.run(*context));
  auto second = test::call_example(test::CallExample::Direct); transforms::FunctionPassManager frontend;
  frontend.add<transforms::SimplifyPass>(); frontend.add<transforms::InlinePass>();
  result = frontend.run(*second, [](const transforms::PassEvent &event) -> Result<void> {
    if (event.pass == "inline") return Error{ErrorCode::InvalidArgument, "observer failure"}; return {};
  });
  CHECK_FALSE(result); CHECK(result.error().message.find("[inline]") != std::string::npos); REQUIRE(frontend.run(*second));
}
TEST_CASE("native block layout moves preserve labels instructions storage and CFG revisions") {
  Context context; auto m = context.create_module("layout"); REQUIRE(m); auto f = (*m)->create_function("entry", {}); REQUIRE(f);
  auto first = (*f)->create_block("first"), last = (*f)->create_block("last"); REQUIRE(first); REQUIRE(last);
  auto label = (*f)->create_label(**last); REQUIRE(label); auto jump = IRBuilder(**first).jump(**label); REQUIRE(jump);
  auto ret = IRBuilder(**last).ret(); REQUIRE(ret); auto storage = (*ret)->operands().data(); IRRewriter rewriter(**f);
  auto before = (*f)->revisions(); REQUIRE(rewriter.move_block_before(**last, *first));
  CHECK(&*(*f)->blocks().begin() == *last); CHECK(&(*ret)->block() == *last); CHECK((*ret)->operands().data() == storage);
  CHECK(&(*label)->block() == *last); CHECK((*f)->revisions().control_flow > before.control_flow);
  REQUIRE(rewriter.move_block_before(**first, *last)); REQUIRE(context.verify());
  auto other = (*m)->create_function("other", {}); REQUIRE(other); auto foreign = (*other)->create_block(); REQUIRE(foreign);
  CHECK_FALSE(rewriter.move_block_before(**last, *foreign));
}
TEST_CASE("native inline reports frame growth overflow before encoding") {
  auto context = serialization::parse_text("m:module\np:proto i64\nleaf:func i64\nlocal i64:a, i64:r\nalloca a, 9223372036854775792\nmov i64:(a), 42\nmov r, i64:(a)\nret r\nendfunc\nentry:func i64\nlocal i64:a, i64:r\nalloca a, 32\nmov i64:(a), 0\ncall p, leaf, r\nret r\nendfunc\nendmodule\n"); REQUIRE(context);
  transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>(); passes.add<transforms::InlinePass>();
  auto result = passes.run(**context); CHECK_FALSE(result); CHECK(result.error().message.find("entry [inline]") != std::string::npos);
  CHECK(result.error().message.find("frame growth overflow") != std::string::npos);
}
TEST_CASE("native rewriter transfers owned operands and validates before consuming them") {
  Context context; auto m = context.create_module("transfer"); REQUIRE(m); auto f = (*m)->create_function("entry", {}); REQUIRE(f);
  auto block = (*f)->create_block(); REQUIRE(block); auto r = (*f)->create_register(Type::i64()); REQUIRE(r); IRRewriter rewriter(**f);
  std::vector<Operand> operands{Operand(**r), Operand(42)}; auto storage = operands.data();
  auto instruction = rewriter.insert(**block, nullptr, Opcode::Mov, std::move(operands)); REQUIRE(instruction);
  CHECK((*instruction)->operands().data() == storage); CHECK((*instruction)->operands()[1] == Operand(42));
  std::vector<Operand> invalid{Operand(**r)}; storage = invalid.data();
  auto result = rewriter.insert(**block, nullptr, Opcode::Mov, std::move(invalid)); CHECK_FALSE(result);
  CHECK(invalid.data() == storage); CHECK(invalid.size() == 1); CHECK((*block)->size() == 1);
  REQUIRE(IRBuilder(**block).ret()); REQUIRE(context.verify());
}
