/* Licensed under the MIT License. Native copy propagation and memory liveness. */
#include "mirnext/Transforms/AnalysisAccess.hpp"
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/CopyPropagation.hpp"
#include "mirnext/Transforms/DeadStores.hpp"
#include "mirnext/Transforms/GVN.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "MemoryFixtures.hpp"
#include "GVNFixtures.hpp"
#include "../IR/PublicInstructions.hpp"
#include <bit>
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
void prepare (Function &f, FunctionAnalysisManager &analyses, bool gvn = true) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> ();
  if (gvn) passes.add<GVNPass> ();
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
std::size_t stores (const Function &f) {
  std::size_t result = 0;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if ((i.opcode () == Opcode::Mov || i.opcode () == Opcode::DMov || i.opcode () == Opcode::FMov
           || i.opcode () == Opcode::LDMov)
          && mirnext::ir::detail::operand_access (i.operands ()[0]).get<Memory> ())
        ++result;
  return result;
}
}  // namespace
TEST_CASE ("copy and memory passes retain canonical identities metadata and independent lifetime") {
  auto inputs = test::memory_fixtures ();
  auto random = test::random_memory_fixtures ();
  inputs.insert (inputs.end (), random.begin (), random.end ());
  for (auto &text : inputs) {
    CAPTURE (text);
    auto source = parse (text);
    auto before = dump (*source);
    auto copy = clone (*source);
    REQUIRE ((copy).has_value ());
    auto &f = entry ((copy).value ());
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    REQUIRE ((graph).has_value ());
    auto identity = graph.value ().blocks.data ();
    auto metadata = analyses.get<GVNAnalysis> ();
    REQUIRE ((metadata).has_value ());
    auto memory = metadata.value ().memory.data ();
    FunctionPassManager passes;
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
    std::size_t observations = 0;
    auto status = passes.run (f, analyses, [&] (const PassEvent &event) -> Result<void> {
      if (event.phase == PassEvent::Phase::After) {
        ++observations;
        REQUIRE ((event.function.ssa_form ().value ().verify ()).has_value ());
        auto updated = analyses.get<GVNAnalysis> ();
        REQUIRE ((updated).has_value ());
        CHECK (updated.value ().memory.data () == memory);
        for (auto &[value, number] :
             mirnext::transforms::detail::AnalysisAccess::numbers (((updated)).value ())) {
          (void) number;
          CHECK (&value->form () == f.ssa_form ().get ());
        }
        for (auto &location : updated.value ().memory)
          if ((location.base).has_value ()) CHECK (location.base.value ().alive ());
        auto cfg = analyses.get<FunctionGraphAnalysis> ();
        REQUIRE ((cfg).has_value ());
        CHECK (cfg.value ().blocks.data () == identity);
      }
      return {};
    });
    INFO (((status).has_value () ? "" : status.error ().message));
    REQUIRE ((status).has_value ());
    CHECK (observations == 2);
    CHECK (dump (*source) == before);
    finish (f, analyses);
    source.reset ();
    REQUIRE ((copy.value ().verify ()).has_value ());
    auto binary = serialization::write_binary ((copy).value ());
    REQUIRE ((binary).has_value ());
    REQUIRE ((serialization::read_binary ((binary).value ())).has_value ());
    auto printed = serialization::write_text ((copy).value ());
    REQUIRE ((printed).has_value ());
    REQUIRE ((serialization::parse_text ((printed).value ())).has_value ());
  }
}
TEST_CASE ("dead stores disappear only when their memory is not observable") {
  auto text = [] (std::string_view body) {
    return "m: module\nentry: func i64, i64:x\nlocal i64:p, i64:v\nalloca p, 16\n"
           + std::string (body) + "\nendfunc\nendmodule\n";
  };
  for (bool live : {false, true}) {
    auto context
      = parse (text (live ? "mov i64:0(p), x\nmov v, i64:0(p)\nret v" : "mov i64:0(p), x\nret x"));
    auto &f = entry (*context);
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    CHECK (stores (f) == 1);
    FunctionPassManager passes;
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
    REQUIRE ((passes.run (f, analyses)).has_value ());
    // GVN forwards a same-location load, so even the originally read stack slot is dead.
    CHECK (stores (f) == 0);
    REQUIRE ((f.verify ()).has_value ());
  }
  auto context = parse (
    "m: module\nstate: i64 0\nentry: func i64, i64:x\nlocal i64:p\nmov p, state\nmov i64:0(p), "
    "x\nret x\nendfunc\nendmodule\n");
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses);
  FunctionPassManager passes;
  passes.add<DeadStoresPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
  CHECK (stores (f) == 1);
}
TEST_CASE ("memory pass phase ownership and stale metadata failures are diagnosed before edits") {
  auto context = parse (test::memory_fixtures ()[0]), foreign = parse (test::memory_fixtures ()[0]);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f), wrong (entry (*foreign));
  auto before = dump (f);
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (CopyPropagationPass (), f, analyses)).has_value ());
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (DeadStoresPass (), f, analyses)).has_value ());
  CHECK (dump (f) == before);
  prepare (f, analyses, false);
  before = dump (f);
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (DeadStoresPass (), f, analyses)).has_value ());
  CHECK (dump (f) == before);
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (CopyPropagationPass (), f, wrong)).has_value ());
  CHECK_FALSE ((mirnext::transforms::detail::run_pass (DeadStoresPass (), f, wrong)).has_value ());
  CHECK (dump (f) == before);
  FunctionPassManager standalone;
  standalone.add<CopyPropagationPass> ();
  REQUIRE ((standalone.run (f, analyses)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  finish (f, analyses);
  prepare (f, analyses);
  auto value = analyses.get<GVNAnalysis> ();
  REQUIRE ((value).has_value ());
  auto first = &*f.blocks ().begin ()->instructions ().begin ();
  auto rewriter = SSAUpdater::get (f);
  REQUIRE ((rewriter).has_value ());
  auto inputs
    = mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->inputs (*first));
  std::vector<SSAValue *> definitions;
  for (auto use : inputs) definitions.push_back (const_cast<SSAValue *> (&use->value ()));
  REQUIRE ((rewriter.value ().replace (*first, first->opcode (), first->operands (), definitions))
             .has_value ());
  before = dump (f);
  CHECK_FALSE (
    (mirnext::transforms::detail::run_pass (DeadStoresPass (), f, analyses)).has_value ());
  CHECK (dump (f) == before);
}
TEST_CASE ("stack byte overlap remains conservative across signed displacement overflow") {
  constexpr auto store_displacement = static_cast<std::uint64_t> (INT64_MAX) - 3;
  for (std::uint64_t distance : {4, 8}) {
    auto context
      = parse ("m: module\nentry: func i64, i64:x\nlocal i64:p, i64:v\nalloca p, 16\nmov i64:"
               + std::to_string (store_displacement) + "(p), x\nmov v, i8:"
               + std::to_string (std::bit_cast<std::int64_t> (store_displacement + distance))
               + "(p)\nret v\nendfunc\nendmodule\n");
    auto &f = entry (*context);
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    CHECK (stores (f) == 1);
    FunctionPassManager passes;
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
    REQUIRE ((passes.run (f, analyses)).has_value ());
    CHECK (stores (f) == (distance == 4 ? 1 : 0));
    finish (f, analyses);
    REQUIRE ((serialization::write_binary (*context)).has_value ());
  }
}
TEST_CASE ("commuting constant AND maintains inverse SSA operand positions") {
  for (auto op : {"and", "ands"}) {
    auto context
      = parse ("m: module\nentry: func i64, i64:x\nlocal i64:a, i64:b\n" + std::string (op)
               + " a, 131071, x\nuext8 b, a\nret b\nendfunc\nendmodule\n");
    auto &f = entry (*context);
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    FunctionPassManager passes;
    passes.add<CopyPropagationPass> ();
    REQUIRE ((passes.run (f, analyses)).has_value ());
    std::size_t ands = 0;
    for (auto &block : f.blocks ())
      for (auto &i : block.instructions ())
        if (i.opcode () == Opcode::And || i.opcode () == Opcode::Ands) {
          ++ands;
          for (auto use :
               mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->inputs (i))) {
            auto site = std::get_if<SSAInstructionUse> (&use->site ());
            REQUIRE (site);
            CHECK (*mirnext::ir::detail::operand_access (i.operands ()[site->operand])
                      .get<const Register *> ()
                   == &use->value ().reg ());
          }
        }
    CHECK (ands == 2);
    REQUIRE ((f.ssa_form ().value ().verify ()).has_value ());
    finish (f, analyses);
    REQUIRE ((f.verify ()).has_value ());
  }
}
TEST_CASE (
  "coalescing copies preserves hardware return effects and rejects invalid edits atomically") {
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  auto context = parse (
    "m: module\nentry: func i64, i64:x\nglobal i64:h:" + std::string (binding)
    + "\nlocal i64:a, i64:b\nadd a, x, 1\nmov h, a\nadd b, a, x\nret b\nendfunc\nendmodule\n");
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, false);
  FunctionPassManager passes;
  passes.add<CopyPropagationPass> ();
  REQUIRE ((passes.run (f, analyses)).has_value ());
  REQUIRE ((f.ssa_form ().value ().verify ()).has_value ());
  bool binding_defined = false;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Add)
        for (auto v :
             mirnext::ir::detail::ReferenceAccess::pointers (f.ssa_form ().get ()->definitions (i)))
          binding_defined |= v->reg ().hardware_binding () == binding;
  CHECK (binding_defined);
  finish (f, analyses);
  REQUIRE ((serialization::write_binary (*context)).has_value ());
  auto rejected = parse ("m: module\nentry: func i64\nglobal i64:h:" + std::string (binding)
                         + "\nlocal i64:a\nmov h, 42\nmov a, h\nret a\nendfunc\nendmodule\n");
  auto &g = entry (*rejected);
  FunctionAnalysisManager g_analyses (g);
  prepare (g, g_analyses, false);
  Instruction *copy = nullptr;
  for (auto &b : g.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Mov
          && mirnext::ir::detail::operand_access (i.operands ()[1]).get<const Register *> ())
        copy = &i;
  REQUIRE (copy);
  auto rewriter = SSAUpdater::get (g);
  REQUIRE ((rewriter).has_value ());
  auto before = dump (g);
  CHECK_FALSE ((rewriter.value ().coalesce_copy (*copy)).has_value ());
  CHECK (dump (g) == before);
  REQUIRE ((g.ssa_form ().value ().verify ()).has_value ());
}
TEST_CASE ("all public opcodes pass copy propagation and dead store elimination") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = test::public_instruction_example (descriptor);
    FunctionPassManager passes;
    passes.add<SimplifyPass> ();
    passes.add<PrepareCFGPass> ();
    passes.add<BuildSSAPass> ();
    passes.add<GVNPass> ();
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
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
