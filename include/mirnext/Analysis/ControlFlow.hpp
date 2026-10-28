/* Licensed under the MIT License. Derived CFG; the IR remains the sole owner of blocks. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"

namespace mirnext::analysis {
namespace detail {
class NodeAccess;
}
enum class EdgeKind { Branch, Fallthrough, SwitchCase, Indirect, Entry, Exit };
struct Edge {
  std::size_t source, destination;
  EdgeKind kind;
  std::size_t operand = SIZE_MAX;
};
struct ControlFlowGraph {
  ir::ReferenceRange<const ir::BasicBlock> blocks () const {
    return ir::ReferenceRange<const ir::BasicBlock> (blocks_);
  }

 private:
  friend class detail::NodeAccess;
  std::vector<const ir::BasicBlock *> blocks_;

 public:
  std::vector<Edge> edges;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::vector<std::size_t> reverse_postorder;
  std::vector<std::size_t> entry_roots;
  std::vector<bool> reachable;
  std::optional<std::size_t> index (const ir::BasicBlock &block) const {
    auto at = indices.find (&block);
    return at == indices.end () ? std::nullopt : std::optional (at->second);
  }

 private:
  friend class detail::NodeAccess;
  std::unordered_map<const ir::BasicBlock *, std::size_t> indices;
};
struct ControlFlowAnalysis {
  static const unsigned char *id () noexcept;
  using Result = ControlFlowGraph;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run (const ir::Function &);
};
struct RegisterOccurrence {
  enum class Part { Value, MemoryBase, MemoryIndex };
  RegisterOccurrence (const ir::Instruction &instruction, std::size_t operand, Part part,
                      bool definition)
    : instruction_ (&instruction), operand (operand), part (part), definition (definition) {}
  const ir::Instruction &instruction () const { return *instruction_; }

 private:
  const ir::Instruction *instruction_;

 public:
  std::size_t operand;
  Part part;
  bool definition;
};
struct RegisterUseIndex {
  std::span<const RegisterOccurrence> uses (const ir::Register &reg) const {
    auto at = occurrences.find (&reg);
    return at == occurrences.end () ? std::span<const RegisterOccurrence>{} : at->second;
  }

 private:
  friend class detail::NodeAccess;
  std::unordered_map<const ir::Register *, std::vector<RegisterOccurrence>> occurrences;
};
struct RegisterUseAnalysis {
  static const unsigned char *id () noexcept;
  using Result = RegisterUseIndex;
  static constexpr Dependency dependencies = Dependency::Instructions | Dependency::Registers;
  static mirnext::Result<Result> run (const ir::Function &);
};
}  // namespace mirnext::analysis
