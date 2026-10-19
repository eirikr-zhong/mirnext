/* Licensed under the MIT License. Native block cloning tests without the C runtime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "CloningFixtures.hpp"
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
void prepare(Function &function, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); REQUIRE(passes.run(function, analyses));
}
}
TEST_CASE("block cloning preserves surviving identities and native SSA and format lifecycle") {
  auto inputs = test::cloning_fixtures(); auto random = test::random_cloning_fixtures(); inputs.insert(inputs.end(), random.begin(), random.end());
  auto controls = test::cloning_control_fixtures(); inputs.insert(inputs.end(), controls.begin(), controls.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto context = parse(text); auto &function = entry(*context); FunctionAnalysisManager analyses(function); prepare(function, analyses);
    std::unordered_set<const Instruction *> identities;
    for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) if (instruction.opcode() != Opcode::Jmp) identities.insert(&instruction);
    auto result = BlockCloningPass().run(function, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
    for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) identities.erase(&instruction);
    CHECK(identities.empty()); REQUIRE(function.verify());
    auto cloned = clone(*context); REQUIRE(cloned); CHECK(dump(**cloned) == dump(*context));
    auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
    FunctionPassManager ssa; ssa.add<BuildSSAPass>(); ssa.add<ConventionalSSAPass>(); ssa.add<DestroySSAPass>(); REQUIRE(ssa.run(function, analyses)); REQUIRE(context->verify());
  }
}
TEST_CASE("returns switches and absent return boundaries prohibit cloning") {
  for (auto terminator : {"ret v", "jret v", "switch c, done, done"}) {
    bool jump_return = std::string_view(terminator) == "jret v";
    auto text = "m: module\nentry: func" + std::string(jump_return ? "" : " i64") + "\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\n"
      + std::string(terminator) + "\ndone:\n" + (jump_return ? "jret v" : "ret v") + "\ncold:\nmov v, 10\njmp hot\nendfunc\nendmodule\n";
    auto context = parse(text); auto &f = entry(*context); FunctionAnalysisManager analyses(f);
    FunctionPassManager cfg; cfg.add<PrepareCFGPass>(); REQUIRE(cfg.run(f, analyses));
    auto before = dump(f); auto result = BlockCloningPass().run(f, analyses); REQUIRE(result);
    CHECK(result->transformations == 0); CHECK(dump(f) == before);
  }
  auto context = parse("m: module\nentry: func\nloop:\njmp loop\nendfunc\nendmodule\n");
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses); auto before = dump(f);
  auto result = BlockCloningPass().run(f, analyses); REQUIRE(result); CHECK(result->transformations == 0); CHECK(dump(f) == before);
}
TEST_CASE("property and indirect terminators keep valid explicit CFG edges after cloning") {
  for (auto &text : test::cloning_control_fixtures()) {
    auto context = parse(text); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
    auto result = BlockCloningPass().run(f, analyses); REQUIRE(result); CHECK(result->transformations == 1);
    auto cfg = analyses.get<ControlFlowAnalysis>(); REQUIRE(cfg); REQUIRE(f.verify());
    unsigned branches = 0;
    for (auto &block : f.blocks()) if (auto tail = block.terminator(); tail && (tail->opcode() == Opcode::PrBeq || tail->opcode() == Opcode::PrBne)) {
      auto b = (*cfg)->indices.at(&block); CHECK((*cfg)->outgoing[b].size() == 2); ++branches;
    }
    CHECK(branches == (text.find("jmpi") == std::string::npos ? 2 : 0));
  }
}
TEST_CASE("cloning diagnoses wrong owner and active SSA before changing the body") {
  auto inputs = test::cloning_fixtures(); auto context = parse(inputs[0]), other = parse(inputs[0]);
  auto &f = entry(*context); FunctionAnalysisManager analyses(f), foreign(entry(*other)); prepare(f, analyses);
  auto before = dump(f); CHECK_FALSE(BlockCloningPass().run(f, foreign)); CHECK(dump(f) == before);
  FunctionPassManager ssa; ssa.add<BuildSSAPass>(); REQUIRE(ssa.run(f, analyses)); before = dump(f);
  CHECK_FALSE(BlockCloningPass().run(f, analyses)); CHECK(dump(f) == before); REQUIRE(f.ssa_form()->verify());
}
TEST_CASE("initial DFS back edges are not cloned") {
  auto context = parse(test::cloning_fixtures().back()); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
  auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph); auto identity = *graph;
  auto before = dump(f); auto result = BlockCloningPass().run(f, analyses); REQUIRE(result); CHECK(result->transformations == 0); CHECK(dump(f) == before);
  CHECK(*analyses.get<FunctionGraphAnalysis>() == identity);
}
TEST_CASE("recursive anchor hot checks can self clone without invalidating borrowed instructions") {
  for (bool extra : {false, true}) {
  std::string text = R"(m: module
entry: func i64
local i64:v
jmp cold
hot:
jmp cold
done:
ret v
cold:
mov v, 42
jmp hot
endfunc
anchor: lref done
endmodule
)";
  if (extra) {
    text.insert(text.find("endfunc"), "cold_extra:\nmov v, 0\njmp hot\n");
    text.insert(text.find("endmodule"), "extra_ref: lref cold_extra\n");
  }
  auto context = parse(text);
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); FunctionPassManager cfg; cfg.add<PrepareCFGPass>(); REQUIRE(cfg.run(f, analyses));
  auto result = BlockCloningPass().run(f, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
  if (extra) CHECK(result->transformations > 1); else CHECK(result->transformations == 1);
  CHECK(f.instruction_count() < 32); REQUIRE(f.verify());
  auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
  }
}
TEST_CASE("the strict growth threshold permits a second clone at nine but prohibits it at ten") {
  auto fixtures = test::cloning_fixtures();
  for (unsigned padding : {6, 7}) {
    auto context = parse(fixtures[7 + padding - 4]); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
    auto result = BlockCloningPass().run(f, analyses); REQUIRE(result);
    CHECK(result->transformations == (padding == 6 ? 3 : 2)); REQUIRE(f.verify());
  }
}
