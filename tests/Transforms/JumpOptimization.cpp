/* Licensed under the MIT License. Canonical post-SSA jump and identity regressions. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/JumpOptimization.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "JumpFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::analysis; using namespace mirnext::transforms;
namespace {
std::unique_ptr<Context> parse(std::string_view text) {
  auto c = serialization::parse_text(text); INFO((c ? "" : c.error().message)); REQUIRE(c); return std::move(*c);
}
Function &entry(Context &c) { return const_cast<Function &>(*c.modules().begin()->find_symbol("entry")->function()); }
void prepare(Function &f, FunctionAnalysisManager &a, bool prune = true) {
  FunctionPassManager p; p.add<SimplifyPass>(); p.add<PrepareCFGPass>(prune); auto checked = p.run(f, a);
  INFO((checked ? "" : checked.error().message)); REQUIRE(checked);
}
void formats(Context &c) {
  REQUIRE(c.verify()); auto binary = serialization::write_binary(c); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
  auto text = serialization::write_text(c); REQUIRE(text); REQUIRE(serialization::parse_text(*text));
}
}
TEST_CASE("jump optimization removes direct fallthrough branches with stable surviving identities") {
  auto fixtures = test::jump_fixtures();
  for (auto &text : fixtures) if (text.find(" done,") != text.npos && text.find("ret 0") == text.npos && text.find("jmp") == text.npos) {
    CAPTURE(text); auto c = parse(text); auto &f = entry(*c); FunctionAnalysisManager a(f);
    FunctionPassManager p; p.add<PrepareCFGPass>(); REQUIRE(p.run(f, a));
    const Instruction *ret = nullptr; const BasicBlock *ret_block = nullptr; std::size_t branches = 0;
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) {
      if (i.opcode() == Opcode::Ret) { ret = &i; ret_block = &b; }
      if (instruction_descriptor(i.opcode())->flow == ControlFlow::Branch) ++branches;
    }
    REQUIRE(branches == 1); auto result = JumpOptimizationPass().run(f, a); INFO((result ? "" : result.error().message)); REQUIRE(result);
    CHECK(ret->opcode() == Opcode::Ret); CHECK(&ret->block() == ret_block);
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) CHECK(instruction_descriptor(i.opcode())->flow != ControlFlow::Branch);
    auto graph = a.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto cfg = a.get<ControlFlowAnalysis>(); REQUIRE(cfg);
    formats(*c);
  }
}
TEST_CASE("jump optimization preserves forbidden property switch and indirect targets") {
  for (auto &text : test::jump_fixtures()) if (text.find("prbe") != text.npos || text.find("prbn") != text.npos || text.find("switch") != text.npos || text.find("jmpi") != text.npos) {
    CAPTURE(text); auto c = parse(text); auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a);
    std::vector<std::pair<const Instruction *, const Label *>> targets;
    for (auto &b : f.blocks()) for (auto &i : b.instructions())
      if (i.opcode() == Opcode::Switch || i.opcode() == Opcode::PrBeq || i.opcode() == Opcode::PrBne || i.opcode() == Opcode::LAddr)
        for (auto &operand : i.operands()) if (auto label = operand.get<const Label *>()) targets.emplace_back(&i, *label);
    auto result = JumpOptimizationPass().run(f, a); INFO((result ? "" : result.error().message)); REQUIRE(result);
    for (auto [i, label] : targets) { bool retained = false; for (auto &operand : i->operands()) if (auto l = operand.get<const Label *>()) retained |= *l == label; CHECK(retained); }
    formats(*c);
  }
}
TEST_CASE("jump optimization preserves owned results formats and repeated independent analyses") {
  auto inputs = test::jump_fixtures(); auto random = test::random_jump_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto source = parse(text); auto original = dump(*source); auto c = clone(*source); REQUIRE(c);
    auto &f = entry(**c); FunctionAnalysisManager a(f); prepare(f, a, false); auto checked = JumpOptimizationPass().run(f, a);
    INFO((checked ? "" : checked.error().message)); REQUIRE(checked); CHECK(dump(*source) == original); source.reset();
    FunctionAnalysisManager fresh(f); REQUIRE(JumpOptimizationPass().run(f, fresh)); formats(**c);
  }
}
TEST_CASE("jump optimization retains historical address roots without artificial entry reachability") {
  auto c = parse("m: module\nentry: func i64\nlocal i64:p\nladdr p, kept\nret 42\ndead:\nret 9\nkept:\nret 13\nendfunc\nendmodule\n");
  auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a, false);
  auto graph = a.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto saved = a.take_cached<FunctionGraphAnalysis>(); REQUIRE(saved);
  const BasicBlock *kept = nullptr; Instruction *address = nullptr;
  for (auto &b : f.blocks()) { for (auto label : b.labels()) if (label->name() == "kept") kept = &b; for (auto &i : b.instructions()) if (i.opcode() == Opcode::LAddr) address = &i; }
  REQUIRE(kept); REQUIRE(address); CHECK((**saved).blocks[(**saved).indices.at(kept)].address_taken);
  REQUIRE(IRRewriter(f).erase(*address)); REQUIRE(a.set<FunctionGraphAnalysis>(std::move(**saved)));
  REQUIRE(JumpOptimizationPass().run(f, a)); graph = a.get<FunctionGraphAnalysis>(); REQUIRE(graph); CHECK((*graph)->indices.contains(kept));
  for (auto &b : f.blocks()) for (auto l : b.labels()) CHECK(l->name() != "dead"); formats(*c);
}
TEST_CASE("jump optimization rejects SSA foreign ownership and unseparated anchors before edits") {
  auto c = parse(test::jump_fixtures()[1]), other = parse(test::jump_fixtures()[0]); auto &f = entry(*c);
  FunctionAnalysisManager a(f), foreign(entry(*other)); auto before = dump(f);
  CHECK_FALSE(JumpOptimizationPass().run(f, foreign)); CHECK_FALSE(JumpOptimizationPass().run(f, a)); CHECK(dump(f) == before);
  prepare(f, a); FunctionPassManager p; p.add<BuildSSAPass>(); REQUIRE(p.run(f, a)); before = dump_ssa(f);
  CHECK_FALSE(JumpOptimizationPass().run(f, a)); CHECK(dump_ssa(f) == before);
  FunctionPassManager finish; finish.add<ConventionalSSAPass>(); finish.add<DestroySSAPass>(); REQUIRE(finish.run(f, a)); REQUIRE(JumpOptimizationPass().run(f, a)); formats(*c);
}
TEST_CASE("jump optimization handles long adjacent label chains without recursive traversal") {
  std::ostringstream text; text << "m: module\nentry: func i64\njmp label0\n";
  for (unsigned n = 0; n < 4096; ++n) text << "label" << n << ":\n";
  text << "ret 42\nendfunc\nendmodule\n"; auto c = parse(text.str()); auto &f = entry(*c); FunctionAnalysisManager a(f);
  FunctionPassManager p; p.add<PrepareCFGPass>(); REQUIRE(p.run(f, a)); CHECK(std::distance(f.blocks().begin(), f.blocks().end()) >= 4096);
  auto result = JumpOptimizationPass().run(f, a); INFO((result ? "" : result.error().message)); REQUIRE(result); CHECK(f.instruction_count() == 1); formats(*c);
}
TEST_CASE("jump optimization builds verifies and encodes every public opcode") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto c = test::public_instruction_example(descriptor); auto &f = entry(*c); FunctionAnalysisManager a(f); prepare(f, a);
    auto checked = JumpOptimizationPass().run(f, a); INFO((checked ? "" : checked.error().message)); REQUIRE(checked); formats(*c);
  }
}
