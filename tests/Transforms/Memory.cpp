/* Licensed under the MIT License. Native copy propagation and memory liveness. */
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
std::unique_ptr<Context> parse (std::string_view text) {
  auto result = serialization::parse_text (text);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  return std::move (*result);
}
Function &entry (Context &context) {
  return const_cast<Function &> (*context.modules ().begin ()->find_symbol ("entry")->function ());
}
void prepare (Function &f, FunctionAnalysisManager &analyses, bool gvn = true) {
  FunctionPassManager passes;
  passes.add<SimplifyPass> ();
  passes.add<PrepareCFGPass> ();
  passes.add<BuildSSAPass> ();
  if (gvn) passes.add<GVNPass> ();
  auto result = passes.run (f, analyses);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
}
void finish (Function &f, FunctionAnalysisManager &analyses) {
  FunctionPassManager passes;
  passes.add<ConventionalSSAPass> ();
  passes.add<DestroySSAPass> ();
  REQUIRE (passes.run (f, analyses));
}
std::size_t stores (const Function &f) {
  std::size_t result = 0;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if ((i.opcode () == Opcode::Mov || i.opcode () == Opcode::DMov || i.opcode () == Opcode::FMov
           || i.opcode () == Opcode::LDMov)
          && i.operands ()[0].get<Memory> ())
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
    REQUIRE (copy);
    auto &f = entry (**copy);
    FunctionAnalysisManager analyses (f);
    prepare (f, analyses);
    auto graph = analyses.get<FunctionGraphAnalysis> ();
    REQUIRE (graph);
    auto identity = (*graph)->blocks.data ();
    auto metadata = analyses.get<GVNAnalysis> ();
    REQUIRE (metadata);
    auto memory = (*metadata)->memory.data ();
    FunctionPassManager passes;
    passes.add<CopyPropagationPass> ();
    passes.add<DeadStoresPass> ();
    std::size_t observations = 0;
    auto status = passes.run (f, analyses, [&] (const PassEvent &event) -> Result<void> {
      if (event.phase == PassEvent::Phase::After) {
        ++observations;
        REQUIRE (event.function.ssa_form ()->verify ());
        auto updated = analyses.get<GVNAnalysis> ();
        REQUIRE (updated);
        CHECK ((*updated)->memory.data () == memory);
        for (auto &[value, number] : (*updated)->numbers) {
          (void) number;
          CHECK (&value->form () == f.ssa_form ());
        }
        for (auto &location : (*updated)->memory)
          if (location.base) CHECK (location.base->alive ());
        auto cfg = analyses.get<FunctionGraphAnalysis> ();
        REQUIRE (cfg);
        CHECK ((*cfg)->blocks.data () == identity);
      }
      return {};
    });
    INFO ((status ? "" : status.error ().message));
    REQUIRE (status);
    CHECK (observations == 2);
    CHECK (dump (*source) == before);
    finish (f, analyses);
    source.reset ();
    REQUIRE ((*copy)->verify ());
    auto binary = serialization::write_binary (**copy);
    REQUIRE (binary);
    REQUIRE (serialization::read_binary (*binary));
    auto printed = serialization::write_text (**copy);
    REQUIRE (printed);
    REQUIRE (serialization::parse_text (*printed));
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
    REQUIRE (passes.run (f, analyses));
    // GVN forwards a same-location load, so even the originally read stack slot is dead.
    CHECK (stores (f) == 0);
    REQUIRE (f.verify ());
  }
  auto context = parse (
    "m: module\nstate: i64 0\nentry: func i64, i64:x\nlocal i64:p\nmov p, state\nmov i64:0(p), "
    "x\nret x\nendfunc\nendmodule\n");
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses);
  FunctionPassManager passes;
  passes.add<DeadStoresPass> ();
  REQUIRE (passes.run (f, analyses));
  CHECK (stores (f) == 1);
}
TEST_CASE ("memory pass phase ownership and stale metadata failures are diagnosed before edits") {
  auto context = parse (test::memory_fixtures ()[0]), foreign = parse (test::memory_fixtures ()[0]);
  auto &f = entry (*context);
  FunctionAnalysisManager analyses (f), wrong (entry (*foreign));
  auto before = dump (f);
  CHECK_FALSE (CopyPropagationPass ().run (f, analyses));
  CHECK_FALSE (DeadStoresPass ().run (f, analyses));
  CHECK (dump (f) == before);
  prepare (f, analyses, false);
  before = dump (f);
  CHECK_FALSE (DeadStoresPass ().run (f, analyses));
  CHECK (dump (f) == before);
  CHECK_FALSE (CopyPropagationPass ().run (f, wrong));
  CHECK_FALSE (DeadStoresPass ().run (f, wrong));
  CHECK (dump (f) == before);
  FunctionPassManager standalone;
  standalone.add<CopyPropagationPass> ();
  REQUIRE (standalone.run (f, analyses));
  REQUIRE (f.verify ());
  finish (f, analyses);
  prepare (f, analyses);
  auto value = analyses.get<GVNAnalysis> ();
  REQUIRE (value);
  auto first = &*f.blocks ().begin ()->instructions ().begin ();
  auto rewriter = SSARewriter::get (f);
  REQUIRE (rewriter);
  auto inputs = f.ssa_form ()->inputs (*first);
  std::vector<SSAValue *> definitions;
  for (auto use : inputs) definitions.push_back (const_cast<SSAValue *> (&use->value ()));
  REQUIRE (rewriter->replace (*first, first->opcode (), first->operands (), definitions));
  before = dump (f);
  CHECK_FALSE (DeadStoresPass ().run (f, analyses));
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
    REQUIRE (passes.run (f, analyses));
    CHECK (stores (f) == (distance == 4 ? 1 : 0));
    finish (f, analyses);
    REQUIRE (serialization::write_binary (*context));
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
    REQUIRE (passes.run (f, analyses));
    std::size_t ands = 0;
    for (auto &block : f.blocks ())
      for (auto &i : block.instructions ())
        if (i.opcode () == Opcode::And || i.opcode () == Opcode::Ands) {
          ++ands;
          for (auto use : f.ssa_form ()->inputs (i)) {
            auto site = std::get_if<SSAInstructionUse> (&use->site ());
            REQUIRE (site);
            CHECK (*i.operands ()[site->operand].get<const Register *> () == &use->value ().reg ());
          }
        }
    CHECK (ands == 2);
    REQUIRE (f.ssa_form ()->verify ());
    finish (f, analyses);
    REQUIRE (f.verify ());
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
  REQUIRE (passes.run (f, analyses));
  REQUIRE (f.ssa_form ()->verify ());
  bool binding_defined = false;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Add)
        for (auto v : f.ssa_form ()->definitions (i))
          binding_defined |= v->reg ().hardware_binding () == binding;
  CHECK (binding_defined);
  finish (f, analyses);
  REQUIRE (serialization::write_binary (*context));
  auto rejected = parse ("m: module\nentry: func i64\nglobal i64:h:" + std::string (binding)
                         + "\nlocal i64:a\nmov h, 42\nmov a, h\nret a\nendfunc\nendmodule\n");
  auto &g = entry (*rejected);
  FunctionAnalysisManager g_analyses (g);
  prepare (g, g_analyses, false);
  Instruction *copy = nullptr;
  for (auto &b : g.blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == Opcode::Mov && i.operands ()[1].get<const Register *> ()) copy = &i;
  REQUIRE (copy);
  auto rewriter = SSARewriter::get (g);
  REQUIRE (rewriter);
  auto before = dump (g);
  CHECK_FALSE (rewriter->coalesce_copy (*copy));
  CHECK (dump (g) == before);
  REQUIRE (g.ssa_form ()->verify ());
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
    INFO ((result ? "" : result.error ().message));
    REQUIRE (result);
    REQUIRE (context->verify ());
    auto binary = serialization::write_binary (*context);
    REQUIRE (binary);
    REQUIRE (serialization::read_binary (*binary));
  }
}
