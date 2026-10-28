/* Licensed under the MIT License. Function inlining tests. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
void optimize (Program &context, transforms::PassObserver observer = {}) {
  transforms::ModulePassManager passes;
  passes.add_function<transforms::SimplifyPass> ();
  passes.add_function<transforms::InlinePass> ();
  auto result = passes.run (context, {}, std::move (observer));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
void roundtrip (const Program &context) {
  test::check_data_uses (context);
  auto text = serialization::write_text (context);
  INFO (((text).has_value () ? "" : text.error ().message));
  REQUIRE ((text).has_value ());
  auto parsed = serialization::parse_text ((text).value ());
  REQUIRE ((parsed).has_value ());
  REQUIRE ((parsed.value ().verify ()).has_value ());
  auto binary = serialization::write_binary (context);
  REQUIRE ((binary).has_value ());
  auto decoded = serialization::read_binary ((binary).value ());
  REQUIRE ((decoded).has_value ());
  auto again = serialization::write_binary ((decoded).value ());
  REQUIRE ((again).has_value ());
  CHECK ((again).value () == (binary).value ());
}
std::size_t calls (const Function &function) {
  std::size_t count = 0;
  for (auto &block : function.blocks ())
    for (auto &i : block.instructions ())
      count += instruction_descriptor (i.opcode ()).get ()->count == OperandCount::Call;
  return count;
}
}  // namespace
TEST_CASE ("inline queries live label data uses rather than a stage-entry snapshot") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("live_uses");
  REQUIRE ((m).has_value ());
  auto callee
    = mirnext::ir::detail::access (((m)).value ()).create_function ("callee", {{Type::i64 ()}, {}});
  REQUIRE ((callee).has_value ());
  auto b = mirnext::ir::detail::access (((callee)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  REQUIRE ((InstructionBuilder ((b).value ()).ret (Operand (42))).has_value ());
  auto caller
    = mirnext::ir::detail::access (((m)).value ()).create_function ("caller", {{Type::i64 ()}, {}});
  REQUIRE ((caller).has_value ());
  b = mirnext::ir::detail::access (((caller)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  InstructionBuilder builder ((b).value ());
  auto call = builder.call ((callee).value ());
  REQUIRE ((call).has_value ());
  auto result = call.value ().value ();
  REQUIRE ((result).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  transforms::FunctionPassManager simplify;
  simplify.add<transforms::SimplifyPass> ();
  REQUIRE ((simplify.run (context)).has_value ());
  transforms::InlinePass inliner;
  REQUIRE ((inliner.initialize (context)).has_value ());
  auto label = mirnext::ir::detail::access (((callee)).value ())
                 .create_label (*callee.value ().blocks ().begin ());
  REQUIRE ((label).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ())
       .create_data ("address",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                           &(label).value (), 1)))
      .has_value ());
  analysis::FunctionAnalysisManager analyses ((caller).value ());
  auto inlined = ::mirnext::transforms::detail::run_pass (inliner, (caller).value (), analyses);
  inliner.finalize ();
  REQUIRE ((inlined).has_value ());
  CHECK (inlined.value ().transformations == 0);
  CHECK (calls ((caller).value ()) == 1);
  roundtrip (context);
}
TEST_CASE ("native inline preserves source handles and owns all cloned nodes independently") {
  for (unsigned n = 0; n <= static_cast<unsigned> (test::CallExample::Inline); ++n) {
    CAPTURE (n);
    auto source = test::call_example (static_cast<test::CallExample> (n));
    auto original = dump (*source);
    auto &source_function = *source->modules ().begin ()->functions ().begin ();
    auto identity = &*source_function.blocks ().begin ()->instructions ().begin ();
    auto copy = clone (*source);
    REQUIRE ((copy).has_value ());
    bool inline_started = false;
    std::size_t simplified = 0;
    optimize ((copy).value (), [&] (const transforms::PassEvent &event) -> Result<void> {
      if (event.pass == "simplify" && event.phase == transforms::PassEvent::Phase::After) {
        CHECK_FALSE (inline_started);
        ++simplified;
      }
      if (event.pass == "inline") {
        inline_started = true;
        CHECK (simplified == 2);
      }
      return {};
    });
    CHECK (dump (*source) == original);
    CHECK (identity == &*source_function.blocks ().begin ()->instructions ().begin ());
    auto &entry = *std::prev (copy.value ().modules ().begin ()->functions ().end ());
    bool forbidden = n == unsigned (test::CallExample::Indirect)
                     || n == unsigned (test::CallExample::Variadic)
                     || n == unsigned (test::CallExample::VariadicBlock);
    CHECK (calls (entry) == unsigned (forbidden));
    source.reset ();
    REQUIRE ((copy.value ().verify ()).has_value ());
    roundtrip ((copy).value ());
    transforms::FunctionPassManager repeat;
    repeat.add<transforms::InlinePass> ();
    REQUIRE ((repeat.run ((copy).value ())).has_value ());
    roundtrip ((copy).value ());
  }
}
TEST_CASE ("native inline roundtrips all threshold frame cold and relocation fixtures") {
  unsigned n = 0;
  for (auto &text : test::inline_fixtures ()) {
    CAPTURE (n);
    ++n;
    auto context = serialization::parse_text (text);
    INFO (((context).has_value () ? "" : context.error ().message));
    REQUIRE ((context).has_value ());
    auto copy = clone ((context).value ());
    REQUIRE ((copy).has_value ());
    auto original = dump ((context).value ());
    optimize ((copy).value ());
    CHECK (dump ((context).value ()) == original);
    (context).value () = mirnext::ir::Program{};
    roundtrip ((copy).value ());
    for (auto &module : copy.value ().modules ())
      for (auto &function : module.functions ()) {
        auto graph = analysis::ControlFlowAnalysis::run (function);
        REQUIRE ((graph).has_value ());
        for (auto &block : function.blocks ())
          for (auto &instruction : block.instructions ()) {
            for (auto &operand : instruction.operands ()) {
              if (auto reg = mirnext::ir::detail::operand_access (operand).get<const Register *> ())
                CHECK (&(*reg)->function () == &function);
              if (auto label = mirnext::ir::detail::operand_access (operand).get<const Label *> ())
                CHECK (&(*label)->function () == &function);
              if (auto symbol
                  = mirnext::ir::detail::operand_access (operand).get<SymbolReference> ())
                CHECK (&mirnext::ir::detail::NodeAccess::symbol (*(symbol))->module () == &module);
            }
          }
      }
  }
}
TEST_CASE ("native inline keeps hardware identity across assignment and nested calls") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("hardware");
  REQUIRE ((m).has_value ());
  auto leaf = mirnext::ir::detail::access (((m)).value ()).create_function ("leaf", {});
  REQUIRE ((leaf).has_value ());
  auto b = mirnext::ir::detail::access (((leaf)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  auto bound
    = mirnext::ir::detail::access (((leaf)).value ()).create_register (Type::i64 (), "bound", "r5");
  REQUIRE ((bound).has_value ());
  InstructionBuilder callee ((b).value ());
  REQUIRE ((callee.assign ((bound).value (), 0)).has_value ());
  REQUIRE ((callee.ret ()).has_value ());
  auto mid = mirnext::ir::detail::access (((m)).value ()).create_function ("mid", {});
  REQUIRE ((mid).has_value ());
  b = mirnext::ir::detail::access (((mid)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  InstructionBuilder middle ((b).value ());
  REQUIRE ((middle.call ((leaf).value ())).has_value ());
  REQUIRE ((middle.call ((leaf).value ())).has_value ());
  REQUIRE ((middle.ret ()).has_value ());
  auto entry
    = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((entry).has_value ());
  b = mirnext::ir::detail::access (((entry)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  auto caller_bound = mirnext::ir::detail::access (((entry)).value ())
                        .create_register (Type::i64 (), "caller_bound", "r5");
  REQUIRE ((caller_bound).has_value ());
  InstructionBuilder caller ((b).value ());
  auto assign = caller.assign ((caller_bound).value (), 42);
  REQUIRE ((assign).has_value ());
  REQUIRE ((caller.call ((mid).value ())).has_value ());
  auto ret = caller.ret (Operand ((caller_bound).value ()));
  REQUIRE ((ret).has_value ());
  auto assignment_identity = &(assign).value (), return_identity = &(ret).value ();
  optimize (context);
  CHECK (&(assign).value () == assignment_identity);
  CHECK (&(ret).value () == return_identity);
  CHECK (entry.value ().find_register ("caller_bound").get () == &(caller_bound).value ());
  unsigned bindings = 0, overwrites = 0;
  for (auto &r : entry.value ().registers ()) bindings += !r.hardware_binding ().empty ();
  CHECK (bindings == 1);
  for (auto &block : entry.value ().blocks ())
    for (auto &i : block.instructions ())
      if (&i != assignment_identity && i.opcode () == Opcode::Mov
          && i.operands ()[0] == Operand ((caller_bound).value ()))
        ++overwrites;
  CHECK (overwrites == 3);
  CHECK (calls ((entry).value ()) == 0);
  roundtrip (context);
}
TEST_CASE ("native inline errors name the function and stage and reset scoped state") {
  auto context = test::call_example (test::CallExample::Direct);
  transforms::FunctionPassManager passes;
  passes.add<transforms::InlinePass> ();
  auto result = passes.run (*context);
  CHECK_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("[inline]") != std::string::npos);
  CHECK (result.error ().message.find ("calls/") != std::string::npos);
  optimize (*context);
  REQUIRE ((passes.run (*context)).has_value ());
  auto second = test::call_example (test::CallExample::Direct);
  transforms::FunctionPassManager frontend;
  frontend.add<transforms::SimplifyPass> ();
  frontend.add<transforms::InlinePass> ();
  result = frontend.run (*second, [] (const transforms::PassEvent &event) -> Result<void> {
    if (event.pass == "inline") return Error{ErrorCode::InvalidArgument, "observer failure"};
    return {};
  });
  CHECK_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("[inline]") != std::string::npos);
  REQUIRE ((frontend.run (*second)).has_value ());
}
TEST_CASE ("native block layout moves preserve labels instructions storage and CFG revisions") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("layout");
  REQUIRE ((m).has_value ());
  auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
  REQUIRE ((f).has_value ());
  auto first = mirnext::ir::detail::access (((f)).value ()).create_block ("first"),
       last = mirnext::ir::detail::access (((f)).value ()).create_block ("last");
  REQUIRE ((first).has_value ());
  REQUIRE ((last).has_value ());
  auto label = mirnext::ir::detail::access (((f)).value ()).create_label ((last).value ());
  REQUIRE ((label).has_value ());
  auto jump = InstructionBuilder ((first).value ()).jump ((label).value ());
  REQUIRE ((jump).has_value ());
  auto ret = InstructionBuilder ((last).value ()).ret ();
  REQUIRE ((ret).has_value ());
  auto storage = ret.value ().operands ().data ();
  InstructionRewriter rewriter ((f).value ());
  auto before = f.value ().revisions ();
  REQUIRE ((rewriter.move_block_before ((last).value (), &(first).value ())).has_value ());
  CHECK (&*f.value ().blocks ().begin () == &(last).value ());
  CHECK (&ret.value ().block () == &(last).value ());
  CHECK (ret.value ().operands ().data () == storage);
  CHECK (&label.value ().block () == &(last).value ());
  CHECK (f.value ().revisions ().control_flow > before.control_flow);
  REQUIRE ((rewriter.move_block_before ((first).value (), &(last).value ())).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  auto other = mirnext::ir::detail::access (((m)).value ()).create_function ("other", {});
  REQUIRE ((other).has_value ());
  auto foreign = mirnext::ir::detail::access (((other)).value ()).create_block ();
  REQUIRE ((foreign).has_value ());
  CHECK_FALSE ((rewriter.move_block_before ((last).value (), &(foreign).value ())).has_value ());
}
TEST_CASE ("native inline reports frame growth overflow before encoding") {
  auto context = serialization::parse_text (
    "m:module\np:proto i64\nleaf:func i64\nlocal i64:a, i64:r\nalloca a, 9223372036854775792\nmov "
    "i64:(a), 42\nmov r, i64:(a)\nret r\nendfunc\nentry:func i64\nlocal i64:a, i64:r\nalloca a, "
    "32\nmov i64:(a), 0\ncall p, leaf, r\nret r\nendfunc\nendmodule\n");
  REQUIRE ((context).has_value ());
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::InlinePass> ();
  auto result = passes.run ((context).value ());
  CHECK_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("entry [inline]") != std::string::npos);
  CHECK (result.error ().message.find ("frame growth overflow") != std::string::npos);
}
TEST_CASE ("native rewriter transfers owned operands and validates before consuming them") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("transfer");
  REQUIRE ((m).has_value ());
  auto f = mirnext::ir::detail::access (((m)).value ()).create_function ("entry", {});
  REQUIRE ((f).has_value ());
  auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto r = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ());
  REQUIRE ((r).has_value ());
  InstructionRewriter rewriter ((f).value ());
  std::vector<Operand> operands{Operand ((r).value ()), Operand (42)};
  auto storage = operands.data ();
  auto instruction = rewriter.insert ((block).value (), nullptr, Opcode::Mov, std::move (operands));
  REQUIRE ((instruction).has_value ());
  CHECK (instruction.value ().operands ().data () == storage);
  CHECK (instruction.value ().operands ()[1] == Operand (42));
  std::vector<Operand> invalid{Operand ((r).value ())};
  storage = invalid.data ();
  auto result = rewriter.insert ((block).value (), nullptr, Opcode::Mov, std::move (invalid));
  CHECK_FALSE ((result).has_value ());
  CHECK (invalid.data () == storage);
  CHECK (invalid.size () == 1);
  CHECK (block.value ().size () == 1);
  REQUIRE ((InstructionBuilder ((block).value ()).ret ()).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
