/* Licensed under the MIT License. Allocation rewrite over owning machine nodes. */
#pragma once
#include "mirnext/CodeGen/RegisterAllocation.hpp"
namespace mirnext::codegen {
struct MachineRewriteBlock {
  const MachineBasicBlock *block = nullptr; // Null denotes an entry/exit boundary.
  SparseBitSet live_in, spill_gen, spill_kill;
};
class MachineRewriteResult {
public:
  bool current(const MachineFunction &f) const { return allocation_.current(f); }
  const MachineAllocation &allocation() const { return allocation_; }
  std::span<const MachineRewriteBlock> blocks() const { return blocks_; }
  Result<std::optional<StackLocation>> spill_slot(const MachineVirtualRegister &) const;
  std::size_t deleted_moves() const { return deleted_; }
private:
  friend struct detail::AllocationRewriteEngine;
  friend struct detail::MachineSpillEmitter;
  friend struct detail::EdgeSpillEngine;
  friend struct detail::PostAllocationEngine;
  explicit MachineRewriteResult(MachineAllocation allocation) : allocation_(std::move(allocation)) {}
  MachineAllocation allocation_;
  std::unordered_map<const MachineVirtualRegister *, StackLocation> spill_slots_;
  std::vector<MachineRewriteBlock> blocks_;
  std::size_t deleted_ = 0;
};
// The allocation is consumed; the returned result has the updated revision and
// spill area. Original analyses and any source allocation view become stale.
// This stage performs local/call preservation; edge spills are a separate stage.
Result<MachineRewriteResult> rewrite_allocated(MachineFunction &, const MachineGraphAnalysis &,
  const MachineLiveness &, const MachineLiveRanges &, MachineAllocation);
}
