/* Licensed under the MIT License. Global value numbering tests. */
#include "mirnext/Transforms/AnalysisAccess.hpp"
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
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
std::unique_ptr<Program> parse (std::string_view text) {
  auto result = serialization::parse_text (text);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::make_unique<Program> (std::move ((result).value ()));
}
Function &entry (Program &context) {
  return const_cast<Function &> (
    *context.modules ().begin ()->find_symbol ("entry").get ()->function ().get ());
}
void prepare (Function &f, FunctionAnalysisManager &analyses, bool rename = true) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> (rename);
  auto result = passes.run (f, analyses);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
}
void finish (Function &f, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes;
  passes.add<ConventionalSSAPass> ();
  passes.add<DestroySSAPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
}
}  // namespace
TEST_CASE ("GVN edits canonical SSA and produces independent public IR without a runtime") {
  auto inputs = test::gvn_fixtures ();
  auto rules = test::gvn_rule_fixtures ();
  auto random = test::random_gvn_fixtures ();
  inputs.insert (inputs.end (), rules.begin (), rules.end ());
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs) {
    CAPTURE (text);
    auto source = parse (text);
    auto original = dump (*source);
    auto context = clone (*source);
    REQUIRE ((context).has_value ());
    std::unordered_set<const Instruction *> identities;
    auto &f = entry ((context).value ());
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    for (auto &b : f.blocks ())
      for (auto &i : b.instructions ()) identities.insert (&i);
    FunctionPassManager passes;
    passes.add<GVNPass> ();
    std::size_t observations = 0;
    REQUIRE (
      (passes.run (f, analyses,
                   [&] (const PassEvent &event) -> Result<void> {
                     if (event.phase == PassEvent::Phase::After) {
                       ++observations;
                       REQUIRE ((event.function.ssa_form ().value ().verify ()).has_value ());
                       auto info = analyses.get<GVNAnalysis> ();
                       REQUIRE ((info).has_value ());
                       for (auto &[value, number] :
                            mirnext::transforms::detail::AnalysisAccess::numbers (
                              (info).value ())) {
                         auto queried = info.value ().number (*value);
                         REQUIRE ((queried).has_value ());
                         CHECK (queried.value ().constant == number.constant);
                         CHECK (queried.value ().value == number.value);
                       }
                       for (auto &[instruction, location] :
                            mirnext::transforms::detail::AnalysisAccess::locations (
                              (info).value ()))
                         CHECK (info.value ().location (*instruction) == location);
                       auto graph = analyses.get<FunctionGraphAnalysis> ();
                       REQUIRE ((graph).has_value ());
                       std::size_t edges = 0;
                       for (auto &outgoing : graph.value ().outgoing)
                         for (auto id : outgoing) {
                           REQUIRE (id < graph.value ().edges.size ());
                           auto &edge = graph.value ().edges[id];
                           CHECK (std::ranges::count (graph.value ().incoming[edge.destination], id)
                                  == 1);
                           ++edges;
                         }
                       CHECK (edges == graph.value ().edges.size ());
                     }
                     return {};
                   }))
        .has_value ());
    CHECK (observations == 1);
    CHECK (dump (*source) == original);
    REQUIRE ((f.verify ()).has_value ());
    // Nonterminating computations keep their original canonical identity.
    std::size_t survivors = 0;
    for (auto &b : f.blocks ())
      for (auto &i : b.instructions ()) survivors += identities.contains (&i);
    CHECK (survivors > 0);
    CHECK_FALSE ((serialization::write_binary ((context).value ())).has_value ());
    finish (f, analyses);
    CHECK_FALSE ((analyses.get<GVNAnalysis> ()).has_value ());
    source.reset ();
    REQUIRE ((context.value ().verify ()).has_value ());
    auto encoded = serialization::write_binary ((context).value ());
    REQUIRE ((encoded).has_value ());
    REQUIRE ((serialization::read_binary ((encoded).value ())).has_value ());
    auto text_result = serialization::write_text ((context).value ());
    REQUIRE ((text_result).has_value ());
    REQUIRE ((serialization::parse_text ((text_result).value ())).has_value ());
    auto independent = clone ((context).value ());
    REQUIRE ((independent).has_value ());
    (context).value () = mirnext::ir::Program{};
    REQUIRE ((independent.value ().verify ()).has_value ());
  }
}
TEST_CASE ("GVN keeps overflow producers and rejects illegal phases atomically") {
  auto context = parse (test::gvn_fixtures ()[8]), other = parse (test::gvn_fixtures ()[8]);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f), foreign (entry (*other));
  auto before = dump (f);
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (GVNPass (), f, analyses)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, analyses);
  before = dump (f);
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (GVNPass (), f, foreign)).has_value ());
  CHECK (dump (f) == before);
  FunctionPassManager passes;
  passes.add<GVNPass> ();
  auto failed = passes.run (f, analyses, [] (const PassEvent &event) -> Result<void> {
    if (event.phase == PassEvent::Phase::After)
      return Error{ErrorCode::InvalidArgument, "observer declined"};
    return {};
  });
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("m/entry [gvn]") != std::string::npos);
  REQUIRE ((f.verify ()).has_value ());
  bool overflow_live = false;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Addo) {
        auto definitions
          = mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->definitions (i));
        REQUIRE (definitions.size () == 1);
        overflow_live
          |= !mirnext::ir::detail::ReferenceAccess::pointers (definitions[0]->uses ()).empty ();
      }
  CHECK (overflow_live);
  finish (f, analyses);
  prepare (f, analyses, false);
  before = dump (f);
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (GVNPass (), f, analyses)).has_value ());
  CHECK (dump (f) == before);
  auto conventional = parse (test::gvn_fixtures ()[1]);
  auto &g = entry (*conventional);
  FunctionAnalysisManager g_analyses (g);
  prepare (g, g_analyses);
  FunctionPassManager conventional_pass;
  conventional_pass.add<ConventionalSSAPass> ();
  REQUIRE ((conventional_pass.run (g, g_analyses)).has_value ());
  before = dump (g);
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (GVNPass (), g, g_analyses)).has_value ());
  CHECK (dump (g) == before);
}
TEST_CASE ("GVN can run again on an independent previously optimized result") {
  auto source = parse (test::gvn_fixtures ()[4]);
  auto &f = entry (*source);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses);
  FunctionPassManager passes;
  passes.add<GVNPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
  finish (f, analyses);
  auto context = clone (*source);
  REQUIRE ((context).has_value ());
  source.reset ();
  auto &g = entry ((context).value ());
  FunctionAnalysisManager g_analyses (g);
  prepare (g, g_analyses);
  REQUIRE ((passes.run (g, g_analyses)).has_value ());
  finish (g, g_analyses);
  auto binary = serialization::write_binary ((context).value ());
  REQUIRE ((binary).has_value ());
  REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
}
TEST_CASE (
  "SSA opcode replacement preserves definitions and validates all new inputs before editing") {
  auto context = parse (test::gvn_fixtures ()[0]), other = parse (test::gvn_fixtures ()[0]);
  auto &f = entry (*context), &foreign = entry (*other);
  FunctionAnalysisManager analyses (f), other_analyses (foreign);
  prepare (f, analyses);
  prepare (foreign, other_analyses);
  auto form = f.ssa_form ().get ();
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  Instruction *target = nullptr;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Add && !target) target = &i;
  REQUIRE (target);
  auto definitions = mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (*target));
  auto output = definitions[0];
  auto input = const_cast<SSAValue *> (
    &mirnext::ir::detail::ReferenceAccess::pointers (form->inputs (*target))[0]->value ());
  Operand operands[]{target->operands ()[0], Operand (input->reg ())};
  SSAValue *values[]{input};
  auto before = dump (f);
  auto bad = const_cast<SSAValue *> (&*foreign.ssa_form ().get ()->values ().begin ());
  SSAValue *bad_values[]{bad};
  CHECK_FALSE (
    (rewriter.value ().replace (*target, Opcode::Mov, operands, bad_values)).has_value ());
  CHECK_FALSE ((rewriter.value ().replace (*target, Opcode::Mov, operands, {})).has_value ());
  CHECK (dump (f) == before);
  REQUIRE ((form->verify ()).has_value ());
  REQUIRE ((rewriter.value ().replace (*target, Opcode::Mov, operands, values)).has_value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (form->definitions (*target))[0] == output);
  CHECK (form->inputs (*target).size () == 1);
  REQUIRE ((form->verify ()).has_value ());
  auto reg = rewriter.value ().fresh_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  operands[0] = Operand ((reg).value ());
  before = dump (f);
  CHECK_FALSE ((rewriter.value ().replace (*target, Opcode::Mov, operands, values)).has_value ());
  CHECK (dump (f) == before);
  REQUIRE ((form->verify ()).has_value ());
  CHECK_FALSE ((rewriter.value ().fresh_register (Type::block (16))).has_value ());
  finish (f, analyses);
}
TEST_CASE ("GVN rejects nonmaterialized scalar memory operands before editing SSA") {
  auto context = parse (
    "m: module\nentry: func i64, i64:p\nlocal i64:value\nmov value, i64:8(p)\nret "
    "value\nendfunc\nendmodule\n");
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  FunctionPassManager ssa;
  ssa.add<BuildSSAPass> ();
  REQUIRE ((ssa.run (f, analyses)).has_value ());
  auto before = dump (f);
  auto result = mirnext::transforms::detail::run_pass (GVNPass (), f, analyses);
  REQUIRE_FALSE ((result).has_value ());
  CHECK (result.error ().message.find ("materialize memory") != std::string::npos);
  CHECK (dump (f) == before);
  REQUIRE ((f.verify ()).has_value ());
}
TEST_CASE ("every public opcode passes GVN and binary encoding") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = test::public_instruction_example (descriptor);
    FunctionPassManager passes;
    passes.add<SimplifyPass> ();
    passes.add<PrepareCFGPass> ();
    passes.add<BuildSSAPass> ();
    passes.add<GVNPass> ();
    passes.add<ConventionalSSAPass> ();
    passes.add<DestroySSAPass> ();
    auto result = passes.run (*context);
    INFO (((result).has_value () ? "" : result.error ().message));
    REQUIRE ((result).has_value ());
    REQUIRE ((context->verify ()).has_value ());
    auto binary = serialization::write_binary (*context);
    REQUIRE ((binary).has_value ());
    REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
  }
}
TEST_CASE ("GVN tracks stack provenance memory classes and full pointer escape") {
  auto rules = test::gvn_rule_fixtures ();
  bool escape = false, stack = false, aliases = false;
  for (auto &text : rules) {
    auto context = parse (text);
    auto &f = entry (*context);
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    auto result = mirnext::transforms::detail::run_pass (GVNPass (), f, analyses);
    REQUIRE ((result).has_value ());
    auto info = analyses.get<GVNAnalysis> ();
    REQUIRE ((info).has_value ());
    escape |= info.value ().full_escape;
    for (auto &location : info.value ().memory) {
      stack |= location.alloca_flags != 0;
      aliases |= !location.alias.empty ();
    }
    for (auto &[instruction, slot] :
         mirnext::transforms::detail::AnalysisAccess::locations (((info)).value ())) {
      REQUIRE (slot > 0);
      REQUIRE (slot < info.value ().memory.size ());
      CHECK (instruction->function ().symbol ().name () == "entry");
    }
    finish (f, analyses);
  }
  CHECK (escape);
  CHECK (stack);
  CHECK (aliases);
}
TEST_CASE ("integer folding preserves width rules and avoids undefined arithmetic") {
  auto bits = [] (std::int64_t value) { return std::bit_cast<std::uint64_t> (value); };
  CHECK (fold_integer (Opcode::Add, UINT64_MAX, 1) == 0);
  CHECK (fold_integer (Opcode::Sub, 0, 1) == UINT64_MAX);
  CHECK (fold_integer (Opcode::Neg, bits (INT64_MIN)) == bits (INT64_MIN));
  CHECK (fold_integer (Opcode::Ext8, 0x80) == bits (-128));
  CHECK (fold_integer (Opcode::Ext16, 0x8000) == bits (-32768));
  CHECK (fold_integer (Opcode::Ext32, 0x80000000) == bits (INT32_MIN));
  CHECK (fold_integer (Opcode::UExt32, UINT64_MAX) == UINT32_MAX);
  CHECK (fold_integer (Opcode::Adds, INT32_MAX, 1) == bits (INT32_MIN));
  CHECK (fold_integer (Opcode::UMulos, 0x80000000, 1) == 0x80000000);
  CHECK (fold_integer (Opcode::Mulos, 0x80000000, 1) == bits (INT32_MIN));
  CHECK (fold_integer (Opcode::Rsh, bits (-8), 2) == bits (-2));
  CHECK (fold_integer (Opcode::URsh, bits (-8), 2) == 0x3ffffffffffffffe);
  CHECK (fold_integer (Opcode::Rshs, 0xfffffff8, 2) == bits (-2));
  CHECK (fold_integer (Opcode::URshs, 0xfffffff8, 2) == 0x3ffffffe);
  CHECK (fold_integer (Opcode::Lt, UINT64_MAX, 0) == 1);
  CHECK (fold_integer (Opcode::ULt, UINT64_MAX, 0) == 0);
  CHECK (fold_integer (Opcode::Bts, 0x100000000) == 0);
  CHECK (fold_integer (Opcode::Bfs, 0x100000000) == 1);
  for (auto op : {Opcode::Div, Opcode::Mod, Opcode::UDiv, Opcode::UMod, Opcode::Divs, Opcode::Mods,
                  Opcode::UDivs, Opcode::UMods})
    CHECK_FALSE (fold_integer (op, 42, 0));
  for (auto op : {Opcode::Div, Opcode::Mod})
    CHECK_FALSE (fold_integer (op, bits (INT64_MIN), UINT64_MAX));
  for (auto op : {Opcode::Divs, Opcode::Mods})
    CHECK_FALSE (fold_integer (op, 0x80000000, UINT32_MAX));
  for (auto op : {Opcode::Lsh, Opcode::Rsh, Opcode::URsh}) {
    CHECK_FALSE (fold_integer (op, 42, 64));
    CHECK_FALSE (fold_integer (op, 42, UINT64_MAX));
  }
  for (auto op : {Opcode::Lshs, Opcode::Rshs, Opcode::URshs})
    CHECK_FALSE (fold_integer (op, 42, 32));
  CHECK_FALSE (fold_integer (Opcode::FAdd, 42, 5));
  CHECK_FALSE (fold_integer (Opcode::Add, {}, 5));
  CHECK_FALSE (fold_integer (Opcode::Add, 42, {}));
}
