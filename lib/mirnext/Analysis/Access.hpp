/* Licensed under the MIT License. Private analysis storage access. */
#pragma once
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Analysis/ConstructionOrder.hpp"
namespace mirnext::analysis::detail {
class NodeAccess {
 public:
  static auto &blocks (ConstructionOrder &value) { return value.blocks; }
  static const auto &blocks (const ConstructionOrder &value) { return value.blocks; }
  static auto &labels (ConstructionOrder &value) { return value.labels; }
  static const auto &labels (const ConstructionOrder &value) { return value.labels; }
  static auto &instructions (ConstructionOrder &value) { return value.instructions; }
  static const auto &instructions (const ConstructionOrder &value) { return value.instructions; }
  static auto &indices (ControlFlowGraph &value) { return value.indices; }
  static const auto &indices (const ControlFlowGraph &value) { return value.indices; }
  static auto &indices (FunctionGraph &value) { return value.indices; }
  static const auto &indices (const FunctionGraph &value) { return value.indices; }
  static auto &indices (RegisterLiveness &value) { return value.indices; }
  static const auto &indices (const RegisterLiveness &value) { return value.indices; }
  static auto &occurrences (RegisterUseIndex &value) { return value.occurrences; }
  static const auto &occurrences (const RegisterUseIndex &value) { return value.occurrences; }

  static const ir::BasicBlock *ir (const FunctionGraph::Block &block) { return block.ir_; }
  static const ir::BasicBlock *&ir (FunctionGraph::Block &block) { return block.ir_; }
  static auto &blocks (ControlFlowGraph &value) { return value.blocks_; }
  static const auto &blocks (const ControlFlowGraph &value) { return value.blocks_; }
  static auto &registers (RegisterLiveness &value) { return value.registers_; }
  static const auto &registers (const RegisterLiveness &value) { return value.registers_; }
};
}  // namespace mirnext::analysis::detail
