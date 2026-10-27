/* Licensed under the MIT License. Native CFG construction ranks used by clone heuristics. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"
namespace mirnext::analysis {
struct ConstructionOrder {
  struct Block {
    std::size_t rank, head;
  };
  std::unordered_map<const ir::BasicBlock *, Block> blocks;
  std::unordered_map<const ir::Label *, std::size_t> labels;
  std::unordered_map<const ir::Instruction *, std::size_t> instructions;
  std::size_t next_instruction = 1, next_block = 2;
};
struct ConstructionOrderAnalysis {
  static const unsigned char *id () noexcept;
  using Result = ConstructionOrder;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const ir::Function &);
};
}  // namespace mirnext::analysis
