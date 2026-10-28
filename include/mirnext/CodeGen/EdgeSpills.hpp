/* Licensed under the MIT License. Edge preservation over owning machine IR. */
#pragma once
#include "mirnext/CodeGen/RegisterRewrite.hpp"
namespace mirnext::codegen {
namespace detail {
class NodeAccess;
}
struct MachineSpillPlacement {
  MachineSpillPlacement (const MachineVirtualRegister &reg, const MachineBasicBlock &block,
                         bool end = false, bool store = false)
    : MachineSpillPlacement (Key{}, &reg, &block, std::move (end), std::move (store)) {}
  const MachineVirtualRegister &reg () const { return *reg_; }
  const MachineBasicBlock &block () const { return *block_; }
  bool end = false;
  bool store = false;
  friend bool operator== (const MachineSpillPlacement &, const MachineSpillPlacement &) = default;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  MachineSpillPlacement (Key, const MachineVirtualRegister *reg, const MachineBasicBlock *block,
                         bool end, bool store)
    : end (end), store (store), reg_ (reg), block_ (block) {}
  const MachineVirtualRegister *reg_;
  const MachineBasicBlock *block_;
};
struct MachineEdgeSplit {
  MachineEdgeSplit (const MachineBasicBlock &block, MachineControlFlow cfg)
    : MachineEdgeSplit (Key{}, &block, std::move (cfg)) {}
  const MachineBasicBlock &block () const { return *block_; }
  MachineControlFlow cfg;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  MachineEdgeSplit (Key, const MachineBasicBlock *block, MachineControlFlow cfg)
    : cfg (std::move (cfg)), block_ (block) {}
  const MachineBasicBlock *block_;
};
// The edge index belongs to this checked CFG, never to another function.

class MachineSpillResult {
 public:
  bool current (const MachineFunction &f) const { return rewrite_.current (f) && cfg_.current (f); }
  const MachineRewriteResult &rewrite_result () const { return rewrite_; }
  const MachineAllocation &allocation () const { return rewrite_.allocation (); }
  const MachineControlFlow &cfg () const { return cfg_; }
  std::span<const MachineSpillPlacement> placements () const { return placements_; }

 private:
  friend struct detail::EdgeSpillEngine;
  friend struct detail::PostAllocationEngine;
  explicit MachineSpillResult (MachineRewriteResult r) : rewrite_ (std::move (r)) {}
  MachineRewriteResult rewrite_;
  MachineControlFlow cfg_;
  std::vector<MachineSpillPlacement> placements_;
};
// O0/O1 retain the original no-split condition. O2/O3 use the original edge
// grouping, common-boundary selection, split/reuse and insertion order.

}  // namespace mirnext::codegen
