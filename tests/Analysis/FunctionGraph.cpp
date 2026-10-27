/* Licensed under the MIT License. Function graph analysis tests. */
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
std::unique_ptr<Context> parse (std::string_view text) {
  auto context = serialization::parse_text (text);
  INFO ((context ? "" : context.error ().message));
  REQUIRE (context);
  return std::move (*context);
}
Function &entry (Context &context) { return *context.modules ().begin ()->functions ().begin (); }
void prepare (Function &function, FunctionAnalysisManager &manager, bool prune) {
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::PrepareCFGPass> (prune);
  auto result = passes.run (function, manager);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
}
struct DependencyCycle {
  using Result = unsigned;
  static inline bool recurse = true;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Function &, FunctionAnalysisManager &manager) {
    if (!recurse) return 42;
    auto repeated = manager.get<DependencyCycle> ();
    if (!repeated) return repeated.error ();
    return **repeated;
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
  REQUIRE (graph);
  auto &g = **graph;
  REQUIRE (g.blocks.size () >= 3);
  REQUIRE (g.outgoing[0].size () >= 2);
  CHECK (g.edges[g.outgoing[0][0]].destination == FunctionGraph::exit);
  CHECK (g.edges[g.outgoing[0][1]].destination == 2);
  CHECK (g.preorder[0] == 1);
  CHECK (g.preorder[1] == 2);
  CHECK (g.rpost[0] == 1);
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE (dom);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE (live);
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE (loops);
  CHECK (manager.cached () == 6);
  CHECK (*manager.get<FunctionGraphAnalysis> () == *graph);
  for (auto &block : function.blocks ()) CHECK (g.blocks[g.indices.at (&block)].ir == &block);
  auto source = serialization::write_text (*context);
  REQUIRE (source);
  REQUIRE (manager.get<DominatorAnalysis> ());
  REQUIRE (manager.get<LivenessAnalysis> ());
  REQUIRE (manager.get<LoopAnalysis> ());
  CHECK (*serialization::write_text (*context) == *source);
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
      REQUIRE (graph);
      auto &g = **graph;
      auto dominators = manager.get<DominatorAnalysis> ();
      REQUIRE (dominators);
      auto liveness = manager.get<LivenessAnalysis> ();
      REQUIRE (liveness);
      auto &live = **liveness;
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
          CHECK ((*dominators)->dominates (ancestor, b) == dom[b].contains (ancestor));
        for (std::size_t r = 0; r < live.registers.size (); ++r) {
          CHECK (live.live_in (b, *live.registers[r]) == in[b].contains (r));
          CHECK (live.live_out (b, *live.registers[r]) == out[b].contains (r));
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
  REQUIRE (graph);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE (live);
  auto uses = manager.get<RegisterUseAnalysis> ();
  REQUIRE (uses);
  auto address = function.find_register ("address");
  REQUIRE (address);
  auto first = &*function.blocks ().begin ();
  auto b = (*graph)->indices.at (first);
  CHECK ((*live)->live_in (b, *address));
  CHECK_FALSE ((*live)->killed[b].contains ((*live)->indices.at (address)));
  REQUIRE ((*uses)->occurrences.at (address).size () == 2);
  CHECK ((*uses)->occurrences.at (address)[0].definition);
  auto bound = parse (test::analysis_fixtures ()[8]);
  auto &f = entry (*bound);
  auto hardware = f.find_register ("bound");
  REQUIRE (hardware);
  auto &body = *f.blocks ().begin ();
  auto move = std::next (body.instructions ().begin ());
  REQUIRE (move->opcode () == Opcode::Mov);
  REQUIRE (IRRewriter (f).erase (*move));
  FunctionAnalysisManager analyses (f);
  auto bound_live = analyses.get<LivenessAnalysis> ();
  REQUIRE (bound_live);
  CHECK ((*bound_live)->live_in (0, *hardware));
}
TEST_CASE ("nested natural loops and irreducible loops retain MIR grouping") {
  auto context = parse (test::analysis_fixtures ()[1]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE (loops);
  auto outer = function.find_label ("outer"), inner = function.find_label ("inner");
  REQUIRE (outer);
  REQUIRE (inner);
  CHECK ((*loops)->depth ((*graph)->indices.at (&outer->block ())) == 1);
  CHECK ((*loops)->depth ((*graph)->indices.at (&inner->block ())) == 2);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE (live);
  CHECK ((*live)->transfers > (*graph)->blocks.size ());
  auto multiple = parse (test::analysis_fixtures ()[3]);
  auto &f = entry (*multiple);
  FunctionAnalysisManager other (f);
  prepare (f, other, true);
  auto ignored = other.get<LoopAnalysis> ();
  REQUIRE (ignored);
  CHECK ((*ignored)->nodes.size () == (*other.get<FunctionGraphAnalysis> ())->blocks.size () + 1);
}
TEST_CASE ("value edits retain graph and dominance while liveness recomputes") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE (dom);
  REQUIRE (manager.get<LivenessAnalysis> ());
  auto &move = *function.blocks ().begin ()->instructions ().begin ();
  REQUIRE (move.opcode () == Opcode::Mov);
  REQUIRE (IRRewriter (function).replace_operand (move, 1, Operand (*function.arguments ()[0])));
  manager.invalidate (PreservedAnalyses::all ());
  CHECK (manager.cached () == 3);
  CHECK (*manager.get<FunctionGraphAnalysis> () == *graph);
  CHECK (*manager.get<DominatorAnalysis> () == *dom);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE (live);
  CHECK ((*live)->live_in (0, *function.arguments ()[0]));
}
TEST_CASE ("batch block erasure is atomic preserves live identities and permits infinite loops") {
  auto context = parse (test::analysis_fixtures ()[6]);
  auto &function = entry (*context);
  auto a = function.find_block ("dead1"), b = function.find_block ("dead2");
  if (!a) a = &function.find_label ("dead1")->block ();
  if (!b) b = &function.find_label ("dead2")->block ();
  REQUIRE (a);
  REQUIRE (b);
  auto label = function.find_label ("done");
  REQUIRE (label);
  auto preserved = &label->block ();
  auto text = dump (function);
  auto stamps = function.revisions ();
  IRRewriter rewriter (function);
  auto live = const_cast<BasicBlock *> (preserved);
  CHECK_FALSE (rewriter.erase_blocks ({&live, 1}));
  CHECK (dump (function) == text);
  CHECK (function.revisions ().control_flow == stamps.control_flow);
  BasicBlock *dead[]
    = {const_cast<BasicBlock *> (a), const_cast<BasicBlock *> (b), const_cast<BasicBlock *> (a)};
  REQUIRE (rewriter.erase_blocks (dead));
  REQUIRE (function.verify ());
  CHECK (&label->block () == preserved);
  CHECK_FALSE (function.find_label ("dead1"));
  auto infinite = parse (test::analysis_fixtures ()[5]);
  auto &f = entry (*infinite);
  FunctionAnalysisManager manager (f);
  prepare (f, manager, true);
  REQUIRE (f.verify ());
  CHECK (std::prev (f.blocks ().end ())->terminator ()->opcode () == Opcode::Jmp);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE (loops);
  CHECK ((*loops)->depth ((*graph)->indices.at (&f.find_label ("loop")->block ())) == 1);
  auto encoded = serialization::write_binary (*infinite);
  REQUIRE (encoded);
  auto read = serialization::read_binary (*encoded);
  REQUIRE (read);
  transforms::PrepareCFGPass repeated;
  auto second = repeated.run (f, manager);
  REQUIRE (second);
  CHECK (second->transformations == 0);
}
TEST_CASE ("deep CFG analyses have no recursive traversal or quadratic dominance storage") {
  Context context;
  auto module = context.create_module ("deep");
  REQUIRE (module);
  auto function = (*module)->create_function ("entry", {});
  REQUIRE (function);
  for (unsigned n = 0; n < 12000; ++n) {
    auto block = (*function)->create_block ();
    REQUIRE (block);
    REQUIRE ((*function)->create_label (**block));
  }
  REQUIRE (IRBuilder (*std::prev ((*function)->blocks ().end ())).ret ());
  FunctionAnalysisManager manager (**function);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  auto dom = manager.get<DominatorAnalysis> ();
  REQUIRE (dom);
  auto loops = manager.get<LoopAnalysis> ();
  REQUIRE (loops);
  auto live = manager.get<LivenessAnalysis> ();
  REQUIRE (live);
  CHECK ((*dom)->immediate.size () == 12003);
  CHECK ((*dom)->dominates (2, 12002));
  CHECK ((*dom)->iterations <= 2);
  CHECK ((*loops)->depth (12002) == 0);
  CHECK ((*live)->transfers == 12003);
  auto other = (*module)->create_function ("other", {});
  REQUIRE (other);
  auto block = (*other)->create_block ();
  REQUIRE (block);
  REQUIRE (IRBuilder (**block).ret ());
  CHECK_FALSE (DominatorAnalysis::run (**other, manager));
  CHECK_FALSE (FunctionGraphAnalysis::run (**other, manager));
}
TEST_CASE ("analysis dependency failures unwind without poisoning the cache") {
  auto context = parse (test::analysis_fixtures ()[0]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  DependencyCycle::recurse = true;
  auto failed = manager.get<DependencyCycle> ();
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().message.find ("analysis dependency cycle") != std::string::npos);
  CHECK (manager.cached () == 0);
  DependencyCycle::recurse = false;
  auto recovered = manager.get<DependencyCycle> ();
  REQUIRE (recovered);
  CHECK (**recovered == 42);
}
TEST_CASE (
  "co-located labels become MIR graph anchors without changing code positions or identities") {
  auto context = parse (test::analysis_fixtures ()[9]);
  auto &function = entry (*context);
  auto first = function.find_label ("first"), second = function.find_label ("second");
  REQUIRE (first);
  REQUIRE (second);
  REQUIRE (&first->block () == &second->block ());
  auto &body = const_cast<BasicBlock &> (second->block ());
  auto instruction = &*body.instructions ().begin ();
  auto operands = instruction->operands ().data ();
  auto before = serialization::write_text (*context);
  REQUIRE (before);
  auto split = IRRewriter (function).separate_label_anchors (body);
  REQUIRE (split);
  CHECK (*split == 1);
  CHECK (first->block ().size () == 0);
  CHECK (&second->block () == &body);
  CHECK (&instruction->block () == &body);
  CHECK (instruction->operands ().data () == operands);
  auto after = serialization::write_text (*context);
  REQUIRE (after);
  CHECK (*after == *before);
  REQUIRE (function.verify ());
  auto repeated = IRRewriter (function).separate_label_anchors (body);
  REQUIRE (repeated);
  CHECK (*repeated == 0);
  FunctionAnalysisManager manager (function);
  transforms::PrepareCFGPass pass;
  auto prepared = pass.run (function, manager);
  REQUIRE (prepared);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  CHECK ((*graph)->indices.at (&first->block ()) != (*graph)->indices.at (&second->block ()));
  auto data = parse (test::analysis_fixtures ()[10]);
  auto &f = entry (*data);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, true);
  for (auto &block : f.blocks ()) CHECK (block.labels ().size () <= 1);
  REQUIRE (data->verify ());
  auto binary = serialization::write_binary (*data);
  REQUIRE (binary);
  auto decoded = serialization::read_binary (*binary);
  REQUIRE (decoded);
  auto encoded = serialization::write_binary (**decoded);
  REQUIRE (encoded);
  CHECK (*encoded == *binary);
}
TEST_CASE ("jump calls keep caller supplied continuations and remain valid as tail transfers") {
  auto context = parse (test::analysis_fixtures ()[11]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  prepare (function, manager, true);
  REQUIRE (function.verify ());
  CHECK (function.instruction_count () >= 2);
  CHECK (function.blocks ().begin ()->instructions ().begin ()->opcode () == Opcode::JCall);
  auto graph = manager.get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  CHECK ((*graph)->blocks.size () == 3);
  auto tail = parse (test::analysis_fixtures ()[12]);
  auto &f = entry (*tail);
  FunctionAnalysisManager analyses (f);
  prepare (f, analyses, true);
  auto &b = *f.blocks ().begin ();
  auto returned = const_cast<Instruction *> (b.terminator ());
  REQUIRE (returned);
  REQUIRE (returned->opcode () == Opcode::Ret);
  REQUIRE (IRRewriter (f).erase (*returned));
  REQUIRE (f.verify ());
  auto encoded = serialization::write_binary (*tail);
  REQUIRE (encoded);
  auto decoded = serialization::read_binary (*encoded);
  REQUIRE (decoded);
  IRBuilder builder (b);
  REQUIRE (builder.ret ());
  REQUIRE (f.verify ());
}
TEST_CASE ("indirect predecessor edges follow direct edges and data roots invalidate analyses") {
  auto context = parse (test::analysis_fixtures ()[4]);
  auto &function = entry (*context);
  FunctionAnalysisManager manager (function);
  auto &module = *context->modules ().begin ();
  auto target = function.find_label ("target");
  auto done = function.find_label ("done");
  REQUIRE (target);
  REQUIRE (done);
  auto graph = manager.get<ControlFlowAnalysis> ();
  REQUIRE (graph);
  auto at = (*graph)->indices.at (&target->block ());
  // An extra direct predecessor lies later in layout than the indirect jump.
  auto dead = function.find_label ("dead");
  REQUIRE (dead);
  auto &jump = *std::prev (dead->block ().instructions ().end ());
  auto mutable_jump = const_cast<Instruction *> (&jump);
  REQUIRE (IRRewriter (function).replace_operand (*mutable_jump, 0, Operand (*target)));
  auto updated = manager.get<ControlFlowAnalysis> ();
  REQUIRE (updated);
  REQUIRE ((*updated)->incoming[at].size () == 2);
  CHECK ((*updated)->edges[(*updated)->incoming[at][0]].kind == EdgeKind::Branch);
  CHECK ((*updated)->edges[(*updated)->incoming[at][1]].kind == EdgeKind::Indirect);
  auto root = function.create_block_before (const_cast<BasicBlock &> (dead->block ()), "addressed");
  REQUIRE (root);
  auto root_label = function.create_label (**root, "addressed_label");
  REQUIRE (root_label);
  REQUIRE (IRBuilder (**root).jump (*done));
  auto before = manager.get<ControlFlowAnalysis> ();
  REQUIRE (before);
  CHECK_FALSE ((*before)->reachable[(*before)->indices.at (*root)]);
  REQUIRE (module.create_data ("difference", LabelReference{*root_label, done, 7}));
  auto after = manager.get<ControlFlowAnalysis> ();
  REQUIRE (after);
  CHECK ((*after)->reachable[(*after)->indices.at (*root)]);
  transforms::PrepareCFGPass pass;
  auto prepared = pass.run (function, manager);
  REQUIRE (prepared);
  CHECK (function.find_label ((*root_label)->name ()) == *root_label);
}
