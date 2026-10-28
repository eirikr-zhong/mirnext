/* Licensed under the MIT License. Allocation preparation over owning machine IR. */
#pragma once
#include "mirnext/CodeGen/Analysis.hpp"

namespace mirnext::codegen {
namespace detail {
class NodeAccess;
}
namespace detail {
struct AllocationRewriteEngine;
struct MachineSpillEmitter;
struct EdgeSpillEngine;
struct PostAllocationEngine;
}  // namespace detail
class MachineCoalescing {
 public:
  bool current (const MachineFunction &f) const {
    return owner_ == &f && revision_ == f.revision ();
  }
  bool enabled () const { return enabled_; }
  std::size_t selected_moves () const { return selected_; }
  std::size_t coalesced_moves () const { return coalesced_; }
  std::size_t removed_moves () const { return removed_; }
  std::span<const MachineRegister> registers () const { return registers_; }
  Result<MachineRegister> representative (MachineRegister) const;
  std::span<const MachineRegister> scan_registers () const { return scan_; }
  Result<bool> conflicts (MachineRegister, MachineRegister) const;

 private:
  MachineCoalescing () = default;
  friend Result<MachineCoalescing> coalesce_registers (MachineFunction &,
                                                       const MachineGraphAnalysis &, unsigned,
                                                       std::size_t);
  const MachineFunction *owner_ = nullptr;
  std::uint64_t revision_ = 0;
  bool enabled_ = false;
  std::size_t selected_ = 0, coalesced_ = 0, removed_ = 0;
  std::vector<MachineRegister> registers_, representatives_, scan_;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtuals_, compact_;
  std::unordered_map<unsigned, std::size_t> physicals_;
  std::vector<std::uint64_t> conflicts_;
};
// Levels and the register-scan threshold retain the original backend conditions.
// The result borrows register identities; the function must outlive it.

struct MachineLiveRange {
  int start = 0, finish = 0, reference_cost = 1;
  MachineLiveRange () = default;
  MachineLiveRange (int start, int finish, int reference_cost = 1)
    : start (start), finish (finish), reference_cost (reference_cost) {}
  ir::ReferenceRange<const MachineBasicBlock> blocks () const {
    return ir::ReferenceRange<const MachineBasicBlock> (blocks_);
  }
  friend bool operator== (const MachineLiveRange &, const MachineLiveRange &) = default;

 private:
  friend class detail::NodeAccess;
  std::vector<const MachineBasicBlock *> blocks_;
};
struct MachineSpillBlock {
  SparseBitSet gen, kill;
};
class MachineLiveRanges {
 public:
  static Result<MachineLiveRanges> build (const MachineFunction &, const MachineGraphAnalysis &,
                                          const MachineLiveness &);
  bool current (const MachineFunction &f) const {
    return owner_ == &f && revision_ == f.revision ();
  }
  std::span<const MachineRegister> registers () const { return registers_; }
  // Call-crossed bits index registers(); spill bits index scan_registers().
  // Ordinary allocation uses the same complete universe for both.
  std::span<const MachineRegister> scan_registers () const {
    return compact_ ? std::span<const MachineRegister> (scan_)
                    : std::span<const MachineRegister> (registers_);
  }
  bool is_compact () const { return compact_; }
  unsigned optimization_level () const { return level_; }
  std::span<const target::PhysicalRegister> global_registers () const { return globals_; }
  const SparseBitSet &call_crossed () const { return crossed_; }
  std::span<const MachineSpillBlock> spill_blocks () const { return spill_; }
  std::span<const std::vector<MachineLiveRange>> all_ranges () const { return *ranges_; }
  std::span<const std::vector<MachineLiveRange>> uncompressed_ranges () const { return raw_; }
  Result<std::span<const MachineLiveRange>> ranges (MachineRegister) const;
  int point_count () const { return points_; }
  int uncompressed_point_count () const { return raw_points_; }
  std::span<const int> point_map () const { return point_map_; }

 private:
  MachineLiveRanges () = default;
  friend class MachineAllocation;
  const MachineFunction *owner_ = nullptr;
  std::uint64_t revision_ = 0;
  std::vector<MachineRegister> registers_;
  std::vector<MachineRegister> scan_;
  bool compact_ = false;
  unsigned level_ = 0;
  std::vector<target::PhysicalRegister> globals_;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtuals_;
  std::unordered_map<unsigned, std::size_t> physicals_;
  std::shared_ptr<std::vector<std::vector<MachineLiveRange>>> ranges_
    = std::make_shared<std::vector<std::vector<MachineLiveRange>>> ();
  std::vector<std::vector<MachineLiveRange>> raw_;
  std::vector<MachineSpillBlock> spill_;
  SparseBitSet crossed_;
  std::vector<int> point_map_;
  int points_ = 0, raw_points_ = 0;
};

// Offsets belong to the allocation's spill area, before frame construction.
// They are bytes, never values in a physical-register numbering interval.
struct StackLocation {
  std::size_t offset = 0, size = 8, alignment = 8;
  friend bool operator== (StackLocation, StackLocation) = default;
};
using MachineLocation = std::variant<std::monostate, target::PhysicalRegister, StackLocation>;
struct MachinePointLocations {
  // Physical bits index TargetMachine::registers(); stack bits index 8-byte units
  // in the spill area. The two location domains never share an index space.
  SparseBitSet physical, stack;
};
struct MachineAllocationGap {
  MachineAllocationGap (target::PhysicalRegister physical, const MachineVirtualRegister &reg,
                        MachineLiveRange range)
    : MachineAllocationGap (Key{}, std::move (physical), &reg, std::move (range)) {}
  target::PhysicalRegister physical;
  const MachineVirtualRegister &reg () const { return *reg_; }
  MachineLiveRange range;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  MachineAllocationGap (Key, target::PhysicalRegister physical, const MachineVirtualRegister *reg,
                        MachineLiveRange range)
    : physical (physical), range (range), reg_ (reg) {}
  const MachineVirtualRegister *reg_;
};
class MachineAllocation {
 public:
  static Result<MachineAllocation> assign (const MachineFunction &, const MachineGraphAnalysis &,
                                           const MachineLiveness &, const MachineLiveRanges &);
  bool current (const MachineFunction &f) const {
    return owner_ == &f && revision_ == f.revision ();
  }
  unsigned optimization_level () const { return level_; }
  std::span<const target::PhysicalRegister> global_registers () const { return globals_; }
  Result<MachineLocation> location (const MachineVirtualRegister &) const;
  Result<std::size_t> live_length (const MachineVirtualRegister &) const;
  ir::ReferenceRange<const MachineVirtualRegister> order () const {
    return ir::ReferenceRange<const MachineVirtualRegister> (order_);
  }
  std::span<const MachinePointLocations> used () const { return used_; }
  std::span<const MachinePointLocations> busy () const { return busy_; }
  std::span<const target::PhysicalRegister> used_registers () const { return used_registers_; }
  // Spill bits index registers(), the full typed live-range universe.
  std::span<const MachineRegister> registers () const { return registers_; }
  std::span<const MachineSpillBlock> spill_blocks () const { return spill_; }
  std::span<const MachineAllocationGap> gaps () const { return gaps_; }
  std::size_t stack_bytes () const { return stack_bytes_; }
  ir::ReferenceRange<const MachineBasicBlock> loop_alignment_blocks () const {
    return ir::ReferenceRange<const MachineBasicBlock> (aligned_loops_);
  }
  std::size_t reusable_stack_offset () const { return reusable_stack_; }

 private:
  friend struct detail::AllocationRewriteEngine;
  friend struct detail::MachineSpillEmitter;
  friend struct detail::EdgeSpillEngine;
  friend struct detail::PostAllocationEngine;
  MachineAllocation () = default;
  const MachineFunction *owner_ = nullptr;
  std::uint64_t revision_ = 0;
  unsigned level_ = 0;
  std::vector<target::PhysicalRegister> globals_;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtuals_;
  std::vector<MachineRegister> registers_;
  std::vector<MachineLocation> locations_;
  std::vector<std::size_t> lengths_;
  std::vector<const MachineVirtualRegister *> order_;
  std::vector<MachinePointLocations> used_, busy_;
  std::vector<target::PhysicalRegister> used_registers_;
  std::vector<MachineSpillBlock> spill_;
  std::vector<MachineAllocationGap> gaps_;
  std::vector<const MachineBasicBlock *> aligned_loops_;
  // Shared immutable facts remain valid through allocation's destructive stages.
  std::shared_ptr<const std::vector<std::vector<MachineLiveRange>>> live_ranges_;
  std::size_t stack_bytes_ = 0, reusable_stack_ = 0;
};
}  // namespace mirnext::codegen
