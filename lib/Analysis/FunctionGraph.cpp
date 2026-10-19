/* Licensed under the MIT License. MIR traversal, dominance, liveness and natural loops. */
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <algorithm>
#include <numeric>

namespace mirnext::analysis {
namespace {
Error owner_error(const ir::Function &function) {
  return {ErrorCode::InvalidArgument, std::string(function.symbol().name()) + ": analysis owner mismatch"};
}
}
mirnext::Result<FunctionGraph> FunctionGraphAnalysis::run(const ir::Function &function, FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return owner_error(function);
  auto source = analyses.get<ControlFlowAnalysis>(); if (!source) return source.error();
  FunctionGraph graph;
  graph.blocks.push_back({FunctionGraph::Kind::Entry}); graph.blocks.push_back({FunctionGraph::Kind::Exit});
  // The original generator prepends a label. A pre-existing first label starts
  // another block; represent that empty prologue without mutating public IR.
  const bool prologue = !(*source)->blocks.front()->labels().empty();
  if (prologue) graph.blocks.push_back({FunctionGraph::Kind::Prologue});
  const auto offset = graph.blocks.size();
  for (auto block : (*source)->blocks) {
    graph.indices.emplace(block, graph.blocks.size()); graph.blocks.push_back({FunctionGraph::Kind::Body, block});
  }
  for (auto &block : function.blocks()) for (auto &instruction : block.instructions())
    if (instruction.opcode() == ir::Opcode::LAddr)
      graph.blocks[graph.indices.at(&(*instruction.operands()[1].get<const ir::Label *>())->block())].address_taken = true;
  for (auto data : function.label_data_uses()) if (auto ref = std::get_if<ir::LabelReference>(&data->initializer())) {
    if (&ref->label->function() == &function) graph.blocks[graph.indices.at(&ref->label->block())].address_taken = true;
    if (ref->base && &ref->base->function() == &function) graph.blocks[graph.indices.at(&ref->base->block())].address_taken = true;
  }
  const auto count = graph.blocks.size(); graph.incoming.resize(count); graph.outgoing.resize(count);
  auto edge = [&](std::size_t from, std::size_t to, EdgeKind kind, std::size_t operand = SIZE_MAX) {
    auto id = graph.edges.size(); graph.edges.push_back({from, to, kind, operand});
    graph.outgoing[from].push_back(id); graph.incoming[to].push_back(id);
  };
  if (prologue) edge(2, offset, EdgeKind::Fallthrough);
  for (auto &e : (*source)->edges) edge(e.source + offset, e.destination + offset, e.kind, e.operand);
  // Layout order is observable to DFS/SSA: entry -> exit is added before any
  // additional entry edges, and return -> exit edges follow the body edges.
  for (std::size_t b = 0; b < count; ++b) {
    if (b != FunctionGraph::entry && graph.incoming[b].empty()) edge(FunctionGraph::entry, b, EdgeKind::Entry);
    if (b != FunctionGraph::exit && graph.outgoing[b].empty()) edge(b, FunctionGraph::exit, EdgeKind::Exit);
  }
  enumerate(graph); return graph;
}
void enumerate(FunctionGraph &graph) {
  const auto count = graph.blocks.size();
  graph.preorder.assign(count, 0); graph.rpost.assign(count, 0); graph.back_edges.assign(graph.edges.size(), false);
  struct Frame { std::size_t block, next; };
  std::vector<Frame> stack{{FunctionGraph::entry, 0}}; std::size_t pre = 1, post = count;
  graph.preorder[FunctionGraph::entry] = pre++;
  while (!stack.empty()) {
    auto &frame = stack.back();
    if (frame.next == graph.outgoing[frame.block].size()) {
      graph.rpost[frame.block] = post--; stack.pop_back(); continue;
    }
    auto id = graph.outgoing[frame.block][frame.next++]; auto target = graph.edges[id].destination;
    if (!graph.preorder[target]) { graph.preorder[target] = pre++; stack.push_back({target, 0}); }
    else if (!graph.rpost[target]) graph.back_edges[id] = true;
  }
  graph.order.resize(count); std::iota(graph.order.begin(), graph.order.end(), 0);
  std::stable_sort(graph.order.begin(), graph.order.end(), [&](auto a, auto b) { return graph.rpost[a] < graph.rpost[b]; });
}
bool DominatorTree::dominates(std::size_t ancestor, std::size_t block) const {
  if (ancestor >= immediate.size() || block >= immediate.size()) return false;
  if (immediate[block] == SIZE_MAX) return true;
  return immediate[ancestor] != SIZE_MAX && begin[ancestor] <= begin[block] && end[block] <= end[ancestor];
}
mirnext::Result<DominatorTree> DominatorAnalysis::run(const ir::Function &function, FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return owner_error(function);
  auto result = analyses.get<FunctionGraphAnalysis>(); if (!result) return result.error(); auto &graph = **result;
  const auto count = graph.blocks.size(); DominatorTree tree;
  tree.immediate.assign(count, SIZE_MAX); tree.begin.resize(count); tree.end.resize(count); tree.immediate[0] = 0;
  auto intersect = [&](auto a, auto b) {
    while (a != b) {
      while (graph.rpost[a] > graph.rpost[b]) a = tree.immediate[a];
      while (graph.rpost[b] > graph.rpost[a]) b = tree.immediate[b];
    }
    return a;
  };
  bool changed;
  do {
    changed = false; ++tree.iterations;
    for (auto b : graph.order) if (b && graph.rpost[b]) {
      std::size_t parent = SIZE_MAX;
      for (auto id : graph.incoming[b]) {
        auto pred = graph.edges[id].source; if (tree.immediate[pred] == SIZE_MAX) continue;
        parent = parent == SIZE_MAX ? pred : intersect(parent, pred);
      }
      if (tree.immediate[b] != parent) { tree.immediate[b] = parent; changed = true; }
    }
  } while (changed);
  std::vector<std::vector<std::size_t>> children(count);
  for (std::size_t b = 1; b < count; ++b) if (tree.immediate[b] != SIZE_MAX) children[tree.immediate[b]].push_back(b);
  struct Frame { std::size_t block, next; };
  std::vector<Frame> stack{{0, 0}}; std::size_t time = 1; tree.begin[0] = time++;
  while (!stack.empty()) {
    auto &frame = stack.back();
    if (frame.next == children[frame.block].size()) { tree.end[frame.block] = time++; stack.pop_back(); }
    else { auto child = children[frame.block][frame.next++]; tree.begin[child] = time++; stack.push_back({child, 0}); }
  }
  return tree;
}
bool RegisterLiveness::live_in(std::size_t block, const ir::Register &reg) const {
  auto found = indices.find(&reg); return block < in.size() && found != indices.end() && in[block].contains(found->second);
}
bool RegisterLiveness::live_out(std::size_t block, const ir::Register &reg) const {
  auto found = indices.find(&reg); return block < out.size() && found != indices.end() && out[block].contains(found->second);
}
mirnext::Result<RegisterLiveness> LivenessAnalysis::run(const ir::Function &function, FunctionAnalysisManager &analyses) {
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  if (&analyses.function() != &function) return owner_error(function);
  auto result = analyses.get<FunctionGraphAnalysis>(); if (!result) return result.error(); auto &graph = **result;
  RegisterLiveness live; const auto count = graph.blocks.size();
  live.in.resize(count); live.out.resize(count); live.generated.resize(count); live.killed.resize(count);
  std::vector<const ir::Register *> hardware;
  for (auto &reg : function.registers()) {
    live.indices.emplace(&reg, live.registers.size()); live.registers.push_back(&reg);
    if (!reg.hardware_binding().empty() && !reg.is_memory_backed()) hardware.push_back(&reg);
  }
  // MIR's optimization iterator classifies LADDR's destination as an input.
  // Keep that scan decision here; public IR retains the semantic output role.
  auto output = [&](const ir::Instruction &insn, std::size_t p) {
    return insn.opcode() != ir::Opcode::LAddr && ir::output_operand(function, insn, p);
  };
  for (std::size_t b = 0; b < count; ++b) if (auto block = graph.blocks[b].ir) {
    auto input = [&](const ir::Register *reg) {
      if (reg) { auto id = live.indices.at(reg); if (!live.killed[b].contains(id)) live.generated[b].insert(id); }
    };
    for (auto &insn : block->instructions()) {
      // Hardware outputs are observable at every ordinary return. This is a
      // typed analysis boundary, not an instruction pretending to be a NOP.
      if (insn.opcode() == ir::Opcode::Ret) for (auto reg : hardware) input(reg);
      for (std::size_t p = 0; p < insn.operands().size(); ++p) {
        auto &operand = insn.operands()[p];
        if (auto reg = operand.get<const ir::Register *>(); reg && !output(insn, p)) input(*reg);
        else if (auto memory = operand.get<ir::Memory>()) { input(memory->base); input(memory->index); }
      }
      for (std::size_t p = 0; p < insn.operands().size(); ++p)
        if (auto reg = insn.operands()[p].get<const ir::Register *>(); reg && output(insn, p))
          live.killed[b].insert(live.indices.at(*reg));
    }
  }
  // In/out start empty and only grow. Accumulate outgoing facts and enqueue
  // predecessors only when their input facts can change.
  std::vector<std::size_t> queue(graph.order.rbegin(), graph.order.rend());
  // Deduplication limits pending blocks to count. A fixed ring retains FIFO
  // order without keeping every processed entry through convergence.
  queue.push_back(0); std::size_t head = 0, tail = count, pending = count;
  std::vector<bool> queued(count, true);
  while (pending) {
    auto b = queue[head]; if (++head == queue.size()) head = 0; --pending;
    queued[b] = false; ++live.transfers;
    for (auto id : graph.outgoing[b]) live.out[b].union_with(live.in[graph.edges[id].destination]);
    auto next = live.out[b]; next.subtract(live.killed[b]); next.union_with(live.generated[b]);
    if (next == live.in[b]) continue;
    live.in[b] = std::move(next);
    for (auto id : graph.incoming[b]) {
      auto pred = graph.edges[id].source;
      if (!queued[pred]) {
        queued[pred] = true; queue[tail] = pred; if (++tail == queue.size()) tail = 0; ++pending;
      }
    }
  }
  return live;
}
std::size_t LoopTree::depth(std::size_t block) const {
  if (block >= nodes.size() || nodes[block].block != block) return 0;
  std::size_t depth = 0;
  for (auto parent = nodes[block].parent; parent != SIZE_MAX && parent != root; parent = nodes[parent].parent) ++depth;
  return depth;
}
mirnext::Result<LoopTree> LoopAnalysis::run(const ir::Function &function, FunctionAnalysisManager &analyses) {
  if (&analyses.function() != &function) return owner_error(function);
  auto result = analyses.get<FunctionGraphAnalysis>(); if (!result) return result.error(); auto &graph = **result;
  LoopTree tree; const auto count = graph.blocks.size(); tree.nodes.reserve(2 * count + 1);
  std::vector<std::size_t> roots(count); std::iota(roots.begin(), roots.end(), 0);
  auto top = [&](std::size_t node) {
    auto root = node; while (roots[root] != root) root = roots[root];
    while (roots[node] != node) { auto next = roots[node]; roots[node] = root; node = next; }
    return root;
  };
  std::vector<std::size_t> headers;
  for (std::size_t b = 0; b < count; ++b) {
    tree.nodes.push_back({b, b, SIZE_MAX, {}});
    for (auto id : graph.incoming[b]) if (graph.back_edges[id]) { headers.push_back(b); break; }
  }
  std::stable_sort(headers.begin(), headers.end(), [&](auto a, auto b) { return graph.rpost[a] > graph.rpost[b]; });
  std::vector<std::size_t> seen(2 * count + 1, SIZE_MAX);
  for (auto header : headers) {
    std::vector<std::size_t> members, pending;
    auto add = [&](auto b) {
      auto node = top(b); if (!graph.preorder[b] || seen[node] == header) return;
      seen[node] = header; members.push_back(node); pending.push_back(node);
    };
    for (auto id : graph.incoming[header]) {
      auto pred = graph.edges[id].source; if (pred != header && graph.back_edges[id]) add(pred);
    }
    bool multiple_entries = false;
    while (!pending.empty()) {
      auto node = pending.back(); pending.pop_back(); auto b = tree.nodes[node].block;
      if (b == SIZE_MAX) b = tree.nodes[tree.nodes[node].entry].block;
      if (b == FunctionGraph::entry) { multiple_entries = true; break; }
      for (auto id : graph.incoming[b]) { auto pred = graph.edges[id].source; if (pred != header) add(pred); }
    }
    if (multiple_entries) continue;
    members.push_back(header); auto loop = tree.nodes.size();
    tree.nodes.push_back({SIZE_MAX, header, SIZE_MAX, {}}); roots.push_back(loop);
    for (auto n = members.rbegin(); n != members.rend(); ++n) {
      tree.nodes[loop].children.push_back(*n); tree.nodes[*n].parent = loop; roots[*n] = loop;
    }
  }
  tree.root = tree.nodes.size(); tree.nodes.push_back({});
  for (std::size_t b = 0; b < count; ++b) {
    auto node = top(b); if (tree.nodes[node].parent == tree.root) continue;
    tree.nodes[tree.root].children.push_back(node); tree.nodes[node].parent = tree.root;
  }
  return tree;
}
} // namespace mirnext::analysis
