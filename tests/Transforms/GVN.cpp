/* Licensed under the MIT License. Canonical value numbering without the C runtime. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "GVNFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include <bit>
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
void prepare(Function &f, FunctionAnalysisManager &analyses, bool rename = true) {
  FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>(rename);
  auto result = passes.run(f, analyses); INFO((result ? "" : result.error().message)); REQUIRE(result);
}
void finish(Function &f, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes; passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>(); REQUIRE(passes.run(f, analyses));
}
}
TEST_CASE("GVN edits canonical SSA and produces independent public IR without a runtime") {
  auto inputs = test::gvn_fixtures(); auto rules = test::gvn_rule_fixtures(); auto random = test::random_gvn_fixtures();
  inputs.insert(inputs.end(), rules.begin(), rules.end()); inputs.insert(inputs.end(), random.begin(), random.end());
  for (auto &text : inputs) {
    CAPTURE(text); auto source = parse(text); auto original = dump(*source); auto context = clone(*source); REQUIRE(context);
    std::unordered_set<const Instruction *> identities;
    auto &f = entry(**context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) identities.insert(&i);
    FunctionPassManager passes; passes.add<GVNPass>(); std::size_t observations = 0;
    REQUIRE(passes.run(f, analyses, [&](const PassEvent &event) -> Result<void> {
      if (event.phase == PassEvent::Phase::After) {
        ++observations; REQUIRE(event.function.ssa_form()->verify());
        REQUIRE(analyses.get<GVNAnalysis>()); auto graph = analyses.get<FunctionGraphAnalysis>(); REQUIRE(graph);
        std::size_t edges = 0;
        for (auto &outgoing : (*graph)->outgoing) for (auto id : outgoing) {
          REQUIRE(id < (*graph)->edges.size()); auto &edge = (*graph)->edges[id];
          CHECK(std::ranges::count((*graph)->incoming[edge.destination], id) == 1); ++edges;
        }
        CHECK(edges == (*graph)->edges.size());
      }
      return {};
    })); CHECK(observations == 1); CHECK(dump(*source) == original); REQUIRE(f.verify());
    // Nonterminating computations keep their original canonical identity.
    std::size_t survivors = 0;
    for (auto &b : f.blocks()) for (auto &i : b.instructions()) survivors += identities.contains(&i);
    CHECK(survivors > 0); CHECK_FALSE(serialization::write_binary(**context));
    finish(f, analyses); CHECK_FALSE(analyses.get<GVNAnalysis>()); source.reset(); REQUIRE((*context)->verify());
    auto encoded = serialization::write_binary(**context); REQUIRE(encoded); REQUIRE(serialization::read_binary(*encoded));
    auto text_result = serialization::write_text(**context); REQUIRE(text_result); REQUIRE(serialization::parse_text(*text_result));
    auto independent = clone(**context); REQUIRE(independent); context->reset(); REQUIRE((*independent)->verify());
  }
}
TEST_CASE("GVN keeps overflow producers and rejects illegal phases atomically") {
  auto context = parse(test::gvn_fixtures()[8]), other = parse(test::gvn_fixtures()[8]);
  auto &f = entry(*context); FunctionAnalysisManager analyses(f), foreign(entry(*other));
  auto before = dump(f); CHECK_FALSE(GVNPass().run(f, analyses)); CHECK(dump(f) == before);
  prepare(f, analyses); before = dump(f); CHECK_FALSE(GVNPass().run(f, foreign)); CHECK(dump(f) == before);
  FunctionPassManager passes; passes.add<GVNPass>();
  auto failed = passes.run(f, analyses, [](const PassEvent &event) -> Result<void> {
    if (event.phase == PassEvent::Phase::After) return Error{ErrorCode::InvalidArgument, "observer declined"}; return {};
  }); REQUIRE_FALSE(failed); CHECK(failed.error().message.find("m/entry [gvn]") != std::string::npos);
  REQUIRE(f.verify()); bool overflow_live = false;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() == Opcode::Addo) {
    auto definitions = f.ssa_form()->definitions(i); REQUIRE(definitions.size() == 1); overflow_live |= !definitions[0]->uses().empty();
  }
  CHECK(overflow_live); finish(f, analyses); prepare(f, analyses, false); before = dump(f);
  CHECK_FALSE(GVNPass().run(f, analyses)); CHECK(dump(f) == before);
  auto conventional = parse(test::gvn_fixtures()[1]); auto &g = entry(*conventional); FunctionAnalysisManager g_analyses(g); prepare(g, g_analyses);
  FunctionPassManager conventional_pass; conventional_pass.add<ConventionalSSAPass>(); REQUIRE(conventional_pass.run(g, g_analyses)); before = dump(g);
  CHECK_FALSE(GVNPass().run(g, g_analyses)); CHECK(dump(g) == before);
}
TEST_CASE("GVN can run again on an independent previously optimized result") {
  auto source = parse(test::gvn_fixtures()[4]); auto &f = entry(*source); FunctionAnalysisManager analyses(f);
  prepare(f, analyses); FunctionPassManager passes; passes.add<GVNPass>(); REQUIRE(passes.run(f, analyses)); finish(f, analyses);
  auto context = clone(*source); REQUIRE(context); source.reset(); auto &g = entry(**context); FunctionAnalysisManager g_analyses(g);
  prepare(g, g_analyses); REQUIRE(passes.run(g, g_analyses)); finish(g, g_analyses);
  auto binary = serialization::write_binary(**context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
}
TEST_CASE("SSA opcode replacement preserves definitions and validates all new inputs before editing") {
  auto context = parse(test::gvn_fixtures()[0]), other = parse(test::gvn_fixtures()[0]);
  auto &f = entry(*context), &foreign = entry(*other); FunctionAnalysisManager analyses(f), other_analyses(foreign);
  prepare(f, analyses); prepare(foreign, other_analyses); auto form = f.ssa_form(); auto rewriter = SSARewriter::get(f); REQUIRE(rewriter);
  Instruction *target = nullptr;
  for (auto &b : f.blocks()) for (auto &i : b.instructions()) if (i.opcode() == Opcode::Add && !target) target = &i;
  REQUIRE(target); auto definitions = form->definitions(*target); auto output = definitions[0];
  auto input = const_cast<SSAValue *>(&form->inputs(*target)[0]->value());
  Operand operands[]{target->operands()[0], Operand(input->reg())}; SSAValue *values[]{input};
  auto before = dump(f); auto bad = const_cast<SSAValue *>(&*foreign.ssa_form()->values().begin()); SSAValue *bad_values[]{bad};
  CHECK_FALSE(rewriter->replace(*target, Opcode::Mov, operands, bad_values)); CHECK_FALSE(rewriter->replace(*target, Opcode::Mov, operands, {}));
  CHECK(dump(f) == before); REQUIRE(form->verify());
  REQUIRE(rewriter->replace(*target, Opcode::Mov, operands, values)); CHECK(form->definitions(*target)[0] == output);
  CHECK(form->inputs(*target).size() == 1); REQUIRE(form->verify());
  auto reg = rewriter->fresh_register(Type::i64()); REQUIRE(reg); operands[0] = Operand(**reg); before = dump(f);
  CHECK_FALSE(rewriter->replace(*target, Opcode::Mov, operands, values)); CHECK(dump(f) == before); REQUIRE(form->verify());
  CHECK_FALSE(rewriter->fresh_register(Type::block(16))); finish(f, analyses);
}
TEST_CASE("GVN rejects nonmaterialized scalar memory operands before editing SSA") {
  auto context = parse("m: module\nentry: func i64, i64:p\nlocal i64:value\nmov value, i64:8(p)\nret value\nendfunc\nendmodule\n");
  auto &f = entry(*context); FunctionAnalysisManager analyses(f); FunctionPassManager ssa; ssa.add<BuildSSAPass>(); REQUIRE(ssa.run(f, analyses));
  auto before = dump(f); auto result = GVNPass().run(f, analyses); REQUIRE_FALSE(result);
  CHECK(result.error().message.find("materialize memory") != std::string::npos); CHECK(dump(f) == before); REQUIRE(f.verify());
}
TEST_CASE("every public opcode passes native GVN and binary encoding with the C oracle disabled") {
  for (auto &descriptor : instruction_descriptors()) {
    CAPTURE(descriptor.name); auto context = test::public_instruction_example(descriptor);
    FunctionPassManager passes; passes.add<SimplifyPass>(); passes.add<PrepareCFGPass>(); passes.add<BuildSSAPass>();
    passes.add<GVNPass>(); passes.add<ConventionalSSAPass>(); passes.add<DestroySSAPass>();
    auto result = passes.run(*context); INFO((result ? "" : result.error().message)); REQUIRE(result);
    REQUIRE(context->verify()); auto binary = serialization::write_binary(*context); REQUIRE(binary); REQUIRE(serialization::read_binary(*binary));
  }
}
TEST_CASE("GVN tracks stack provenance memory classes and full pointer escape") {
  auto rules = test::gvn_rule_fixtures(); bool escape = false, stack = false, aliases = false;
  for (auto &text : rules) {
    auto context = parse(text); auto &f = entry(*context); FunctionAnalysisManager analyses(f); prepare(f, analyses);
    auto result = GVNPass().run(f, analyses); REQUIRE(result); auto info = analyses.get<GVNAnalysis>(); REQUIRE(info);
    escape |= (*info)->full_escape;
    for (auto &location : (*info)->memory) { stack |= location.alloca_flags != 0; aliases |= !location.alias.empty(); }
    for (auto &[instruction, slot] : (*info)->locations) { REQUIRE(slot > 0); REQUIRE(slot < (*info)->memory.size()); CHECK(instruction->function().symbol().name() == "entry"); }
    finish(f, analyses);
  }
  CHECK(escape); CHECK(stack); CHECK(aliases);
}
TEST_CASE("integer folding preserves width rules and avoids undefined C arithmetic") {
  auto bits = [](std::int64_t value) { return std::bit_cast<std::uint64_t>(value); };
  CHECK(fold_integer(Opcode::Add, UINT64_MAX, 1) == 0); CHECK(fold_integer(Opcode::Sub, 0, 1) == UINT64_MAX);
  CHECK(fold_integer(Opcode::Neg, bits(INT64_MIN)) == bits(INT64_MIN));
  CHECK(fold_integer(Opcode::Ext8, 0x80) == bits(-128)); CHECK(fold_integer(Opcode::Ext16, 0x8000) == bits(-32768));
  CHECK(fold_integer(Opcode::Ext32, 0x80000000) == bits(INT32_MIN)); CHECK(fold_integer(Opcode::UExt32, UINT64_MAX) == UINT32_MAX);
  CHECK(fold_integer(Opcode::Adds, INT32_MAX, 1) == bits(INT32_MIN)); CHECK(fold_integer(Opcode::UMulos, 0x80000000, 1) == 0x80000000);
  CHECK(fold_integer(Opcode::Mulos, 0x80000000, 1) == bits(INT32_MIN));
  CHECK(fold_integer(Opcode::Rsh, bits(-8), 2) == bits(-2)); CHECK(fold_integer(Opcode::URsh, bits(-8), 2) == 0x3ffffffffffffffe);
  CHECK(fold_integer(Opcode::Rshs, 0xfffffff8, 2) == bits(-2)); CHECK(fold_integer(Opcode::URshs, 0xfffffff8, 2) == 0x3ffffffe);
  CHECK(fold_integer(Opcode::Lt, UINT64_MAX, 0) == 1); CHECK(fold_integer(Opcode::ULt, UINT64_MAX, 0) == 0);
  CHECK(fold_integer(Opcode::Bts, 0x100000000) == 0); CHECK(fold_integer(Opcode::Bfs, 0x100000000) == 1);
  for (auto op : {Opcode::Div, Opcode::Mod, Opcode::UDiv, Opcode::UMod, Opcode::Divs, Opcode::Mods, Opcode::UDivs, Opcode::UMods}) CHECK_FALSE(fold_integer(op, 42, 0));
  for (auto op : {Opcode::Div, Opcode::Mod}) CHECK_FALSE(fold_integer(op, bits(INT64_MIN), UINT64_MAX));
  for (auto op : {Opcode::Divs, Opcode::Mods}) CHECK_FALSE(fold_integer(op, 0x80000000, UINT32_MAX));
  for (auto op : {Opcode::Lsh, Opcode::Rsh, Opcode::URsh}) { CHECK_FALSE(fold_integer(op, 42, 64)); CHECK_FALSE(fold_integer(op, 42, UINT64_MAX)); }
  for (auto op : {Opcode::Lshs, Opcode::Rshs, Opcode::URshs}) CHECK_FALSE(fold_integer(op, 42, 32));
  CHECK_FALSE(fold_integer(Opcode::FAdd, 42, 5)); CHECK_FALSE(fold_integer(Opcode::Add, {}, 5)); CHECK_FALSE(fold_integer(Opcode::Add, 42, {}));
}
