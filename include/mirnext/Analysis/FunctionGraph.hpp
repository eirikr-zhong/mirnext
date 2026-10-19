/* Licensed under the MIT License. Optimization boundaries are derived, never a second IR body. */
#pragma once
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Support/SparseBitSet.hpp"

namespace mirnext::analysis {
struct FunctionGraph {
  enum class Kind { Entry, Exit, Prologue, Body };
  struct Block {
    Kind kind; const ir::BasicBlock *ir = nullptr;
    // Keep CFG construction's address roots even if an LADDR later disappears.
    bool address_taken = false;
  };
  static constexpr std::size_t entry = 0, exit = 1;
  std::vector<Block> blocks;
  std::vector<Edge> edges;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::vector<std::size_t> preorder, rpost, order;
  std::vector<bool> back_edges;
  std::unordered_map<const ir::BasicBlock *, std::size_t> indices;
};
struct FunctionGraphAnalysis {
  using Result = FunctionGraph;
  static constexpr Dependency dependencies = ControlFlowAnalysis::dependencies;
  static mirnext::Result<Result> run(const ir::Function &, FunctionAnalysisManager &);
};
void enumerate(FunctionGraph &);
struct DominatorTree {
  // SIZE_MAX denotes a node not reached by DFS. MIR's initial universal facts
  // are retained for those nodes by dominates(), without quadratic storage.
  std::vector<std::size_t> immediate, begin, end;
  std::size_t iterations = 0;
  bool dominates(std::size_t ancestor, std::size_t block) const;
};
struct DominatorAnalysis {
  using Result = DominatorTree;
  static constexpr Dependency dependencies = FunctionGraphAnalysis::dependencies;
  static mirnext::Result<Result> run(const ir::Function &, FunctionAnalysisManager &);
};
struct RegisterLiveness {
  std::vector<const ir::Register *> registers;
  std::unordered_map<const ir::Register *, std::size_t> indices;
  std::vector<SparseBitSet> in, out, generated, killed;
  std::size_t transfers = 0;
  bool live_in(std::size_t block, const ir::Register &reg) const;
  bool live_out(std::size_t block, const ir::Register &reg) const;
};
struct LivenessAnalysis {
  using Result = RegisterLiveness;
  static constexpr Dependency dependencies = Dependency::Instructions | Dependency::ControlFlow | Dependency::Registers;
  static mirnext::Result<Result> run(const ir::Function &, FunctionAnalysisManager &);
};
struct LoopTree {
  struct Node { std::size_t block = SIZE_MAX, entry = SIZE_MAX, parent = SIZE_MAX; std::vector<std::size_t> children; };
  std::vector<Node> nodes;
  std::size_t root = SIZE_MAX;
  std::size_t depth(std::size_t block) const;
};
struct LoopAnalysis {
  using Result = LoopTree;
  static constexpr Dependency dependencies = FunctionGraphAnalysis::dependencies;
  static mirnext::Result<Result> run(const ir::Function &, FunctionAnalysisManager &);
};
} // namespace mirnext::analysis
