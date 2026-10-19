/* Licensed under the MIT License. Linear graph construction and nonrecursive traversal. */
#include "mirnext/Analysis/ControlFlow.hpp"
#include <algorithm>

namespace mirnext::analysis {
mirnext::Result<ControlFlowGraph> ControlFlowAnalysis::run(const ir::Function &function) {
  auto verified = function.verify(); if (!verified) return verified.error();
  ControlFlowGraph graph;
  for (auto &block : function.blocks()) { graph.indices.emplace(&block, graph.blocks.size()); graph.blocks.push_back(&block); }
  const auto count = graph.blocks.size();
  graph.incoming.resize(count); graph.outgoing.resize(count); graph.reachable.resize(count);
  auto edge = [&](std::size_t source, std::size_t destination, EdgeKind kind, std::size_t operand = SIZE_MAX) {
    auto id = graph.edges.size(); graph.edges.push_back({source, destination, kind, operand});
    graph.outgoing[source].push_back(id); graph.incoming[destination].push_back(id);
  };
  std::vector<bool> addressable(count);
  for (auto &block : function.blocks()) for (auto &instruction : block.instructions())
    if (instruction.opcode() == ir::Opcode::LAddr)
      addressable[graph.indices.at(&(*instruction.operands()[1].get<const ir::Label *>())->block())] = true;
  for (auto data : function.label_data_uses()) if (auto reference = std::get_if<ir::LabelReference>(&data->initializer())) {
    if (&reference->label->function() == &function) addressable[graph.indices.at(&reference->label->block())] = true;
    if (reference->base && &reference->base->function() == &function) addressable[graph.indices.at(&reference->base->block())] = true;
  }
  std::vector<std::size_t> indirect_targets;
  for (std::size_t b = 0; b < count; ++b) if (addressable[b]) indirect_targets.push_back(b);
  for (std::size_t b = 0; b < count; ++b) {
    const auto terminator = graph.blocks[b]->terminator();
    if (!terminator) { if (b + 1 < count) edge(b, b + 1, EdgeKind::Fallthrough); continue; }
    auto opcode = terminator->opcode(); auto operands = terminator->operands();
    if (opcode == ir::Opcode::Switch) {
      for (std::size_t n = 1; n < operands.size(); ++n)
        edge(b, graph.indices.at(&(*operands[n].get<const ir::Label *>())->block()), EdgeKind::SwitchCase, n);
    } else if (opcode == ir::Opcode::Jmp || ir::instruction_descriptor(opcode)->flow == ir::ControlFlow::Branch) {
      auto label = *operands[0].get<const ir::Label *>();
      if (opcode != ir::Opcode::Jmp && b + 1 < count) edge(b, b + 1, EdgeKind::Fallthrough);
      edge(b, graph.indices.at(&label->block()), EdgeKind::Branch, 0);
    }
  }
  // MIR appends indirect edges after all direct edges. Incoming order is also
  // PHI operand order, even when an earlier block contains the indirect jump.
  for (std::size_t b = 0; b < count; ++b)
    if (auto terminator = graph.blocks[b]->terminator(); terminator && terminator->opcode() == ir::Opcode::JmpIndirect)
      for (auto target : indirect_targets) edge(b, target, EdgeKind::Indirect);
  // An explicit stack avoids recursion proportional to the input's block count.
  struct Frame { std::size_t block, next; };
  graph.entry_roots.push_back(0);
  for (auto root : indirect_targets) if (root != 0) graph.entry_roots.push_back(root);
  for (auto root : graph.entry_roots) {
    if (graph.reachable[root]) continue;
    std::vector<Frame> stack{{root, 0}}; graph.reachable[root] = true;
    std::vector<std::size_t> postorder;
    while (!stack.empty()) {
      auto &frame = stack.back();
      if (frame.next == graph.outgoing[frame.block].size()) {
        postorder.push_back(frame.block); stack.pop_back(); continue;
      }
      auto target = graph.edges[graph.outgoing[frame.block][frame.next++]].destination;
      if (!graph.reachable[target]) { graph.reachable[target] = true; stack.push_back({target, 0}); }
    }
    graph.reverse_postorder.insert(graph.reverse_postorder.end(), postorder.rbegin(), postorder.rend());
  }
  return graph;
}
mirnext::Result<RegisterUseIndex> RegisterUseAnalysis::run(const ir::Function &function) {
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  auto verified = function.verify(); if (!verified) return verified.error();
  RegisterUseIndex index;
  for (auto &block : function.blocks()) for (auto &instruction : block.instructions()) {
    for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
      auto &operand = instruction.operands()[n];
      if (auto reg = operand.get<const ir::Register *>())
        index.occurrences[*reg].push_back({&instruction, n, RegisterOccurrence::Part::Value, ir::output_operand(function, instruction, n)});
      else if (auto memory = operand.get<ir::Memory>()) {
        if (memory->base) index.occurrences[memory->base].push_back({&instruction, n, RegisterOccurrence::Part::MemoryBase, false});
        if (memory->index) index.occurrences[memory->index].push_back({&instruction, n, RegisterOccurrence::Part::MemoryIndex, false});
      }
    }
  }
  return index;
}
} // namespace mirnext::analysis
