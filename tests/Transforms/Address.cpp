/* Licensed under the MIT License. Address lowering uses only the native object model. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "AddressFixtures.hpp"
#include <unordered_set>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
std::unique_ptr<Context> parse(std::string_view text) {
  auto result = serialization::parse_text(text); INFO((result ? "" : result.error().message)); REQUIRE(result); return std::move(*result);
}
Function &entry(Context &context) { return const_cast<Function &>(*context.modules().begin()->find_symbol("entry")->function()); }
void lower(Context &context) {
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(false);
  passes.add<AddressTransformPass>(); auto result = passes.run(context);
  INFO((result ? "" : result.error().message)); REQUIRE(result);
}
}
TEST_CASE("address families lower through native SSA and preserve surviving instruction identities") {
  auto inputs = test::address_fixtures(); auto random = test::random_address_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  auto consumers = test::address_memory_consumers(); inputs.insert(inputs.end(), consumers.begin(), consumers.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto context = parse(text); auto &f = entry(*context);
    std::unordered_set<const Instruction *> identities;
    FunctionPassManager prepare; prepare.add<SimplifyPass>(); prepare.add<PrepareCFGPass>(); prepare.add<BuildSSAPass>(false);
    REQUIRE(prepare.run(*context));
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() != Opcode::Addr && i.opcode() != Opcode::Addr8
        && i.opcode() != Opcode::Addr16 && i.opcode() != Opcode::Addr32) identities.insert(&i);
    FunctionPassManager pass; pass.add<AddressTransformPass>(); REQUIRE(pass.run(*context));
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) identities.erase(&i);
    CHECK(identities.empty()); REQUIRE(context->verify()); CHECK(f.ssa_form() == nullptr);
    auto copied = clone(*context); REQUIRE(copied); CHECK(dump(**copied) == dump(*context));
    auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
    FunctionPassManager ssa; ssa.add<BuildSSAPass>(); ssa.add<ConventionalSSAPass>(); ssa.add<DestroySSAPass>();
    REQUIRE(ssa.run(*context)); REQUIRE(context->verify());
  }
}
TEST_CASE("narrow memory uses select every extension while scalar addresses vanish") {
  auto inputs = test::address_fixtures();
  const Opcode extensions[]{Opcode::Ext8, Opcode::UExt8, Opcode::Ext16, Opcode::UExt16, Opcode::Ext32, Opcode::UExt32, Opcode::Mov, Opcode::Mov};
  for (unsigned n = 0; n < 8; ++n) {
    auto context = parse(inputs[n]); lower(*context); auto &f = entry(*context); auto v = f.find_register("v"); REQUIRE(v);
    CHECK_FALSE(v->is_memory_backed()); bool extended = false;
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
      CHECK(i.opcode() != Opcode::Addr); CHECK(i.opcode() != Opcode::Addr8); CHECK(i.opcode() != Opcode::Addr16); CHECK(i.opcode() != Opcode::Addr32);
      extended |= i.opcode() == extensions[n];
    }
    CHECK(extended);
  }
}
TEST_CASE("escaping copied storage preserves the separate hardware binding and its return uses") {
  auto inputs = test::address_fixtures(); auto context = parse(inputs.back());
  FunctionPassManager raw; raw.add<BuildSSAPass>(false); raw.add<AddressTransformPass>(); REQUIRE(raw.run(*context)); auto &f = entry(*context);
  auto reg = f.find_register("storage"); REQUIRE(reg); CHECK(reg->is_memory_backed()); CHECK(reg->hardware_binding().empty());
  auto bound = f.find_register("v"); REQUIRE(bound); CHECK_FALSE(bound->is_memory_backed()); CHECK_FALSE(bound->hardware_binding().empty());
  FunctionAnalysisManager analyses(f); FunctionPassManager build; build.add<BuildSSAPass>(); REQUIRE(build.run(f, analyses));
  bool hardware_return = false;
  for (auto &use : f.ssa_form()->uses()) if (use.alive()) hardware_return |= std::holds_alternative<SSAHardwareUse>(use.site());
  CHECK(hardware_return);
  FunctionPassManager exit; exit.add<ConventionalSSAPass>(); exit.add<DestroySSAPass>(); REQUIRE(exit.run(f, analyses));
  auto live = analyses.get<LivenessAnalysis>(); REQUIRE(live);
  auto copied = clone(*context); REQUIRE(copied); CHECK(entry(**copied).find_register("storage")->is_memory_backed());
  CHECK_FALSE(entry(**copied).find_register("v")->is_memory_backed());
  auto before = dump(f); auto ret = const_cast<Instruction *>(std::prev(f.blocks().end())->terminator()); REQUIRE(ret);
  CHECK_FALSE(IRRewriter(f).replace_operand(*ret, 0, Operand(*reg))); CHECK(dump(f) == before); REQUIRE(f.verify());
}
TEST_CASE("partial stores retain storage when any address exposes a wider integer view") {
  for (auto [type,kind] : {std::pair{"i8",Type::Kind::I8}, {"u8",Type::Kind::U8}, {"i16",Type::Kind::I16},
      {"u16",Type::Kind::U16}, {"i32",Type::Kind::I32}, {"u32",Type::Kind::U32}}) for (bool separate_view : {false,true}) for (bool direct : {false,true}) {
    CAPTURE(type); CAPTURE(separate_view); CAPTURE(direct);
    auto narrow_address = std::string_view(type).ends_with("8") ? "addr8" : std::string_view(type).ends_with("16") ? "addr16" : "addr32";
    auto context = parse("m:module\nentry:func i64\nlocal i64:v,p:p,p:q,i64:r\nmov v,1311768467463790320\n"
      +std::string(separate_view ? narrow_address : "addr")+" p,v\n"+(direct ? "" : "addr q,v\n")+"mov "+type
      +":(p),1\n"+(direct ? "mov r,v\n" : "mov r,i64:(q)\n")+"ret r\nendfunc\nendmodule\n");
    lower(*context); auto &f = entry(*context); auto value = f.find_register("v"); REQUIRE(value); CHECK(value->is_memory_backed());
    bool partial_store = false;
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.operands().size() && i.operands()[0].get<Memory>())
      partial_store |= i.operands()[0].get<Memory>()->type == Type(kind);
    CHECK(partial_store); REQUIRE(context->verify());
  }
}
TEST_CASE("address pass diagnoses phase and owner errors atomically and can repeat after rebuild") {
  auto inputs = test::address_fixtures(); auto context = parse(inputs[8]); auto other = parse(inputs[0]);
  auto &f = entry(*context), &foreign = entry(*other); FunctionAnalysisManager analyses(f), foreign_analyses(foreign);
  AddressTransformPass pass; auto before = dump(f); CHECK_FALSE(pass.run(f, analyses)); CHECK(dump(f) == before);
  CHECK_FALSE(pass.run(f, foreign_analyses)); CHECK(dump(f) == before);
  FunctionPassManager prepare; prepare.add<SimplifyPass>(); prepare.add<PrepareCFGPass>(); prepare.add<BuildSSAPass>(); REQUIRE(prepare.run(f, analyses));
  before = dump(f); CHECK_FALSE(pass.run(f, analyses)); CHECK(dump(f) == before);
  FunctionPassManager finish; finish.add<ConventionalSSAPass>(); finish.add<DestroySSAPass>(); REQUIRE(finish.run(f, analyses));
  FunctionPassManager lower_pass; lower_pass.add<BuildSSAPass>(false); lower_pass.add<AddressTransformPass>();
  REQUIRE(lower_pass.run(f, analyses)); before = dump(f); REQUIRE(lower_pass.run(f, analyses)); CHECK(dump(f) == before);
  auto reg = foreign.find_register("v"); REQUIRE(reg);
  CHECK_FALSE(IRRewriter(f).materialize_register_storage(*reg)); CHECK(dump(f) == before);
  auto target = foreign.find_register("r"); REQUIRE(target); before = dump(foreign);
  CHECK_FALSE(IRRewriter(foreign).materialize_register_storage(*target)); CHECK(dump(foreign) == before);
}
TEST_CASE("non scalar address consumers retain storage instead of substituting incompatible registers") {
  for (auto &text : test::address_memory_consumers()) {
    CAPTURE(text); auto context = parse(text); lower(*context); auto &f = entry(*context);
    auto v = f.find_register("v"); REQUIRE(v); CHECK(v->is_memory_backed());
    std::size_t addresses = 0; for (auto &b : f.blocks()) for (auto &i : b.instructions()) addresses += i.opcode() == Opcode::Addr;
    CHECK(addresses >= 2); REQUIRE(context->verify());
  }
}
TEST_CASE("noncanonical escaping memory indexes report an error before consuming SSA") {
  auto context = parse(R"(m: module
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 7
mov q, 0
addr p, v
mov r, i64:0(q, v, 1)
ret p
endfunc
endmodule
)");
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); FunctionPassManager ssa; ssa.add<BuildSSAPass>(false);
  REQUIRE(ssa.run(f, analyses)); auto before = dump(f); auto form = f.ssa_form();
  auto result = AddressTransformPass().run(f, analyses); REQUIRE_FALSE(result);
  CHECK(result.error().message.find("simplify") != std::string::npos); CHECK(f.ssa_form() == form); CHECK(dump(f) == before); REQUIRE(f.verify());
}
TEST_CASE("unsimplified memory address results fail before consuming SSA") {
  auto context = parse("m: module\nentry: func i64\nlocal i64:v, i64:p\nmov v, 7\nmov p, 0\naddr i64:0(p), v\nret v\nendfunc\nendmodule\n");
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); FunctionPassManager ssa; ssa.add<BuildSSAPass>(false);
  REQUIRE(ssa.run(f, analyses)); auto before = dump(f); auto form = f.ssa_form();
  auto result = AddressTransformPass().run(f, analyses); REQUIRE_FALSE(result);
  CHECK(result.error().message.find("simplify") != std::string::npos); CHECK(f.ssa_form() == form); CHECK(dump(f) == before); REQUIRE(f.verify());
}
TEST_CASE("nested address definitions rename through PHIs while storage facts survive SSA exit") {
  auto inputs = test::address_fixtures(); auto context = parse(inputs[inputs.size() - 2]); lower(*context);
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); FunctionPassManager build; build.add<BuildSSAPass>(); REQUIRE(build.run(f, analyses));
  bool pointer_phi = false;
  for (auto &phi : f.ssa_form()->phis()) if (phi.alive() && phi.output().origin().name() == "p") {
    pointer_phi = true; CHECK(&phi.output().reg() != f.find_register("p"));
    for (auto use : phi.output().uses()) if (auto site = std::get_if<SSAInstructionUse>(&use->site()); site && site->instruction->opcode() == Opcode::Addr)
      CHECK(*site->instruction->operands()[site->operand].get<const Register *>() == &phi.output().reg());
  }
  CHECK(pointer_phi); FunctionPassManager exit; exit.add<ConventionalSSAPass>(); exit.add<DestroySSAPass>(); REQUIRE(exit.run(f, analyses));
  for (auto &block : f.blocks()) for (auto &instruction : block.instructions()) if (instruction.opcode() == Opcode::Addr)
    CHECK((*instruction.operands()[1].get<const Register *>())->is_memory_backed());
  REQUIRE(f.verify());
}
TEST_CASE("addresses assigned after their pointer slot escapes remain observable") {
  for (bool direct : {false, true}) {
  auto context = parse(test::stored_pointer_address_fixture(direct)); lower(*context); auto &f = entry(*context);
  CHECK(f.find_register("p")->is_memory_backed()); CHECK(f.find_register("v")->is_memory_backed());
  bool retained = false; std::unordered_set<const Register *> slots, values;
  for (auto &block : f.blocks()) for (auto &instruction : block.instructions()) if (instruction.opcode() == Opcode::Addr) {
    auto source = *instruction.operands()[1].get<const Register *>();
    if (source == f.find_register("p")) slots.insert(*instruction.operands()[0].get<const Register *>());
    if (source == f.find_register("v")) values.insert(*instruction.operands()[0].get<const Register *>());
  }
  for (auto &block : f.blocks()) for (auto &instruction : block.instructions()) if (instruction.opcode() == Opcode::Mov)
    if (auto memory = instruction.operands()[0].get<Memory>(); memory && slots.contains(memory->base))
      if (auto value = instruction.operands()[1].get<const Register *>()) retained |= values.contains(*value);
  CHECK(retained); FunctionPassManager ssa; ssa.add<BuildSSAPass>(); ssa.add<ConventionalSSAPass>(); ssa.add<DestroySSAPass>();
  REQUIRE(ssa.run(*context)); REQUIRE(context->verify());
  }
  for (auto &text : test::pointer_alias_address_fixtures()) {
    auto context = parse(text); lower(*context); FunctionPassManager ssa;
    ssa.add<BuildSSAPass>(); ssa.add<ConventionalSSAPass>(); ssa.add<DestroySSAPass>(); REQUIRE(ssa.run(*context));
    REQUIRE(context->verify()); auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
  }
}
