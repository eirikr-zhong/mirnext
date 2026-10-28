/* Licensed under the MIT License. Original MIR jump traversal on the canonical IR. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Transforms/JumpOptimization.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <algorithm>
#include <unordered_set>
namespace mirnext::transforms {
namespace {
using namespace ir;
using namespace analysis;
bool branch (Opcode code) {
  return code == Opcode::Jmp
         || (instruction_descriptor (code).get ()->flow == ControlFlow::Branch
             && code != Opcode::PrBeq && code != Opcode::PrBne);
}
class JumpOptimizer {
  Function &function;
  InstructionRewriter rewriter;
  FunctionGraph graph;
  std::vector<bool> alive;
  std::vector<std::size_t> next;
  std::size_t changes = 0;
  BasicBlock *body (std::size_t b) {
    return const_cast<BasicBlock *> (analysis::detail::NodeAccess::ir (graph.blocks[b]));
  }
  void erase_edge (std::size_t id) {
    auto &edge = graph.edges[id];
    std::erase (graph.outgoing[edge.source], id);
    std::erase (graph.incoming[edge.destination], id);
  }
  void erase_block (std::size_t b) {
    while (!graph.incoming[b].empty ()) erase_edge (graph.incoming[b].back ());
    while (!graph.outgoing[b].empty ()) erase_edge (graph.outgoing[b].back ());
    alive[b] = false;
    if (auto block = body (b))
      changes
        += 1 + block->size () + ir::detail::ReferenceAccess::pointers (block->labels ()).size ();
  }
  std::size_t following (std::size_t b) {
    auto n = next[b];
    while (n != SIZE_MAX && !alive[n]) n = next[n];
    return next[b] = n;
  }
  void prune () {
    std::vector<bool> reachable (alive.size ());
    std::vector<std::size_t> pending{2};
    reachable[2] = true;
    for (std::size_t b = 2; b < alive.size (); ++b)
      if (alive[b] && graph.blocks[b].address_taken && !reachable[b]) {
        reachable[b] = true;
        pending.push_back (b);
      }
    while (!pending.empty ()) {
      auto b = pending.back ();
      pending.pop_back ();
      for (auto id : graph.outgoing[b]) {
        auto target = graph.edges[id].destination;
        if (!reachable[target]) {
          reachable[target] = true;
          pending.push_back (target);
        }
      }
    }
    for (std::size_t b = 3; b < alive.size (); ++b)
      if (alive[b] && !reachable[b]) erase_block (b);
  }
  // Return the next flat-IR label only if no instruction precedes it.
  const Label *next_label (std::size_t b) {
    for (auto n = following (b); n != SIZE_MAX; n = following (n)) {
      auto block = body (n);
      if (!block) continue;
      if (!ir::detail::ReferenceAccess::pointers (block->labels ()).empty ())
        return ir::detail::ReferenceAccess::pointers (block->labels ()).front ();
      if (block->size ()) return nullptr;
    }
    return nullptr;
  }
  const Label *chain (const Label &label) {
    auto result = &label;
    auto b = analysis::detail::NodeAccess::indices (graph).at (&label.block ());
    while (true) {
      auto block = body (b);
      if (block && block->size ()) {
        auto &first = *block->instructions ().begin ();
        return first.opcode () == Opcode::Jmp
                 ? *ir::detail::operand_access (first.operands ()[0]).get<const Label *> ()
                 : result;
      }
      auto n = following (b);
      if (n == SIZE_MAX) return result;
      b = n;
      block = body (b);
      if (block && !ir::detail::ReferenceAccess::pointers (block->labels ()).empty ())
        result = ir::detail::ReferenceAccess::pointers (block->labels ()).back ();
    }
  }
  FunctionGraph compact () {
    FunctionGraph result;
    std::vector<std::size_t> ids (alive.size (), SIZE_MAX), edges (graph.edges.size (), SIZE_MAX);
    for (std::size_t b = 0; b < alive.size (); ++b)
      if (alive[b]) {
        ids[b] = result.blocks.size ();
        result.blocks.push_back (graph.blocks[b]);
        if (body (b)) analysis::detail::NodeAccess::indices (result).emplace (body (b), ids[b]);
      }
    result.incoming.resize (result.blocks.size ());
    result.outgoing.resize (result.blocks.size ());
    for (std::size_t b = 0; b < alive.size (); ++b)
      if (alive[b])
        for (auto id : graph.outgoing[b]) {
          edges[id] = result.edges.size ();
          auto e = graph.edges[id];
          e.source = ids[e.source];
          e.destination = ids[e.destination];
          result.edges.push_back (e);
          result.outgoing[ids[b]].push_back (edges[id]);
        }
    for (std::size_t b = 0; b < alive.size (); ++b)
      if (alive[b])
        for (auto id : graph.incoming[b]) result.incoming[ids[b]].push_back (edges[id]);
    enumerate (result);
    return result;
  }

 public:
  JumpOptimizer (Function &f, FunctionGraph g)
    : function (f),
      rewriter (f),
      graph (std::move (g)),
      alive (graph.blocks.size (), true),
      next (graph.blocks.size (), SIZE_MAX) {
    for (std::size_t b = 2; b + 1 < next.size (); ++b) next[b] = b + 1;
  }
  Result<PassResult> run (FunctionAnalysisManager &analyses) {
    prune ();
    std::unordered_set<const Label *> referenced;
    for (std::size_t b = 2; b < alive.size (); ++b)
      if (alive[b])
        if (auto block = body (b))
          for (auto &i : block->instructions ())
            for (auto &operand : i.operands ())
              if (auto label = ir::detail::operand_access (operand).get<const Label *> ())
                referenced.insert (*label);
    for (auto data : ir::detail::ReferenceAccess::pointers (function.label_data_uses ()))
      if (auto ref = std::get_if<LabelReference> (&data->initializer ())) {
        referenced.insert (ir::detail::NodeAccess::label (*(ref)));
        if (ir::detail::NodeAccess::base (*(ref)))
          referenced.insert (ir::detail::NodeAccess::base (*(ref)));
      }
    bool redirected = false;
    for (std::size_t b = 2; b != SIZE_MAX;) {
      auto after = following (b);
      auto block = body (b);
      if (b != 2 && graph.incoming[b].size () == 1
          && (!block
              || (!block->size ()
                  && (ir::detail::ReferenceAccess::pointers (block->labels ()).empty ()
                      || !referenced.contains (
                        ir::detail::ReferenceAccess::pointers (block->labels ()).front ()))))) {
        auto id = graph.incoming[b].front (), out = graph.outgoing[b].front ();
        auto target = graph.edges[out].destination;
        auto &incoming = graph.incoming[target];
        auto at = std::ranges::find (incoming, out);
        incoming.insert (at, id);
        graph.edges[id].destination = target;
        graph.incoming[b].clear ();
        erase_block (b);
        b = after;
        continue;
      }
      auto insn = block ? const_cast<Instruction *> (block->terminator ().get ()) : nullptr;
      if (!insn || !branch (insn->opcode ())) {
        b = after;
        continue;
      }
      auto label = *ir::detail::operand_access (insn->operands ()[0]).get<const Label *> ();
      if (next_label (b) == label) {
        auto first = graph.outgoing[b].front ();
        graph.edges[first].kind = EdgeKind::Fallthrough;
        graph.edges[first].operand = SIZE_MAX;
        while (graph.outgoing[b].size () > 1) erase_edge (graph.outgoing[b].back ());
        auto checked = rewriter.erase (*insn);
        if (!(checked).has_value ()) return checked.error ();
        ++changes;
        continue;
      }
      std::unordered_set<const Label *> visited;
      for (;;) {
        auto target = chain (*label);
        if (target == label) break;
        if (visited.empty ()) visited.insert (label);
        if (!visited.insert (target).second) break;
        auto old = analysis::detail::NodeAccess::indices (graph).at (&label->block ()),
             destination = analysis::detail::NodeAccess::indices (graph).at (&target->block ());
        auto edge = std::ranges::find_if (graph.outgoing[b], [&] (auto id) {
          return graph.edges[id].destination == old;
        });
        if (edge == graph.outgoing[b].end ())
          return Error{ErrorCode::InvalidArgument, std::string (function.symbol ().name ())
                                                     + ": jump-opt: missing branch edge"};
        auto checked = rewriter.replace_operand (*insn, 0, Operand (*target));
        if (!(checked).has_value ()) return checked.error ();
        auto id = *edge;
        std::erase (graph.incoming[old], id);
        graph.edges[id].destination = destination;
        graph.incoming[destination].push_back (id);
        label = target;
        ++changes;
        redirected = true;
      }
      b = after;
    }
    if (redirected) prune ();
    auto result = compact ();
    std::vector<BasicBlock *> removed;
    for (std::size_t b = 2; b < alive.size (); ++b)
      if (!alive[b] && body (b)) removed.push_back (body (b));
    auto checked = rewriter.erase_blocks (removed);
    if (!(checked).has_value ()) return checked.error ();
    checked = function.verify ();
    if (!(checked).has_value ()) return checked.error ();
    checked = analyses.set<FunctionGraphAnalysis> (std::move (result));
    if (!(checked).has_value ()) return checked.error ();
    auto preserved = PreservedAnalyses::none ();
    preserved.preserve<FunctionGraphAnalysis> ();
    return PassResult{changes, preserved};
  }
};
}  // namespace
Result<PassResult> JumpOptimizationPass::run (ir::MIRBuilder &edit,
                                              analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  auto invalid = [&] (std::string reason) {
    return Error{ErrorCode::InvalidArgument,
                 std::string (function.symbol ().name ()) + ": jump-opt: " + reason};
  };
  if (&analyses.function () != &function) return invalid ("analysis owner mismatch");
  auto phase = function.require_register_ir ();
  if (!(phase).has_value ()) return invalid (phase.error ().message);
  if (function.needs_materialization ())
    return invalid ("stack scopes must be materialized before jump optimization");
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  for (auto &block : function.blocks ())
    if (ir::detail::ReferenceAccess::pointers (block.labels ()).size () > 1)
      return invalid ("CFG label anchors must be separated");
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  auto retained = analyses.take_cached<analysis::FunctionGraphAnalysis> ();
  if (!(retained).has_value ()) return retained.error ();
  return JumpOptimizer (function, std::move (*(retained).value ())).run (analyses);
}
}  // namespace mirnext::transforms
