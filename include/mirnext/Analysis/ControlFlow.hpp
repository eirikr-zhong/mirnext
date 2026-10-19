/* Licensed under the MIT License. Derived CFG; the IR remains the sole owner of blocks. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"

namespace mirnext::analysis {
enum class EdgeKind { Branch, Fallthrough, SwitchCase, Indirect, Entry, Exit };
struct Edge {
  std::size_t source, destination;
  EdgeKind kind;
  std::size_t operand = SIZE_MAX;
};
struct ControlFlowGraph {
  std::vector<const ir::BasicBlock *> blocks;
  std::vector<Edge> edges;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::vector<std::size_t> reverse_postorder;
  std::vector<std::size_t> entry_roots;
  std::vector<bool> reachable;
  std::unordered_map<const ir::BasicBlock *, std::size_t> indices;
};
struct ControlFlowAnalysis {
  using Result = ControlFlowGraph;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run(const ir::Function &);
};
struct RegisterOccurrence {
  enum class Part { Value, MemoryBase, MemoryIndex };
  const ir::Instruction *instruction;
  std::size_t operand;
  Part part;
  bool definition;
};
struct RegisterUseIndex {
  std::unordered_map<const ir::Register *, std::vector<RegisterOccurrence>> occurrences;
};
struct RegisterUseAnalysis {
  using Result = RegisterUseIndex;
  static constexpr Dependency dependencies = Dependency::Instructions | Dependency::Registers;
  static mirnext::Result<Result> run(const ir::Function &);
};
} // namespace mirnext::analysis
