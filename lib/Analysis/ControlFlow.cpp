/* Licensed under the MIT License. Linear graph construction and nonrecursive traversal. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include <algorithm>

namespace mirnext::analysis {
const unsigned char *ControlFlowAnalysis::id () noexcept {
  static unsigned char token;
  return &token;
}
const unsigned char *RegisterUseAnalysis::id () noexcept {
  static unsigned char token;
  return &token;
}
mirnext::Result<ControlFlowGraph> ControlFlowAnalysis::run (const ir::Function &function) {
  auto verified = function.verify ();
  if (!(verified).has_value ()) return verified.error ();
  ControlFlowGraph graph;
  for (auto &block : function.blocks ()) {
    detail::NodeAccess::indices (graph).emplace (&block,
                                                 detail::NodeAccess::blocks (graph).size ());
    detail::NodeAccess::blocks (graph).push_back (&block);
  }
  const auto count = detail::NodeAccess::blocks (graph).size ();
  graph.incoming.resize (count);
  graph.outgoing.resize (count);
  graph.reachable.resize (count);
  auto edge = [&] (std::size_t source, std::size_t destination, EdgeKind kind,
                   std::size_t operand = SIZE_MAX) {
    auto id = graph.edges.size ();
    graph.edges.push_back ({source, destination, kind, operand});
    graph.outgoing[source].push_back (id);
    graph.incoming[destination].push_back (id);
  };
  std::vector<bool> addressable (count);
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ())
      if (instruction.opcode () == ir::Opcode::LAddr)
        addressable[detail::NodeAccess::indices (graph).at (
          &(*ir::detail::operand_access (instruction.operands ()[1]).get<const ir::Label *> ())
             ->block ())] = true;
  for (auto data : ir::detail::ReferenceAccess::pointers (function.label_data_uses ()))
    if (auto reference = std::get_if<ir::LabelReference> (&data->initializer ())) {
      if (&ir::detail::NodeAccess::label (*(reference))->function () == &function)
        addressable[detail::NodeAccess::indices (graph).at (
          &ir::detail::NodeAccess::label (*(reference))->block ())] = true;
      if (ir::detail::NodeAccess::base (*(reference))
          && &ir::detail::NodeAccess::base (*(reference))->function () == &function)
        addressable[detail::NodeAccess::indices (graph).at (
          &ir::detail::NodeAccess::base (*(reference))->block ())] = true;
    }
  std::vector<std::size_t> indirect_targets;
  for (std::size_t b = 0; b < count; ++b)
    if (addressable[b]) indirect_targets.push_back (b);
  for (std::size_t b = 0; b < count; ++b) {
    const auto terminator = detail::NodeAccess::blocks (graph)[b]->terminator ().get ();
    if (!terminator) {
      if (b + 1 < count) edge (b, b + 1, EdgeKind::Fallthrough);
      continue;
    }
    auto opcode = terminator->opcode ();
    auto operands = terminator->operands ();
    if (opcode == ir::Opcode::Switch) {
      for (std::size_t n = 1; n < operands.size (); ++n)
        edge (b,
              detail::NodeAccess::indices (graph).at (
                &(*ir::detail::operand_access (operands[n]).get<const ir::Label *> ())->block ()),
              EdgeKind::SwitchCase, n);
    } else if (opcode == ir::Opcode::Jmp
               || ir::instruction_descriptor (opcode).get ()->flow == ir::ControlFlow::Branch) {
      auto label = *ir::detail::operand_access (operands[0]).get<const ir::Label *> ();
      if (opcode != ir::Opcode::Jmp && b + 1 < count) edge (b, b + 1, EdgeKind::Fallthrough);
      edge (b, detail::NodeAccess::indices (graph).at (&label->block ()), EdgeKind::Branch, 0);
    }
  }
  // MIR appends indirect edges after all direct edges. Incoming order is also
  // PHI operand order, even when an earlier block contains the indirect jump.
  for (std::size_t b = 0; b < count; ++b)
    if (auto terminator = detail::NodeAccess::blocks (graph)[b]->terminator ().get ();
        terminator && terminator->opcode () == ir::Opcode::JmpIndirect)
      for (auto target : indirect_targets) edge (b, target, EdgeKind::Indirect);
  // An explicit stack avoids recursion proportional to the input's block count.
  struct Frame {
    std::size_t block, next;
  };
  graph.entry_roots.push_back (0);
  for (auto root : indirect_targets)
    if (root != 0) graph.entry_roots.push_back (root);
  for (auto root : graph.entry_roots) {
    if (graph.reachable[root]) continue;
    std::vector<Frame> stack{{root, 0}};
    graph.reachable[root] = true;
    std::vector<std::size_t> postorder;
    while (!stack.empty ()) {
      auto &frame = stack.back ();
      if (frame.next == graph.outgoing[frame.block].size ()) {
        postorder.push_back (frame.block);
        stack.pop_back ();
        continue;
      }
      auto target = graph.edges[graph.outgoing[frame.block][frame.next++]].destination;
      if (!graph.reachable[target]) {
        graph.reachable[target] = true;
        stack.push_back ({target, 0});
      }
    }
    graph.reverse_postorder.insert (graph.reverse_postorder.end (), postorder.rbegin (),
                                    postorder.rend ());
  }
  return graph;
}
mirnext::Result<RegisterUseIndex> RegisterUseAnalysis::run (const ir::Function &function) {
  auto phase = function.require_register_ir ();
  if (!(phase).has_value ()) return phase.error ();
  auto verified = function.verify ();
  if (!(verified).has_value ()) return verified.error ();
  RegisterUseIndex index;
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ()) {
      for (std::size_t n = 0; n < instruction.operands ().size (); ++n) {
        auto &operand = instruction.operands ()[n];
        if (auto reg = ir::detail::operand_access (operand).get<const ir::Register *> ())
          detail::NodeAccess::occurrences (index)[*reg].push_back (
            {instruction, n, RegisterOccurrence::Part::Value,
             ir::output_operand (function, instruction, n)});
        else if (auto memory = ir::detail::operand_access (operand).get<ir::Memory> ()) {
          if (ir::detail::NodeAccess::base (*(memory)))
            detail::NodeAccess::occurrences (index)[ir::detail::NodeAccess::base (*(memory))]
              .push_back ({instruction, n, RegisterOccurrence::Part::MemoryBase, false});
          if (ir::detail::NodeAccess::index (*(memory)))
            detail::NodeAccess::occurrences (index)[ir::detail::NodeAccess::index (*(memory))]
              .push_back ({instruction, n, RegisterOccurrence::Part::MemoryIndex, false});
        }
      }
    }
  return index;
}
}  // namespace mirnext::analysis
