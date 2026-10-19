/* Live-range construction derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. No transported instruction or CFG body. */
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/Detail/RegisterIndex.hpp"
#include "mirnext/CodeGen/Detail/RegisterEffects.hpp"
#include <algorithm>
#include <climits>
#include <numeric>

namespace mirnext::codegen {
namespace {
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", machine-live-ranges]: " + std::move (why)};
}
}  // namespace
Result<std::span<const MachineLiveRange>> MachineLiveRanges::ranges (MachineRegister r) const {
  if (!current (*owner_)) return invalid (*owner_, "stale live ranges");
  if (auto v = std::get_if<const MachineVirtualRegister *> (&r)) {
    auto at = virtuals_.find (*v);
    if (at != virtuals_.end ()) return std::span<const MachineLiveRange> ((*ranges_)[at->second]);
  } else {
    auto p = std::get<target::PhysicalRegister> (r);
    auto at = physicals_.find (p.encoding ());
    if (at != physicals_.end () && registers_[at->second] == r)
      return std::span<const MachineLiveRange> ((*ranges_)[at->second]);
  }
  return invalid (*owner_, "foreign register");
}
Result<MachineLiveRanges> MachineLiveRanges::build (const MachineFunction &f,
                                                    const MachineGraphAnalysis &graph,
                                                    const MachineLiveness &live) {
  if (!graph.current (f) || !live.current (f)) return invalid (f, "stale or foreign CFG/liveness");
  auto valid = f.verify ();
  if (!valid) return valid.error ();
  detail::RegisterIndex index (f);
  MachineLiveRanges result;
  result.owner_ = &f;
  result.revision_ = f.revision ();
  result.registers_ = index.values;
  result.virtuals_ = index.virtuals;
  result.physicals_ = index.physicals;
  result.compact_ = live.is_compact ();
  if (result.compact_) result.scan_.assign (live.registers ().begin (), live.registers ().end ());
  result.level_ = live.optimization_level ();
  result.globals_.assign (live.global_registers ().begin (), live.global_registers ().end ());
  auto &storage = *result.ranges_;
  storage.resize (index.values.size ());
  for (auto &b : live.blocks ()) result.spill_.push_back ({b.gen, b.kill});
  std::vector<std::optional<std::size_t>> scans (index.values.size ());
  std::vector<std::size_t> full;
  full.reserve (live.registers ().size ());
  for (auto r : live.registers ()) {
    auto p = *index.find (r);
    scans[p] = full.size ();
    full.push_back (p);
  }
  const bool compact = live.is_compact ();
  SparseBitSet living, referenced;
  int point = 0;
  auto advance = [&] {
    if (point == INT_MAX) return false;
    ++point;
    return true;
  };
  auto dead = [&] (std::size_t r, std::size_t s, bool instruction) {
    if (instruction && !compact) referenced.insert (r);
    auto &ranges = storage[r];
    if (living.contains (s)) {
      living.erase (s);
      ranges.back ().finish = point;
    } else
      ranges.push_back ({point, point, 1, {}});
    return true;
  };
  auto born = [&] (std::size_t r, std::size_t s, bool instruction) {
    if (instruction && !compact) referenced.insert (r);
    if (living.contains (s)) return false;
    living.insert (s);
    auto &ranges = storage[r];
    if (!instruction || ranges.empty () || !ranges.back ().blocks.empty ()
        || (ranges.back ().finish != point && ranges.back ().finish != point - 1))
      ranges.push_back ({point, -1, 1, {}});
    return true;
  };
  auto count = graph.cfg ().blocks.size ();
  std::vector<std::size_t> order (count - 2);
  std::iota (order.begin (), order.end (), 2);
  if (live.optimization_level () == 1)
    std::stable_sort (order.begin (), order.end (), [&] (auto a, auto b) {
      return graph.reverse_postorder ()[a] > graph.reverse_postorder ()[b];
    });
  // O0 follows physical layout. The canonical CFG stores that same layout for
  // real blocks; O2/O3 retain it as the original allocator does.
  for (auto b : order) {
    auto block = graph.cfg ().blocks[b];
    auto body = block->instructions ();
    if (body.begin () == body.end () && block->labels ().empty ()) continue;
    living.clear ();
    referenced.clear ();
    live.blocks ()[b].out.for_each ([&] (auto s) { born (full[s], s, false); });
    for (auto at = body.end (); at != body.begin ();) {
      auto &i = *--at;
      bool increment = false;
      detail::visit_registers (i, true, [&] (auto reg) {
        auto r = *index.find (reg);
        if (scans[r]) increment |= dead (r, *scans[r], true);
      });
      if (!compact && detail::is_call (i.opcode ())) {
        if (increment && !advance ()) return invalid (f, "live point overflow");
        increment = false;
        for (auto &p : f.target ().registers ())
          if (f.target ().call_clobbers (p.reg, ir::Type (ir::Type::Kind::Undef))) {
            auto r = *index.find (p.reg);
            dead (r, *scans[r], true);
            increment = true;
          }
        // Global bindings are implicit call inputs even when caller-clobbered.
        // Match CFG liveness, preserving their value before the call boundary.
        for (auto p : live.global_registers ()) {
          auto r = *index.find (p);
          born (r, *scans[r], true);
        }
        for (auto p : i.call_arguments ()) {
          auto r = *index.find (p);
          born (r, *scans[r], true);
        }
        living.for_each ([&] (auto s) {
          if (std::holds_alternative<const MachineVirtualRegister *> (index.values[full[s]]))
            result.crossed_.insert (full[s]);
        });
      }
      if (increment && !advance ()) return invalid (f, "live point overflow");
      increment = false;
      detail::visit_registers (i, false, [&] (auto reg) {
        auto r = *index.find (reg);
        if (scans[r]) increment |= born (r, *scans[r], true);
      });
      if (!compact)
        for (auto p : early_clobbers (f.target (), i.opcode ()))
          if (p) {
            auto r = *index.find (*p);
            increment |= born (r, *scans[r], true);
            increment |= dead (r, *scans[r], true);
          }
      if (increment && !advance ()) return invalid (f, "live point overflow");
    }
    if (living != live.blocks ()[b].in)
      return invalid (f, "live-in disagrees with reverse instruction scan");
    live.blocks ()[b].in.for_each ([&] (auto s) {
      auto r = full[s];
      dead (r, s, false);
      if (!compact && !referenced.contains (r)) storage[r].back ().blocks.push_back (block);
    });
    if (!compact) {
      result.spill_[b] = {};
      referenced.for_each ([&] (auto r) {
        if (std::holds_alternative<const MachineVirtualRegister *> (index.values[r]))
          result.spill_[b].kill.insert (r);
      });
    }
    if (!live.blocks ()[b].in.empty () && !advance ()) return invalid (f, "live point overflow");
  }
  for (auto &ranges : storage) std::reverse (ranges.begin (), ranges.end ());
  result.raw_ = storage;
  result.raw_points_ = point;
  SparseBitSet births, deaths;
  for (auto &ranges : storage)
    for (auto &r : ranges) {
      if (r.start < 0 || r.finish < r.start) return invalid (f, "unfinished live range");
      births.insert (r.start);
      deaths.insert (r.finish);
    }
  auto events = births;
  events.union_with (deaths);
  result.point_map_.resize (static_cast<std::size_t> (point) + 1);
  int compressed = -1;
  bool previous_dead = true;
  events.for_each ([&] (auto p) {
    if (previous_dead && births.contains (p)) ++compressed;
    result.point_map_[p] = compressed;
    previous_dead = deaths.contains (p);
  });
  if (compressed == INT_MAX) return invalid (f, "compressed live point overflow");
  result.points_ = compressed + 1;
  for (auto &ranges : storage) {
    std::vector<MachineLiveRange> merged;
    for (auto range : ranges) {
      range.start = result.point_map_[range.start];
      range.finish = result.point_map_[range.finish];
      if (merged.empty ()
          || (merged.back ().start != range.finish && merged.back ().start - 1 != range.finish)
          || merged.back ().blocks.empty () != range.blocks.empty ())
        merged.push_back (std::move (range));
      else {
        merged.back ().start = range.start;
        for (auto block : range.blocks)
          merged.back ().blocks.insert (merged.back ().blocks.begin (), block);
      }
    }
    ranges = std::move (merged);
  }
  return result;
}
}  // namespace mirnext::codegen
