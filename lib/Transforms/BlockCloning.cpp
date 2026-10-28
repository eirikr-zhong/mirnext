#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. Derived from MIR clone_bbs, Copyright 2018-2024 Vladimir Makarov.
 */
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <unordered_set>
#include <map>
#include <tuple>
namespace mirnext::transforms {
namespace {
using namespace ir;
struct Node;
struct Link {
  Node *target;
  bool back = false;
};
struct Node {
  BasicBlock *body;
  std::size_t rank, head, size;
  std::vector<Link> outgoing;
};
Error invalid (const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument,
          std::string (function.symbol ().name ()) + ": clone-blocks: " + std::move (reason)};
}
}  // namespace
Result<PassResult> BlockCloningPass::run (ir::MIRBuilder &edit,
                                          analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (&analyses.function () != &function) return invalid (function, "analysis owner mismatch");
  auto phase = function.require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  if (function.needs_materialization ())
    return invalid (function, "stack scopes must be materialized first");
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto original = analyses.get<analysis::ConstructionOrderAnalysis> ();
  if (!(original).has_value ()) return original.error ();
  auto order = (original).value ();
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  if (!(graph).has_value ()) return graph.error ();
  std::unordered_map<const BasicBlock *, Node> nodes;
  nodes.reserve (analysis::detail::NodeAccess::blocks (order).size ());
  for (auto &block : function.blocks ()) {
    auto info = analysis::detail::NodeAccess::blocks (order).at (&block);
    nodes.emplace (&block, Node{&block,
                                info.rank,
                                info.head,
                                block.size ()
                                  + ir::detail::ReferenceAccess::pointers (block.labels ()).size ()
                                  + (info.head == 0),
                                {}});
  }
  using EdgeKey = std::tuple<std::size_t, std::size_t, analysis::EdgeKind, std::size_t>;
  std::map<EdgeKey, std::size_t> edge_order;
  auto rank = [&] (const analysis::FunctionGraph::Block &block) {
    return analysis::detail::NodeAccess::ir (block)
             ? nodes.at (analysis::detail::NodeAccess::ir (block)).rank
           : block.kind == analysis::FunctionGraph::Kind::Entry ? 0u
           : block.kind == analysis::FunctionGraph::Kind::Exit  ? 1u
                                                                : 2u;
  };
  for (std::size_t id = 0; id < graph.value ().edges.size (); ++id) {
    auto &edge = graph.value ().edges[id];
    edge_order.emplace (EdgeKey{rank (graph.value ().blocks[edge.source]),
                                rank (graph.value ().blocks[edge.destination]), edge.kind,
                                edge.operand},
                        id);
  }
  std::size_t next_edge = graph.value ().edges.size ();
  auto connect
    = [&] (Node &source, Node &target, analysis::EdgeKind kind, std::size_t operand = SIZE_MAX) {
        source.outgoing.push_back ({&target});
        edge_order.insert_or_assign (EdgeKey{source.rank, target.rank, kind, operand}, next_edge++);
      };
  for (auto &block : function.blocks ()) {
    auto b = analysis::detail::NodeAccess::indices (((graph)).value ()).at (&block);
    auto &node = nodes.at (&block);
    for (auto id : graph.value ().outgoing[b])
      if (auto target = analysis::detail::NodeAccess::ir (
            graph.value ().blocks[graph.value ().edges[id].destination]))
        node.outgoing.push_back ({&nodes.at (target), graph.value ().back_edges[id]});
  }
  std::unordered_set<std::size_t> hot{0, 1, 2};
  BasicBlock *first_return = nullptr;
  for (auto &block : function.blocks ()) {
    hot.insert (nodes.at (&block).rank);
    if (auto tail = block.terminator ().get ();
        tail && (tail->opcode () == Opcode::Ret || tail->opcode () == Opcode::JRet)) {
      first_return = &block;
      break;
    }
  }
  if (!first_return) return PassResult{0, analysis::PreservedAnalyses::all ()};
  InstructionRewriter rewriter (function);
  std::vector<Node *> work;
  for (auto block = rewriter.next_block (*first_return); block;
       block = rewriter.next_block (*block)) {
    auto &node = nodes.at (block);
    auto tail = block->terminator ().get ();
    if (tail && tail->opcode () == Opcode::Jmp && !node.outgoing.empty ()
        && hot.contains (node.outgoing[0].target->rank))
      work.push_back (&node);
  }
  auto add
    = [&] (Node &node, Opcode opcode, std::span<const Operand> operands) -> Result<Instruction &> {
    auto result = rewriter.insert (*node.body, nullptr, opcode, operands);
    if (!(result).has_value ()) return result.error ();
    analysis::detail::NodeAccess::instructions (order).emplace (&(result).value (),
                                                                order.next_instruction++);
    ++node.size;
    return (result).value ();
  };
  auto anchor = [&] (Node &node) -> Result<const Label &> {
    if (!ir::detail::ReferenceAccess::pointers (node.body->labels ()).empty ())
      return *(*ir::detail::ReferenceAccess::pointers (node.body->labels ()).begin ());
    auto result = ir::detail::access (function).create_label (*node.body);
    if (!(result).has_value ()) return result.error ();
    node.head = order.next_instruction++;
    analysis::detail::NodeAccess::labels (order).emplace (&(result).value (), node.head);
    ++node.size;
    analysis::detail::NodeAccess::blocks (order).at (node.body).head = node.head;
    return (result).value ();
  };
  auto jump = [&] (Node &node, const Label &label) -> Result<void> {
    Operand operands[]{Operand (label)};
    auto added = add (node, Opcode::Jmp, operands);
    if (!(added).has_value ()) return added.error ();
    return {};
  };
  std::size_t bound = work.size (), original_size = 0, size = 0, changes = 0;
  while (!work.empty ()) {
    if (bound > work.size ()) {
      bound = work.size ();
      original_size = size = work.back ()->size;
    }
    auto &source = *work.back ();
    work.pop_back ();
    if (source.outgoing.size () != 1 || !source.body->terminator ().get ()
        || source.body->terminator ().get ()->opcode () != Opcode::Jmp)
      return invalid (function, "inconsistent jump predecessor");
    if (source.outgoing[0].back) continue;
    auto &destination = *source.outgoing[0].target;
    auto tail = destination.body->size () ? &*std::prev (destination.body->instructions ().end ())
                                          : nullptr;
    auto opcode = tail ? tail->opcode () : Opcode::Label;
    if (opcode == Opcode::Ret || opcode == Opcode::JRet || opcode == Opcode::Switch
        || size > 3 * original_size)
      continue;
    // Borrow stable destination instructions. A self-clone needs a snapshot
    // because erasing and appending would otherwise change the visited range.
    struct Copy {
      Opcode opcode;
      std::vector<Operand> operands;
    };
    std::vector<Copy> copies;
    if (&source == &destination)
      for (auto &instruction : destination.body->instructions ())
        copies.push_back ({instruction.opcode (),
                           {instruction.operands ().begin (), instruction.operands ().end ()}});
    auto removed = const_cast<Instruction *> (source.body->terminator ().get ());
    analysis::detail::NodeAccess::instructions (order).erase (removed);
    checked = rewriter.erase (*removed);
    if (!(checked).has_value ()) return checked.error ();
    --source.size;
    if (&source == &destination) {
      for (auto &instruction : copies) {
        auto added = add (source, instruction.opcode, instruction.operands);
        if (!(added).has_value ()) return added.error ();
        ++size;
      }
    } else {
      for (auto &instruction : destination.body->instructions ()) {
        auto added = add (source, instruction.opcode (), instruction.operands ());
        if (!(added).has_value ()) return added.error ();
        ++size;
      }
    }
    ++changes;
    source.outgoing.clear ();
    if (opcode == Opcode::Jmp) {
      auto label = *ir::detail::operand_access (source.body->terminator ().get ()->operands ()[0])
                      .get<const Label *> ();
      auto &target = nodes.at (&label->block ());
      connect (source, target, analysis::EdgeKind::Branch, 0);
      // This deliberately preserves MIR's anchor-rank lookup, which differs
      // from the initial worklist's block-rank lookup.
      if (hot.contains (target.head)) work.push_back (&source);
    } else {
      auto following = rewriter.next_block (*destination.body);
      if (!following) return invalid (function, "missing destination fallthrough");
      auto &target = nodes.at (following);
      auto label = anchor (target);
      if (!(label).has_value ()) return label.error ();
      const bool split = source.body->terminator ().get () != nullptr;
      if (!split) {
        checked = jump (source, (label).value ());
        if (!(checked).has_value ()) return checked.error ();
        connect (source, target, analysis::EdgeKind::Branch, 0);
        if (hot.contains (target.head)) work.push_back (&source);
      } else {
        auto next = rewriter.next_block (*source.body);
        auto created = next ? ir::detail::access (function).create_block_before (*next)
                            : ir::detail::access (function).create_block ();
        if (!(created).has_value ()) return created.error ();
        auto rank = order.next_block++;
        auto head = order.next_instruction;
        analysis::detail::NodeAccess::blocks (order)
          .emplace (&(created).value (), analysis::ConstructionOrder::Block{rank, head});
        auto [position, fresh]
          = nodes.emplace (&(created).value (), Node{&(created).value (), rank, head, 0, {}});
        auto &fallthrough = position->second;
        checked = jump (fallthrough, (label).value ());
        if (!(checked).has_value ()) return checked.error ();
        connect (source, fallthrough, analysis::EdgeKind::Fallthrough);
        if (instruction_descriptor (opcode).get ()->flow == ControlFlow::Branch) {
          auto branch
            = *ir::detail::operand_access (source.body->terminator ().get ()->operands ()[0])
                 .get<const Label *> ();
          connect (source, nodes.at (&branch->block ()), analysis::EdgeKind::Branch, 0);
        } else if (opcode == Opcode::JmpIndirect) {
          source.outgoing.clear ();
          for (auto edge : destination.outgoing)
            connect (source, *edge.target, analysis::EdgeKind::Indirect);
        }
        connect (fallthrough, target, analysis::EdgeKind::Branch, 0);
        const bool conditional = instruction_descriptor (opcode).get ()->flow == ControlFlow::Branch
                                 && opcode != Opcode::PrBeq && opcode != Opcode::PrBne;
        if (hot.contains (conditional ? target.rank : target.head)) work.push_back (&fallthrough);
      }
    }
  }
  if (!changes) return PassResult{0, analysis::PreservedAnalyses::all ()};
  {
    auto cfg = analyses.get<analysis::ControlFlowAnalysis> ();
    if (!(cfg).has_value ()) return cfg.error ();
    std::vector<BasicBlock *> dead;
    for (auto &block : function.blocks ())
      if (!cfg.value ()
             .reachable[analysis::detail::NodeAccess::indices (((cfg)).value ()).at (&block)])
        dead.push_back (&block);
    checked = rewriter.erase_blocks (dead);
    if (!(checked).has_value ()) return checked.error ();
  }
  checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto rebuilt = analysis::FunctionGraphAnalysis::run (function, analyses);
  if (!(rebuilt).has_value ()) return rebuilt.error ();
  auto sequence = [&] (std::size_t id) {
    auto &edge = rebuilt.value ().edges[id];
    auto found = edge_order.find ({rank (rebuilt.value ().blocks[edge.source]),
                                   rank (rebuilt.value ().blocks[edge.destination]), edge.kind,
                                   edge.operand});
    return found == edge_order.end () ? SIZE_MAX : found->second;
  };
  for (auto &incoming : rebuilt.value ().incoming)
    std::stable_sort (incoming.begin (), incoming.end (),
                      [&] (auto a, auto b) { return sequence (a) < sequence (b); });
  for (auto &outgoing : rebuilt.value ().outgoing)
    std::stable_sort (outgoing.begin (), outgoing.end (),
                      [&] (auto a, auto b) { return sequence (a) < sequence (b); });
  analysis::enumerate ((rebuilt).value ());
  checked = analyses.set<analysis::FunctionGraphAnalysis> (std::move ((rebuilt).value ()));
  if (!(checked).has_value ()) return checked.error ();
  checked = analyses.set<analysis::ConstructionOrderAnalysis> (std::move (order));
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::ConstructionOrderAnalysis> ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  return PassResult{changes, preserved};
}
}  // namespace mirnext::transforms
