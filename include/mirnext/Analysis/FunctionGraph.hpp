/* Licensed under the MIT License. Optimization boundaries are derived, never a second IR body. */
#pragma once
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Support/SparseBitSet.hpp"

namespace mirnext::analysis {
namespace detail {
class NodeAccess;
}
struct FunctionGraph {
  enum class Kind { Entry, Exit, Prologue, Body };
  struct Block {
    Kind kind;
    Block (Kind kind, OptionalRef<const ir::BasicBlock> body = {}, bool address_taken = false)
      : kind (kind), ir_ (body.get ()), address_taken (address_taken) {}
    OptionalRef<const ir::BasicBlock> ir () const {
      return ::mirnext::detail::borrow_optional (ir_);
    }

   private:
    friend class detail::NodeAccess;
    const ir::BasicBlock *ir_ = nullptr;

   public:
    // Keep CFG construction's address roots even if an LADDR later disappears.
    bool address_taken = false;
  };
  static constexpr std::size_t entry = 0, exit = 1;
  std::vector<Block> blocks;
  std::vector<Edge> edges;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::vector<std::size_t> preorder, rpost, order;
  std::vector<bool> back_edges;
  std::optional<std::size_t> index (const ir::BasicBlock &block) const {
    auto at = indices.find (&block);
    return at == indices.end () ? std::nullopt : std::optional (at->second);
  }

 private:
  friend class detail::NodeAccess;
  std::unordered_map<const ir::BasicBlock *, std::size_t> indices;
};
struct FunctionGraphAnalysis {
  static const unsigned char *id () noexcept;
  using Result = FunctionGraph;
  static constexpr Dependency dependencies = ControlFlowAnalysis::dependencies;
  static mirnext::Result<Result> run (const ir::Function &, FunctionAnalysisManager &);
};
void enumerate (FunctionGraph &);
struct DominatorTree {
  // SIZE_MAX denotes a node not reached by DFS. MIR's initial universal facts
  // are retained for those nodes by dominates(), without quadratic storage.
  std::vector<std::size_t> immediate, begin, end;
  std::size_t iterations = 0;
  bool dominates (std::size_t ancestor, std::size_t block) const;
};
struct DominatorAnalysis {
  static const unsigned char *id () noexcept;
  using Result = DominatorTree;
  static constexpr Dependency dependencies = FunctionGraphAnalysis::dependencies;
  static mirnext::Result<Result> run (const ir::Function &, FunctionAnalysisManager &);
};
struct RegisterLiveness {
  ir::ReferenceRange<const ir::Register> registers () const {
    return ir::ReferenceRange<const ir::Register> (registers_);
  }

 private:
  friend class detail::NodeAccess;
  std::vector<const ir::Register *> registers_;

 public:
  std::optional<std::size_t> index (const ir::Register &block) const {
    auto at = indices.find (&block);
    return at == indices.end () ? std::nullopt : std::optional (at->second);
  }

 private:
  friend class detail::NodeAccess;
  std::unordered_map<const ir::Register *, std::size_t> indices;

 public:
  std::vector<SparseBitSet> in, out, generated, killed;
  std::size_t transfers = 0;
  bool live_in (std::size_t block, const ir::Register &reg) const;
  bool live_out (std::size_t block, const ir::Register &reg) const;
};
struct LivenessAnalysis {
  static const unsigned char *id () noexcept;
  using Result = RegisterLiveness;
  static constexpr Dependency dependencies
    = Dependency::Instructions | Dependency::ControlFlow | Dependency::Registers;
  static mirnext::Result<Result> run (const ir::Function &, FunctionAnalysisManager &);
};
struct LoopTree {
  struct Node {
    std::size_t block = SIZE_MAX, entry = SIZE_MAX, parent = SIZE_MAX;
    std::vector<std::size_t> children;
  };
  std::vector<Node> nodes;
  std::size_t root = SIZE_MAX;
  std::size_t depth (std::size_t block) const;
};
struct LoopAnalysis {
  static const unsigned char *id () noexcept;
  using Result = LoopTree;
  static constexpr Dependency dependencies = FunctionGraphAnalysis::dependencies;
  static mirnext::Result<Result> run (const ir::Function &, FunctionAnalysisManager &);
};
}  // namespace mirnext::analysis
