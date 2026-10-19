/* Licensed under the MIT License. Native pass semantics and object lifetime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../IR/Expressions.hpp"
#include "../IR/Calls.hpp"
#include "../IR/Variables.hpp"
#include "../IR/StackScopes.hpp"
#include "SimplifyFixtures.hpp"
#include <cmath>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
void simplify(Context &context) {
  for (auto &module : context.modules()) for (auto &function : module.functions()) {
    analysis::FunctionAnalysisManager analyses(function); transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>();
    auto result = passes.run(function, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
  }
  auto valid = context.verify(); INFO((valid ? "" : valid.error().message)); REQUIRE(valid);
}
std::unique_ptr<Context> parse(std::string_view text) {
  auto parsed = serialization::parse_text(text); INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed); return std::move(*parsed);
}
void roundtrip(const Context &context) {
  auto original = dump(context); auto copied = clone(context); INFO((copied ? "" : copied.error().message)); REQUIRE(copied); CHECK(dump(**copied) == original);
  auto text = serialization::write_text(context); INFO((text ? "" : text.error().message)); REQUIRE(text);
  auto parsed = serialization::parse_text(*text); INFO((parsed ? "" : parsed.error().message)); REQUIRE(parsed);
  auto second = serialization::write_text(**parsed); REQUIRE(second); CHECK(*second == *text);
  auto binary = serialization::write_binary(context); REQUIRE(binary); auto decoded = serialization::read_binary(*binary); REQUIRE(decoded);
  CHECK(dump(context) == original); REQUIRE((*decoded)->verify());
}
}
TEST_CASE("native simplify handles every public instruction and independent cloning") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor); simplify(*context); roundtrip(*context);
  }
}
TEST_CASE("native simplify handles expression call global and scope fixtures") {
  for (unsigned n = 0; n <= static_cast<unsigned>(test::ExpressionExample::Initializer); ++n) {
    CAPTURE(n); auto context = test::expression_example(static_cast<test::ExpressionExample>(n)); simplify(*context); roundtrip(*context);
  }
  for (unsigned n = 0; n <= static_cast<unsigned>(test::CallExample::Inline); ++n) {
    CAPTURE(n); auto context = test::call_example(static_cast<test::CallExample>(n)); simplify(*context); roundtrip(*context);
  }
  for (unsigned n = 0; n <= static_cast<unsigned>(test::VariableExample::Narrow); ++n) {
    CAPTURE(n); auto context = test::variable_example(static_cast<test::VariableExample>(n)); simplify(*context); roundtrip(*context);
  }
  for (auto mode : {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto context = test::stack_scope_example(mode); simplify(*context); CHECK_FALSE(needs_materialization(*context)); roundtrip(*context);
  }
}
TEST_CASE("native simplify verifies and roundtrips all focused rule fixtures") {
  unsigned n = 0;
  for (auto &text : test::simplify_fixtures()) { CAPTURE(n); ++n; auto context = parse(text); simplify(*context); roundtrip(*context); }
}
TEST_CASE("native simplify isolates hardware bindings without changing their identity") {
  Context context; auto module = context.create_module("hardware"); REQUIRE(module);
  auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); REQUIRE(function);
  auto block = (*function)->create_block(); REQUIRE(block); auto bound = (*function)->create_register(Type::i64(), "bound", "r5"); REQUIRE(bound);
  auto pointer = (*function)->create_register(Type::pointer(), "pointer", "r6"); REQUIRE(pointer);
  IRBuilder builder(**block); REQUIRE(builder.assign(**bound, 40));
  REQUIRE(builder.emit(Opcode::Add, {Operand(**bound), Operand(**bound), Operand(2)}));
  REQUIRE(builder.store(Memory{Type::i64(), *pointer, *pointer, 8, 4}, Operand(**bound)));
  auto copy = (*function)->create_register(Type::i64(), "copy"); REQUIRE(copy);
  REQUIRE(builder.assign(**copy, Operand(**bound)));
  REQUIRE(builder.address_of(**copy)); REQUIRE(builder.ret(Operand(**bound))); simplify(context);
  CHECK((*function)->find_register("bound") == *bound); CHECK((*function)->find_register("pointer") == *pointer);
  unsigned bindings = 0;
  for (auto &reg : (*function)->registers()) bindings += !reg.hardware_binding().empty(); CHECK(bindings == 2);
  bool saw_address = false;
  for (auto &b : (*function)->blocks()) {
    const Instruction *previous = nullptr;
    for (auto &i : b.instructions()) {
      if (i.opcode() == Opcode::Add) for (auto &o : i.operands()) if (auto r = o.get<const Register *>()) CHECK((*r)->hardware_binding().empty());
      if (i.opcode() == Opcode::Addr) {
        saw_address = true; auto proxy = i.operands()[1].get<const Register *>(); REQUIRE(proxy);
        CHECK((*proxy)->hardware_binding().empty()); REQUIRE(previous); REQUIRE(previous->opcode() == Opcode::Mov);
        CHECK(*previous->operands()[0].get<const Register *>() == *proxy);
        CHECK(*previous->operands()[1].get<const Register *>() == *bound);
      }
      for (auto &o : i.operands()) if (auto m = o.get<Memory>()) { CHECK(m->base->hardware_binding().empty()); CHECK_FALSE(m->index); }
      previous = &i;
    }
  }
  CHECK(saw_address);
  roundtrip(context);
}
TEST_CASE("native simplify preserves unsigned immediate identity and does not overapply algebraic rules") {
  for (auto opcode : {Opcode::Add, Opcode::Mul, Opcode::UDiv, Opcode::And, Opcode::UMulo}) {
    Context context; auto m = context.create_module("unsigned"); REQUIRE(m);
    auto f = (*m)->create_function("entry", {{Type::i64()}, {}}); REQUIRE(f); auto b = (*f)->create_block(); REQUIRE(b);
    auto r = (*f)->create_register(Type::i64()); REQUIRE(r); IRBuilder builder(**b);
    auto instruction = builder.emit(opcode, {Operand(**r), Operand(42), Operand(std::uint64_t(opcode == Opcode::Add ? 0 : 1))}); REQUIRE(instruction);
    REQUIRE(builder.ret(Operand(**r))); simplify(context); CHECK((*instruction)->opcode() == opcode);
    bool found = false; for (auto &i : (*b)->instructions()) for (auto &o : i.operands()) found |= o.get<std::uint64_t>() != nullptr;
    CHECK(found); roundtrip(context);
  }
}
TEST_CASE("native literal pools preserve string bytes float bits and collision-free ordered names") {
  Context context; auto m = context.create_module("pools"); REQUIRE(m);
  REQUIRE((*m)->create_data(".literal0", ZeroData{8})); REQUIRE((*m)->import(".literal1"));
  for (auto text : {std::string(""), std::string("abc"), std::string("abc\0",4), std::string("a\0b",3)}) {
    auto data = (*m)->create_literal(Operand(text)); REQUIRE(data); auto &v = std::get<InitializedData>((*data)->initializer()).values;
    CHECK(v.size() == text.size() + (text.empty() || text.back() != '\0')); CHECK(*v.back().get<std::int64_t>() == 0);
    for (std::size_t n = 0; n < text.size(); ++n) CHECK(*v[n].get<std::int64_t>() == static_cast<unsigned char>(text[n]));
    CHECK((*data)->is_literal_pool()); CHECK_FALSE((*data)->writable()); CHECK((*data)->symbol().name() != ".literal0"); CHECK((*data)->symbol().name() != ".literal1");
  }
  auto literal = (*m)->create_literal(Operand(-0.0)); REQUIRE(literal);
  auto &value = std::get<InitializedData>((*literal)->initializer()).values.front(); CHECK(std::signbit(*value.get<double>()));
  auto count = (*m)->items().size(); CHECK_FALSE((*m)->create_literal(Operand(42))); CHECK((*m)->items().size() == count); CHECK_FALSE((*m)->error());
  auto copied = clone(context); REQUIRE(copied); REQUIRE((*copied)->verify());
}
TEST_CASE("native simplify preserves real instruction identity and invalidates cached CFG") {
  auto context = parse("m:module\nf:func i64\nlocal i64:x\nadd x, 20, 22\nret x\nendfunc\nendmodule\n");
  auto &f = *context->modules().begin()->functions().begin(); auto &add = *f.blocks().begin()->instructions().begin();
  auto identity = &add; analysis::FunctionAnalysisManager analyses(f); REQUIRE(analyses.get<analysis::ControlFlowAnalysis>());
  auto before = f.revisions(); transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>();
  REQUIRE(passes.run(f, analyses)); CHECK(&add == identity); CHECK(add.opcode() == Opcode::Add);
  CHECK(add.operands()[1].get<const Register *>()); CHECK(add.operands()[2].get<const Register *>());
  CHECK(f.revisions().instructions > before.instructions); REQUIRE(context->verify());
  REQUIRE(passes.run(f, analyses)); REQUIRE(context->verify()); roundtrip(*context);
}
TEST_CASE("native simplify materializes float initializer literals with checked provenance") {
  auto context = parse("m:module\nvalue:expr f\nf:func d\nlocal d:x\ndadd x, 20.5, 21.5\nret x\nendfunc\nendmodule\n");
  simplify(*context); CHECK(needs_materialization(*context)); auto &f = *context->modules().begin()->functions().begin();
  Instruction *load = nullptr; const Memory *memory = nullptr;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() == Opcode::DMov && i.operands()[1].get<Memory>()) {
    load = &i; memory = i.operands()[1].get<Memory>(); break;
  }
  REQUIRE(load); REQUIRE(memory); REQUIRE(memory->literal_source()); roundtrip(*context);
  auto copied = clone(*context); REQUIRE(copied); context.reset(); REQUIRE((*copied)->verify()); roundtrip(**copied);
  auto &copy_function = *(*copied)->modules().begin()->functions().begin();
  for (auto &b : copy_function.blocks()) for (auto &i : b.instructions())
    if (i.opcode() == Opcode::DMov && i.operands()[1].get<Memory>()) { load = &i; memory = i.operands()[1].get<Memory>(); break; }
  REQUIRE(memory); auto address = memory->base; IRRewriter rewriter(copy_function);
  REQUIRE(rewriter.insert_before(*load, Opcode::Mov, {Operand(*address), Operand(0)}));
  auto invalid = (*copied)->verify(); REQUIRE_FALSE(invalid); CHECK(invalid.error().message.find("verified literal pools") != std::string::npos);
}
TEST_CASE("native simplify keeps scalar memory alias metadata and local addresses") {
  auto context = parse("m:module\nf:func i64\nlocal i64:x, i64:p, i64:i\nmov x, 42\naddr p, x\nadd x, i64:8(p,i,4), 2\nret x\nendfunc\nendmodule\n");
  auto &f = *context->modules().begin()->functions().begin();
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() == Opcode::Add) {
    auto memory = *i.operands()[1].get<Memory>(); memory.alias = "a"; memory.nonalias = "b";
    REQUIRE(IRRewriter(f).replace_operand(i, 1, Operand(memory)));
  }
  simplify(*context); bool found = false, address = false;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
    if (i.opcode() == Opcode::Addr) { address = true; CHECK((*i.operands()[1].get<const Register *>())->name() == "x"); }
    for (auto &o : i.operands()) if (auto m = o.get<Memory>()) { found = true; CHECK(m->alias == "a"); CHECK(m->nonalias == "b"); CHECK(m->base); CHECK_FALSE(m->index); CHECK(m->displacement == 0); }
  }
  CHECK(found); CHECK(address); roundtrip(*context);
}
TEST_CASE("native simplify returns alignment overflow diagnostics before entering a runtime") {
  for (auto text : {"m:module\nf:func\nlocal i64:p\nalloca p, 9223372036854775807\nret\nendfunc\nendmodule\n",
      "m:module\nf:func\nlocal i64:p, i64:q\nalloca p, 9223372036854775792\nalloca q, 16\nret\nendfunc\nendmodule\n"}) {
    auto context = parse(text); auto &f = *context->modules().begin()->functions().begin();
    analysis::FunctionAnalysisManager analyses(f); transforms::FunctionPassManager passes; passes.add<transforms::SimplifyPass>();
    auto result = passes.run(f, analyses); REQUIRE_FALSE(result); CHECK(result.error().message.find("m/f [simplify]") != std::string::npos);
    CHECK(result.error().message.find("overflows") != std::string::npos);
  }
}
TEST_CASE("literal provenance rejects tampering and foreign owners atomically") {
  Context context; auto module = context.create_module("pool"); REQUIRE(module);
  auto function = (*module)->create_function("entry", {{Type(Type::Kind::F64)}, {}}); REQUIRE(function);
  auto block = (*function)->create_block(); REQUIRE(block);
  auto address = (*function)->create_register(Type::i64()); REQUIRE(address);
  auto result = (*function)->create_register(Type(Type::Kind::F64)); REQUIRE(result);
  auto pool = (*module)->create_literal(Operand(42.0)); REQUIRE(pool); IRRewriter rewriter(**function);
  auto memory = rewriter.literal_memory(**pool,**address); REQUIRE(memory); IRBuilder builder(**block);
  REQUIRE(builder.assign(**address,Operand((*pool)->symbol())));
  auto load = builder.emit(Opcode::DMov,{Operand(**result),Operand(*memory)}); REQUIRE(load); REQUIRE(builder.ret(Operand(**result)));
  for (unsigned n = 0; n < 3; ++n) {
    auto altered = *memory;
    if (n == 0) altered.displacement = 8;
    if (n == 1) altered.index = *address;
    if (n == 2) altered.type = Type::i64();
    auto before = dump(context); CHECK_FALSE(rewriter.replace_operand(**load,1,Operand(altered))); CHECK(dump(context) == before);
  }
  const Operand store[]{Operand(*memory),Operand(**result)};
  CHECK_FALSE(rewriter.replace(**load,Opcode::DMov,store));
  auto other = (*module)->create_function("other", {}); REQUIRE(other); auto other_address = (*other)->create_register(Type::i64()); REQUIRE(other_address);
  CHECK_FALSE(rewriter.literal_memory(**pool,**other_address));
  auto other_block = (*other)->create_block(); REQUIRE(other_block); REQUIRE(IRBuilder(**other_block).ret());
  auto writable = (*module)->create_data("writable",InitializedData{Type(Type::Kind::F64),{Operand(42.0)}}); REQUIRE(writable);
  CHECK_FALSE(rewriter.literal_memory(**writable,**address)); REQUIRE(context.verify());
}
TEST_CASE("multiplication identities retain flags used through intervening moves") {
  auto context = parse("m:module\nf:func i64\nlocal i64:x, i64:y\nmulo x, 42, 1\nmov y, x\nbo L\nret y\nL:\nret 0\nendfunc\nendmodule\n");
  simplify(*context); bool producer = false;
  for (auto &block : context->modules().begin()->functions().begin()->blocks()) for (auto &instruction : block.instructions()) producer |= instruction.opcode() == Opcode::Mulo;
  CHECK(producer); roundtrip(*context);
}
TEST_CASE("already simplified operands preserve storage revisions and cached analyses") {
  auto context = parse("m:module\nf:func i64\nlocal i64:x\nmov x, 42\nret x\nendfunc\nendmodule\n");
  auto &function = *context->modules().begin()->functions().begin();
  auto &instruction = *function.blocks().begin()->instructions().begin(); auto storage = instruction.operands().data();
  auto revisions = function.revisions(); analysis::FunctionAnalysisManager analyses(function);
  auto cfg = analyses.get<analysis::ControlFlowAnalysis>(); REQUIRE(cfg); auto identity = *cfg;
  auto result = transforms::SimplifyPass().run(function,analyses); REQUIRE(result); CHECK(result->transformations == 0);
  CHECK(function.revisions() == revisions); CHECK(instruction.operands().data() == storage);
  analyses.invalidate(result->preserved); auto cached = analyses.get<analysis::ControlFlowAnalysis>(); REQUIRE(cached); CHECK(*cached == identity);
  roundtrip(*context);
}
