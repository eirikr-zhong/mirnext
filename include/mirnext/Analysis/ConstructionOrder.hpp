/* Licensed under the MIT License. Native CFG construction ranks used by clone heuristics. */
#pragma once
#include "mirnext/Analysis/AnalysisManager.hpp"
namespace mirnext::analysis {
namespace detail {
class NodeAccess;
}
struct ConstructionOrder {
  struct Block {
    std::size_t rank, head;
  };
  OptionalRef<const Block> block (const ir::BasicBlock &value) const {
    auto at = blocks.find (&value);
    return at == blocks.end () ? OptionalRef<const Block>{} : OptionalRef<const Block> (at->second);
  }
  std::optional<std::size_t> label (const ir::Label &value) const {
    auto at = labels.find (&value);
    return at == labels.end () ? std::nullopt : std::optional (at->second);
  }
  std::optional<std::size_t> instruction (const ir::Instruction &value) const {
    auto at = instructions.find (&value);
    return at == instructions.end () ? std::nullopt : std::optional (at->second);
  }

 private:
  friend class detail::NodeAccess;
  std::unordered_map<const ir::BasicBlock *, Block> blocks;

  std::unordered_map<const ir::Label *, std::size_t> labels;

  std::unordered_map<const ir::Instruction *, std::size_t> instructions;

 public:
  std::size_t next_instruction = 1, next_block = 2;
};
struct ConstructionOrderAnalysis {
  static const unsigned char *id () noexcept;
  using Result = ConstructionOrder;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const ir::Function &);
};
}  // namespace mirnext::analysis
