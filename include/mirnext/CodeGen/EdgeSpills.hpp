/* Licensed under the MIT License. Edge preservation over owning machine IR. */
#pragma once
#include "mirnext/CodeGen/RegisterRewrite.hpp"
namespace mirnext::codegen {
struct MachineSpillPlacement {
  const MachineVirtualRegister *reg;
  const MachineBasicBlock *block;
  bool end = false, store = false;
  friend bool operator==(const MachineSpillPlacement &, const MachineSpillPlacement &) = default;
};
struct MachineEdgeSplit {
  const MachineBasicBlock *block;
  MachineControlFlow cfg;
};
// The edge index belongs to this checked CFG, never to another function.
Result<MachineEdgeSplit> split_machine_edge(MachineFunction &, const MachineControlFlow &, std::size_t edge);
class MachineSpillResult {
public:
  bool current(const MachineFunction &f) const { return rewrite_.current(f) && cfg_.current(f); }
  const MachineRewriteResult &rewrite_result() const { return rewrite_; }
  const MachineAllocation &allocation() const { return rewrite_.allocation(); }
  const MachineControlFlow &cfg() const { return cfg_; }
  std::span<const MachineSpillPlacement> placements() const { return placements_; }
private:
  friend struct detail::EdgeSpillEngine;
  friend struct detail::PostAllocationEngine;
  explicit MachineSpillResult(MachineRewriteResult r) : rewrite_(std::move(r)) {}
  MachineRewriteResult rewrite_;
  MachineControlFlow cfg_;
  std::vector<MachineSpillPlacement> placements_;
};
// O0/O1 retain the original no-split condition. O2/O3 use the original edge
// grouping, common-boundary selection, split/reuse and insertion order.
Result<MachineSpillResult> split_allocated_spills(MachineFunction &, MachineRewriteResult);
}
