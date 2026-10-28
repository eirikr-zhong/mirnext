/* Licensed under the MIT License. Function graph analysis tests. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "Fixtures.hpp"
#include <random>
#include <set>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
namespace {
std::unique_ptr<Program> parse (std::string_view text) {
  auto context = serialization::parse_text (text);
  INFO (((context).has_value () ? "" : context.error ().message));
  REQUIRE ((context).has_value ());
  return std::make_unique<Program> (std::move ((context).value ()));
}
Function &entry (Program &context) { return *context.modules ().begin ()->functions ().begin (); }
void prepare (Function &function, FunctionAnalysisManager &manager, bool prune) {
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::PrepareCFGPass> (prune);
  auto result = passes.run (function, manager);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
}
struct DependencyCycle {
  using Result = unsigned;
  static inline bool recurse = true;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Function &, FunctionAnalysisManager &manager) {
    if (!recurse) return 42;
    auto repeated = manager.get<DependencyCycle> ();
    if (!(repeated).has_value ()) return repeated.error ();
    return (repeated).value ();
  }
};
}  // namespace
TEST_CASE ("sparse facts agree with independent ordered sets including alias operations") {
  std::mt19937 random (0x5aa0);
  SparseBitSet bits;
  std::set<std::size_t> expected;
  for (unsigned n = 0; n < 1200; ++n) {
    SparseBitSet other;
    std::set<std::size_t> reference;
    for (unsigned k = 0; k < 17; ++k) {
      auto value = random () % 8192;
      other.insert (value);
      reference.insert (value);
    }
    switch (n % 7) {
    case 0:
      bits.subtract (other);
      for (auto value : reference) expected.erase (value);
      break;
    case 1:
      bits.intersect_with (other);
      std::erase_if (expected, [&] (auto v) { return !reference.contains (v); });
      break;
    case 2:
      bits.erase_if ([&] (auto v) { return v % 7 == n % 11; });
      std::erase_if (expected, [&] (auto v) { return v % 7 == n % 11; });
      break;
    case 3:
      for (auto v : reference) {
        bits.erase (v);
        expected.erase (v);
      }
      break;
    default:
      bits.union_with (other);
      expected.insert (reference.begin (), reference.end ());
      break;
    }
    std::set<std::size_t> actual;
    bits.for_each ([&] (auto value) { actual.insert (value); });
    CHECK (actual == expected);
  }
  auto copy = bits;
  CHECK_FALSE (bits.union_with (bits));
  CHECK (bits == copy);
  bits.subtract (bits);
  CHECK (bits.empty ());
  CHECK_FALSE (bits.contains (SIZE_MAX));
  bits.insert (SIZE_MAX);
  CHECK (bits.contains (SIZE_MAX));
  bits.intersect_with (bits);
  CHECK (bits.contains (SIZE_MAX));
  bits.erase (SIZE_MAX);
  CHECK (bits.empty ());
  bits.insert (SIZE_MAX);
  bits.erase_if ([] (auto v) { return v == SIZE_MAX; });
  CHECK (bits.empty ());
  bits.insert (63);
  bits.insert (64);
  bits.intersect_with (SparseBitSet{});
  CHECK (bits.empty ());
  bits.insert (0);
  bits.clear ();
  CHECK (bits.empty ());
}
TEST_CASE ("optimization graph preserves ordered synthetic boundaries and shares cached inputs") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, false);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto &g = (graph).value ();
  REQUIRE (g.blocks.size () >= 3);
  REQUIRE (g.outgoing[0].size () >= 2);
  CHECK (g.edges[g.outgoing[0][0]].destination == FunctionGraph::exit);
  CHECK (g.edges[g.outgoing[0][1]].destination == 2);
  CHECK (g.preorder[0] == 1);
  CHECK (g.preorder[1] == 2);
  CHECK (g.rpost[0] == 1);
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE ((dom).has_value ());
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  CHECK (manager.cached () == 6);
  CHECK (&(manager.get<FunctionGraphAnalysis> ()).value () == &(graph).value ());
  for (auto &block : function.blocks ())
    CHECK (mirnext::analysis::detail::NodeAccess::ir (
             g.blocks[mirnext::analysis::detail::NodeAccess::indices (g).at (&block)])
           == &block);
  auto source = serialization::write_text (*context);
  REQUIRE ((source).has_value ());
  REQUIRE ((manager.get<DominatorAnalysis> ()).has_value ());
  REQUIRE ((manager.get<LivenessAnalysis> ()).has_value ());
  REQUIRE ((manager.get<LoopAnalysis> ()).has_value ());
  CHECK ((serialization::write_text (*context)).value () == (source).value ());
}
TEST_CASE ("dominance and liveness match independent fixed point solvers") {
  auto fixtures = test::analysis_fixtures ();
  auto random = test::random_analysis_fixtures ();
  fixtures.insert (fixtures.end (), std::make_move_iterator (random.begin ()),
                   std::make_move_iterator (random.end ()));
  for (auto &text : fixtures)
    for (bool prune : {false, true}) {
      CAPTURE (text);
      CAPTURE (prune);
      auto context = parse (text);
      auto &function = entry (*context);
      FunctionAnalysisManager manager (function);
      prepare (function, manager, prune);
      auto graph = manager.get<FunctionGraphAnalysis> ();
      REQUIRE ((graph).has_value ());
      auto &g = (graph).value ();
      auto dominators = manager.get<DominatorAnalysis> ();
      REQUIRE ((dominators).has_value ());
      auto liveness = manager.get<LivenessAnalysis> ();
      REQUIRE ((liveness).has_value ());
      auto &live = (liveness).value ();
      const auto count = g.blocks.size ();
      std::set<std::size_t> all;
      for (std::size_t b = 0; b < count; ++b) all.insert (b);
      std::vector<std::set<std::size_t>> dom (count, all), in (count), out (count), gen (count),
        kill (count);
      dom[0].clear ();
      for (std::size_t b = 0; b < count; ++b) {
        live.generated[b].for_each ([&] (auto r) { gen[b].insert (r); });
        live.killed[b].for_each ([&] (auto r) { kill[b].insert (r); });
      }
      bool changed;
      do {
        changed = false;
        for (std::size_t b = 0; b < count; ++b) {
          std::set<std::size_t> next;
          if (!g.incoming[b].empty ()) {
            next = dom[g.edges[g.incoming[b][0]].source];
            for (auto id : g.incoming[b]) {
              std::set<std::size_t> intersection;
              auto &other = dom[g.edges[id].source];
              std::set_intersection (next.begin (), next.end (), other.begin (), other.end (),
                                     std::inserter (intersection, intersection.end ()));
              next = std::move (intersection);
            }
          }
          next.insert (b);
          changed |= next != dom[b];
          dom[b] = std::move (next);
          std::set<std::size_t> outgoing;
          for (auto id : g.outgoing[b]) {
            auto &other = in[g.edges[id].destination];
            outgoing.insert (other.begin (), other.end ());
          }
          auto incoming = gen[b];
          for (auto r : outgoing)
            if (!kill[b].contains (r)) incoming.insert (r);
          changed |= incoming != in[b] || outgoing != out[b];
          in[b] = std::move (incoming);
          out[b] = std::move (outgoing);
        }
      } while (changed);
      for (std::size_t b = 0; b < count; ++b) {
        for (std::size_t ancestor = 0; ancestor < count; ++ancestor)
          CHECK (dominators.value ().dominates (ancestor, b) == dom[b].contains (ancestor));
        for (std::size_t r = 0; r < mirnext::analysis::detail::NodeAccess::registers (live).size ();
             ++r) {
          CHECK (live.live_in (b, *mirnext::analysis::detail::NodeAccess::registers (live)[r])
                 == in[b].contains (r));
          CHECK (live.live_out (b, *mirnext::analysis::detail::NodeAccess::registers (live)[r])
                 == out[b].contains (r));
        }
      }
    }
}
TEST_CASE ("semantic register roles stay distinct from the original optimization scan") {
  auto context = parse (test::analysis_fixtures ()[4]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  auto uses = manager.get<RegisterUseAnalysis> ();
  REQUIRE ((uses).has_value ());
  auto address = function.find_register ("address").get ();
  REQUIRE (address);
  auto first = &*function.blocks ().begin ();
  auto b = mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (first);
  CHECK (live.value ().live_in (b, *address));
  CHECK_FALSE (live.value ().killed[b].contains (
    mirnext::analysis::detail::NodeAccess::indices (((live)).value ()).at (address)));
  REQUIRE (uses.value ().uses (*address).size () == 2);
  CHECK (uses.value ().uses (*address)[0].definition);
  auto bound = parse (test::analysis_fixtures ()[8]);
  auto &f = entry (*bound);
  auto hardware = f.find_register ("bound").get ();
  REQUIRE (hardware);
  auto &body = *f.blocks ().begin ();
  auto move = std::next (body.instructions ().begin ());
  REQUIRE (move->opcode () == Opcode::Mov);
  REQUIRE ((InstructionRewriter (f).erase (*move)).has_value ());
  FunctionAnalysisManager analyses (f);
  auto bound_live = analyses.get<LivenessAnalysis> ();
  REQUIRE ((bound_live).has_value ());
  CHECK (bound_live.value ().live_in (0, *hardware));
}
TEST_CASE ("nested natural loops and irreducible loops retain MIR grouping") {
  auto context = parse (test::analysis_fixtures ()[1]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  auto outer = function.find_label ("outer").get (), inner = function.find_label ("inner").get ();
  REQUIRE (outer);
  REQUIRE (inner);
  CHECK (
    loops.value ().depth (
      mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (&outer->block ()))
    == 1);
  CHECK (
    loops.value ().depth (
      mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (&inner->block ()))
    == 2);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  CHECK (live.value ().transfers > graph.value ().blocks.size ());
  auto multiple = parse (test::analysis_fixtures ()[3]);
  auto &f = entry (*multiple);
  FunctionAnalysisManager other (f);
  prepare (f, other, true);
  auto ignored = other.get<LoopAnalysis> ();
  REQUIRE ((ignored).has_value ());
  CHECK (ignored.value ().nodes.size ()
         == (&(other.get<FunctionGraphAnalysis> ()).value ())->blocks.size () + 1);
}
TEST_CASE ("value edits retain graph and dominance while liveness recomputes") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE ((dom).has_value ());
  REQUIRE ((manager.get<LivenessAnalysis> ()).has_value ());
  auto &move = *function.blocks ().begin ()->instructions ().begin ();
  REQUIRE (move.opcode () == Opcode::Mov);
  REQUIRE ((InstructionRewriter (function)
              .replace_operand (move, 1,
                                Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                                  function.arguments ())[0])))
             .has_value ());
  manager.invalidate (PreservedAnalyses::all ());
  CHECK (manager.cached () == 3);
  CHECK (&(manager.get<FunctionGraphAnalysis> ()).value () == &(graph).value ());
  CHECK (&(manager.get<DominatorAnalysis> ()).value () == &(dom).value ());
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  CHECK (live.value ().live_in (0, *mirnext::ir::detail::ReferenceAccess::pointers (
                                     function.arguments ())[0]));
}
TEST_CASE ("batch block erasure is atomic preserves live identities and permits infinite loops") {
  auto context = parse (test::analysis_fixtures ()[6]);
  auto &function = entry (*context);
  auto a = function.find_block ("dead1").get (), b = function.find_block ("dead2").get ();
  if (!a) a = &function.find_label ("dead1").get ()->block ();
  if (!b) b = &function.find_label ("dead2").get ()->block ();
  REQUIRE (a);
  REQUIRE (b);
  auto label = function.find_label ("done").get ();
  REQUIRE (label);
  auto preserved = &label->block ();
  auto text = dump (function);
  auto stamps = function.revisions ();
  InstructionRewriter rewriter (function);
  auto live = const_cast<BasicBlock *> (preserved);
  CHECK_FALSE ((rewriter.erase_blocks ({&live, 1})).has_value ());
  CHECK (dump (function) == text);
  CHECK (function.revisions ().control_flow == stamps.control_flow);
  BasicBlock *dead[]
    = {const_cast<BasicBlock *> (a), const_cast<BasicBlock *> (b), const_cast<BasicBlock *> (a)};
  REQUIRE ((rewriter.erase_blocks (dead)).has_value ());
  REQUIRE ((function.verify ()).has_value ());
  CHECK (&label->block () == preserved);
  CHECK_FALSE ((function.find_label ("dead1")).has_value ());
  auto infinite = parse (test::analysis_fixtures ()[5]);
  auto &f = entry (*infinite);
  FunctionAnalysisManager manager (f);
  prepare (f, manager, true);
  REQUIRE ((f.verify ()).has_value ());
  CHECK (std::prev (f.blocks ().end ())->terminator ().value ().opcode () == Opcode::Jmp);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  CHECK (loops.value ().depth (mirnext::analysis::detail::NodeAccess::indices (((graph)).value ())
                                 .at (&f.find_label ("loop").value ().block ()))
         == 1);
  auto encoded = serialization::write_binary (*infinite);
  REQUIRE ((encoded).has_value ());
  auto read = serialization::read_binary ((encoded).value ());
  REQUIRE ((read).has_value ());
  transforms::PrepareCFGPass repeated;
  auto second = mirnext::transforms::detail::run_pass (repeated, f, manager);
  REQUIRE ((second).has_value ());
  CHECK (second.value ().transformations == 0);
}
TEST_CASE ("deep CFG analyses have no recursive traversal or quadratic dominance storage") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("deep");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ()).create_function ("entry", {});
  REQUIRE ((function).has_value ());
  for (unsigned n = 0; n < 12000; ++n) {
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    REQUIRE ((mirnext::ir::detail::access (((function)).value ()).create_label ((block).value ()))
               .has_value ());
  }
  REQUIRE (
    (InstructionBuilder (*std::prev (function.value ().blocks ().end ())).ret ()).has_value ());
  FunctionAnalysisManager manager ((function).value ());
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE ((dom).has_value ());
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  CHECK (dom.value ().immediate.size () == 12003);
  CHECK (dom.value ().dominates (2, 12002));
  CHECK (dom.value ().iterations <= 2);
  CHECK (loops.value ().depth (12002) == 0);
  CHECK (live.value ().transfers == 12003);
  auto other = mirnext::ir::detail::access (((module)).value ()).create_function ("other", {});
  REQUIRE ((other).has_value ());
  auto block = mirnext::ir::detail::access (((other)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  REQUIRE ((InstructionBuilder ((block).value ()).ret ()).has_value ());
  CHECK_FALSE ((DominatorAnalysis::run ((other).value (), manager)).has_value ());
  CHECK_FALSE ((FunctionGraphAnalysis::run ((other).value (), manager)).has_value ());
}
TEST_CASE ("analysis dependency failures unwind without poisoning the cache") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  DependencyCycle::recurse = true;
  auto failed = manager.get<DependencyCycle> ();
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("analysis dependency cycle") != std::string::npos);
  CHECK (manager.cached () == 0);
  DependencyCycle::recurse = false;
  auto recovered = manager.get<DependencyCycle> ();
  REQUIRE ((recovered).has_value ());
  CHECK ((recovered).value () == 42);
}
TEST_CASE (
  "co-located labels become MIR graph anchors without changing code positions or identities") {
  auto context = parse (test::analysis_fixtures ()[9]);
  auto &function = entry (*context);
  auto first = function.find_label ("first").get (), second = function.find_label ("second").get ();
  REQUIRE (first);
  REQUIRE (second);
  REQUIRE (&first->block () == &second->block ());
  auto &body = const_cast<BasicBlock &> (second->block ());
  auto instruction = &*body.instructions ().begin ();
  auto operands = instruction->operands ().data ();
  auto before = serialization::write_text (*context);
  REQUIRE ((before).has_value ());
  auto split = InstructionRewriter (function).separate_label_anchors (body);
  REQUIRE ((split).has_value ());
  CHECK ((split).value () == 1);
  CHECK (first->block ().size () == 0);
  CHECK (&second->block () == &body);
  CHECK (&instruction->block () == &body);
  CHECK (instruction->operands ().data () == operands);
  auto after = serialization::write_text (*context);
  REQUIRE ((after).has_value ());
  CHECK ((after).value () == (before).value ());
  REQUIRE ((function.verify ()).has_value ());
  auto repeated = InstructionRewriter (function).separate_label_anchors (body);
  REQUIRE ((repeated).has_value ());
  CHECK ((repeated).value () == 0);
  FunctionAnalysisManager manager (function);
  transforms::PrepareCFGPass pass;
  auto prepared = mirnext::transforms::detail::run_pass (pass, function, manager);
  REQUIRE ((prepared).has_value ());
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (
    mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (&first->block ())
    != mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (&second->block ()));
  auto data = parse (test::analysis_fixtures ()[10]);
  auto &f = entry (*data);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, true);
  for (auto &block : f.blocks ()) CHECK (block.labels ().size () <= 1);
  REQUIRE ((data->verify ()).has_value ());
  auto binary = serialization::write_binary (*data);
  REQUIRE ((binary).has_value ());
  auto decoded = serialization::read_binary ((binary).value ());
  REQUIRE ((decoded).has_value ());
  auto encoded = serialization::write_binary ((decoded).value ());
  REQUIRE ((encoded).has_value ());
  CHECK ((encoded).value () == (binary).value ());
}
TEST_CASE ("jump calls keep caller supplied continuations and remain valid as tail transfers") {
  auto context = parse (test::analysis_fixtures ()[11]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  REQUIRE ((function.verify ()).has_value ());
  CHECK (function.instruction_count () >= 2);
  CHECK (function.blocks ().begin ()->instructions ().begin ()->opcode () == Opcode::JCall);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (graph.value ().blocks.size () == 3);
  auto tail = parse (test::analysis_fixtures ()[12]);
  auto &f = entry (*tail);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, true);
  auto &b = *f.blocks ().begin ();
  auto returned = const_cast<Instruction *> (b.terminator ().get ());
  REQUIRE (returned);
  REQUIRE (returned->opcode () == Opcode::Ret);
  REQUIRE ((InstructionRewriter (f).erase (*returned)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  auto encoded = serialization::write_binary (*tail);
  REQUIRE ((encoded).has_value ());
  auto decoded = serialization::read_binary ((encoded).value ());
  REQUIRE ((decoded).has_value ());
  InstructionBuilder builder (b);
  REQUIRE ((builder.ret ()).has_value ());
  REQUIRE ((f.verify ()).has_value ());
}
TEST_CASE ("indirect predecessor edges follow direct edges and data roots invalidate analyses") {
  auto context = parse (test::analysis_fixtures ()[4]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  auto &module = *context->modules ().begin ();
  auto target = function.find_label ("target").get ();
  auto done = function.find_label ("done").get ();
  REQUIRE (target);
  REQUIRE (done);
  auto graph = manager.get<ControlFlowAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto at
    = mirnext::analysis::detail::NodeAccess::indices (((graph)).value ()).at (&target->block ());
  // An extra direct predecessor lies later in layout than the indirect jump.
  auto dead = function.find_label ("dead").get ();
  REQUIRE (dead);
  auto &jump = *std::prev (dead->block ().instructions ().end ());
  auto mutable_jump = const_cast<Instruction *> (&jump);
  REQUIRE ((InstructionRewriter (function).replace_operand (*mutable_jump, 0, Operand (*target)))
             .has_value ());
  auto updated = manager.get<ControlFlowAnalysis> ();
  REQUIRE ((updated).has_value ());
  REQUIRE (updated.value ().incoming[at].size () == 2);
  CHECK (updated.value ().edges[updated.value ().incoming[at][0]].kind == EdgeKind::Branch);
  CHECK (updated.value ().edges[updated.value ().incoming[at][1]].kind == EdgeKind::Indirect);
  auto root = mirnext::ir::detail::access (function).create_block_before (const_cast<BasicBlock &> (
                                                                            dead->block ()),
                                                                          "addressed");
  REQUIRE ((root).has_value ());
  auto root_label
    = mirnext::ir::detail::access (function).create_label ((root).value (), "addressed_label");
  REQUIRE ((root_label).has_value ());
  REQUIRE ((InstructionBuilder ((root).value ()).jump (*done)).has_value ());
  auto before = manager.get<ControlFlowAnalysis> ();
  REQUIRE ((before).has_value ());
  CHECK_FALSE (
    before.value ().reachable[mirnext::analysis::detail::NodeAccess::indices (((before)).value ())
                                .at (&(root).value ())]);
  REQUIRE (
    (mirnext::ir::detail::access (module)
       .create_data ("difference",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(root_label).value (),
                                                                           done, 7)))
      .has_value ());
  auto after = manager.get<ControlFlowAnalysis> ();
  REQUIRE ((after).has_value ());
  CHECK (
    after.value ().reachable[mirnext::analysis::detail::NodeAccess::indices (((after)).value ())
                               .at (&(root).value ())]);
  transforms::PrepareCFGPass pass;
  auto prepared = mirnext::transforms::detail::run_pass (pass, function, manager);
  REQUIRE ((prepared).has_value ());
  CHECK (function.find_label (root_label.value ().name ()).get () == &(root_label).value ());
}
