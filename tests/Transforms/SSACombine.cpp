/* Licensed under the MIT License. Canonical combine and target-predicate regressions. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/SSACombine.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "CombineFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include <limits>
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::analysis; using namespace mirnext::transforms;
namespace {
std::unique_ptr<Context> parse(std::string_view text) {
  auto result = serialization::parse_text(text); INFO((result ? "" : result.error().message)); REQUIRE(result); return std::move(*result);
}
Function &entry(Context &c) { return const_cast<Function &>(*c.modules().begin()->find_symbol("entry")->function()); }
void prepare(Function &f, FunctionAnalysisManager &a) {
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(); passes.add<ConventionalSSAPass>();
  auto status = passes.run(f, a); INFO((status ? "" : status.error().message)); REQUIRE(status);
}
void finish(Context &c) {
  FunctionPassManager passes; passes.add<DestroySSAPass>(); REQUIRE(passes.run(c)); REQUIRE(c.verify());
  auto binary = serialization::write_binary(c); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
  auto text = serialization::write_text(c); REQUIRE(text); REQUIRE(serialization::parse_text(*text));
}
}
TEST_CASE("SSA combine covers all integer and floating comparison directions") {
  for (auto &text : test::combine_fixtures()) if (text.find(" yes, c") != text.npos) {
    CAPTURE(text); auto c = parse(text); auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a);
    auto before = f.instruction_count(); auto result = SSACombinePass().run(f, a); INFO((result ? "" : result.error().message)); REQUIRE(result);
    bool floating = text.find(" c, x, y") != text.npos && text.find(" i64:x") == text.npos;
    bool prohibited = floating && text.find("bf yes") != text.npos;
    CHECK((f.instruction_count() < before) == !prohibited); REQUIRE(f.verify()); finish(*c);
  }
}
TEST_CASE("SSA combine target rules preserve backend boundary decisions") {
  auto x86 = target::AddressingRules::get(target::Architecture::X86_64); auto arm = target::AddressingRules::get(target::Architecture::AArch64); REQUIRE(x86); REQUIRE(arm);
  CHECK(x86->accepts(Type::i64(), INT32_MAX, true, 8)); CHECK(x86->accepts(Type::i64(), INT32_MIN, false, 99));
  CHECK_FALSE(x86->accepts(Type::i64(), std::int64_t(INT32_MAX) + 1, false, 1)); CHECK_FALSE(x86->accepts(Type::i64(), 0, true, 3));
  CHECK(arm->accepts(Type::i64(), 0, true, 3)); CHECK_FALSE(arm->accepts(Type::i64(), 0, true, 8));
  CHECK(arm->accepts(Type::i64(), 12285, false, 1)); CHECK_FALSE(arm->accepts(Type::i64(), 12288, false, 1));
  CHECK(arm->accepts(Type::i64(), 3, false, 1)); CHECK_FALSE(arm->accepts(Type::i64(), 8, false, 1));
  CHECK(arm->accepts(Type(Type::Kind::Bool), 3, false, 1)); CHECK(arm->accepts(Type(Type::Kind::Bool), 0, true, 3));
  CHECK(arm->accepts(Type(Type::Kind::U8), 0, true, 1)); CHECK_FALSE(arm->accepts(Type(Type::Kind::U8), 1, false, 1));
  CHECK_FALSE(arm->accepts(Type(Type::Kind::Undef), 0, false, 1)); CHECK_FALSE(target::AddressingRules::get(static_cast<target::Architecture>(99)));
}
TEST_CASE("SSA combine preserves independent ownership and formats on both targets") {
  auto fixtures = test::combine_fixtures(); auto random = test::random_combine_fixtures(); fixtures.insert(fixtures.end(), random.begin(), random.end());
  for (auto &text : fixtures) for (auto architecture : {target::Architecture::X86_64, target::Architecture::AArch64}) {
    CAPTURE(text); auto source = parse(text); auto before = dump(*source); auto c = clone(*source); REQUIRE(c);
    auto &f = entry(**c); FunctionAnalysisManager a(f); prepare(f, a); auto result = SSACombinePass(architecture).run(f, a);
    INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(dump(*source) == before); source.reset(); REQUIRE(f.verify()); finish(**c);
  }
}
TEST_CASE("SSA memory rewriting preserves unrelated input identities and fails atomically") {
  auto c = parse("m: module\nentry: func i64, i64:p, i64:x\nmov i64:0(p):a:b, x\nret x\nendfunc\nendmodule\n");
  auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a); auto rw = SSARewriter::get(f); REQUIRE(rw);
  Instruction *store = nullptr; for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.operands()[0].get<Memory>()) store = &i;
  REQUIRE(store); auto form = f.ssa_form(); SSAUse *unrelated = nullptr; SSAValue *base = nullptr;
  for (auto use : form->inputs(*store)) if (std::get<SSAInstructionUse>(use->site()).operand == 1) unrelated = use; else base = const_cast<SSAValue *>(&use->value());
  REQUIRE(base); REQUIRE(unrelated); auto memory = *store->operands()[0].get<Memory>(); memory.index = memory.base; memory.scale = 3;
  auto before = dump_ssa(f); CHECK_FALSE(rw->replace_memory(*store, 0, memory, base, nullptr)); CHECK(dump_ssa(f) == before);
  auto invalid = memory; invalid.scale = 256; CHECK_FALSE(rw->replace_memory(*store, 0, invalid, base, base)); CHECK(dump_ssa(f) == before);
  auto foreign = parse("m: module\nentry: func i64, i64:p\nret p\nendfunc\nendmodule\n");
  auto &other = entry(*foreign); FunctionAnalysisManager other_analyses(other); prepare(other, other_analyses);
  SSAValue *foreign_value = nullptr; for (auto &value : other.ssa_form()->values()) if (value.alive()) foreign_value = const_cast<SSAValue *>(&value);
  REQUIRE(foreign_value); invalid = memory; invalid.base = &foreign_value->reg();
  CHECK_FALSE(rw->replace_memory(*store, 0, invalid, foreign_value, base)); CHECK(dump_ssa(f) == before);
  REQUIRE(rw->replace_memory(*store, 0, memory, base, base)); CHECK(unrelated->alive()); CHECK(form->inputs(*store).front() == unrelated);
  CHECK(base->uses().front() == form->inputs(*store).back()); CHECK(store->operands()[0].get<Memory>()->alias == "a"); REQUIRE(f.verify());
  before = dump_ssa(f); CHECK_FALSE(SSACombinePass().run(f, a)); CHECK(dump_ssa(f) == before); finish(*c);
}
TEST_CASE("SSA combine rejects unsupported targets phases owners and indexed input before mutation") {
  auto text = test::combine_fixtures()[0]; auto c = parse(text), other = parse(text); auto &f = entry(*c); FunctionAnalysisManager a(f), foreign(entry(*other));
  auto before = dump(f); CHECK_FALSE(SSACombinePass().run(f, a)); CHECK(dump(f) == before); prepare(f, a); before = dump_ssa(f);
  CHECK_FALSE(SSACombinePass().run(f, foreign)); CHECK_FALSE(SSACombinePass(static_cast<target::Architecture>(99)).run(f, a)); CHECK(dump_ssa(f) == before);
  REQUIRE(SSACombinePass().run(f, a)); finish(*c);
}
TEST_CASE("SSA combine constructs verifies and encodes every public opcode") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto c = test::public_instruction_example(descriptor); auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a);
    auto result = SSACombinePass().run(f, a); INFO((result ? "" : result.error().message)); REQUIRE(result); finish(*c);
  }
}
TEST_CASE("SSA combine preserves hardware address identity and diagnoses malformed definition cycles") {
  auto inputs = test::combine_fixtures(); auto c = parse(inputs[inputs.size() - 2]); auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a);
  REQUIRE(SSACombinePass().run(f, a)); bool bound = false;
  for (auto &block : f.blocks()) for (auto &i : block.instructions()) for (auto &operand : i.operands())
    if (auto memory = operand.get<Memory>(); memory && memory->base) bound |= !memory->base->hardware_binding().empty();
  CHECK(bound); finish(*c);
  c = parse("m: module\nentry: func i64, i64:p\nlocal i64:a, i64:r\nadd a, p, 3\nmov r, i64:0(a)\nret r\nendfunc\nendmodule\n");
  auto &cycle = entry(*c); FunctionAnalysisManager ca(cycle); prepare(cycle, ca); auto form = cycle.ssa_form(); auto rw = SSARewriter::get(cycle); REQUIRE(rw);
  for (auto &block : cycle.blocks()) for (auto &i : block.instructions()) if (i.opcode() == Opcode::Add) {
    auto value = form->definitions(i).front(); SSAUse *source = nullptr;
    for (auto use : form->inputs(i)) if (std::get<SSAInstructionUse>(use->site()).operand == 1) source = use;
    REQUIRE(source); REQUIRE(rw->retarget(*source, *value));
  }
  auto before = dump_ssa(cycle); auto result = SSACombinePass().run(cycle, ca); REQUIRE_FALSE(result);
  CHECK(result.error().message.find("cyclic address definition") != std::string::npos); CHECK(dump_ssa(cycle) == before);
}
TEST_CASE("SSA combine retains shared comparison values and surviving branch identities") {
  auto c = parse("m: module\nentry: func i64, i64:x, i64:y\nlocal i64:c, i64:r\ngt c, x, y\nbt yes, c\nret c\nyes:\nadd r, c, 7\nret r\nendfunc\nendmodule\n");
  auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a); const Instruction *compare = nullptr, *branch = nullptr;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) { if (i.opcode() == Opcode::Gt) compare = &i; if (i.opcode() == Opcode::Bt) branch = &i; }
  REQUIRE(compare); REQUIRE(branch); auto block = &branch->block(); REQUIRE(SSACombinePass().run(f, a));
  CHECK(compare->opcode() == Opcode::Gt); CHECK(branch->opcode() == Opcode::Bgt); CHECK(&branch->block() == block); REQUIRE(f.verify()); finish(*c);
}
