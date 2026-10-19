/* Priority assignment derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. Consumes owning machine nodes and analyses. */
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include <algorithm>
#include <bit>
#include <climits>
#include <map>
#include <numeric>

namespace mirnext::codegen {
namespace {
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", machine-assignment]: " + std::move (why)};
}
unsigned stack_units (ir::Type type) { return type.kind () == ir::Type::Kind::LongDouble ? 2 : 1; }
struct Gap {
  target::PhysicalRegister physical;
  std::size_t reg, range;
};
}  // namespace
Result<MachineLocation> MachineAllocation::location (const MachineVirtualRegister &reg) const {
  if (!current (*owner_)) return invalid (*owner_, "stale allocation");
  auto at = virtuals_.find (&reg);
  if (at == virtuals_.end ()) return invalid (*owner_, "foreign register");
  return locations_[at->second];
}
Result<std::size_t> MachineAllocation::live_length (const MachineVirtualRegister &reg) const {
  if (!current (*owner_)) return invalid (*owner_, "stale allocation");
  auto at = virtuals_.find (&reg);
  if (at == virtuals_.end ()) return invalid (*owner_, "foreign register");
  return lengths_[at->second];
}
Result<MachineAllocation> MachineAllocation::assign (const MachineFunction &f,
                                                     const MachineGraphAnalysis &graph,
                                                     const MachineLiveness &live,
                                                     const MachineLiveRanges &ranges) {
  if (!graph.current (f) || !live.current (f) || !ranges.current (f))
    return invalid (f, "stale or foreign allocation analyses");
  if (!live.has_frequencies () || live.is_compact () || ranges.is_compact ()
      || live.optimization_level () != ranges.optimization_level ()
      || !std::ranges::equal (live.global_registers (), ranges.global_registers ())
      || !std::ranges::equal (live.registers (), ranges.registers ()))
    return invalid (f, "inconsistent or compact allocation analyses");
  auto valid = f.verify ();
  if (!valid) return valid.error ();
  auto count = live.registers ().size (), physical = f.target ().registers ().size ();
  if (ranges.point_count () < 0 || ranges.point_count () == INT_MAX || count > SIZE_MAX / 16)
    return invalid (f, "allocation dimensions overflow");
  MachineAllocation result;
  result.owner_ = &f;
  result.revision_ = f.revision ();
  result.live_ranges_ = ranges.ranges_;
  result.level_ = live.optimization_level ();
  result.globals_.assign (live.global_registers ().begin (), live.global_registers ().end ());
  result.registers_.assign (live.registers ().begin (), live.registers ().end ());
  result.locations_.resize (count);
  result.lengths_.resize (count);
  result.spill_.assign (ranges.spill_blocks ().begin (), ranges.spill_blocks ().end ());
  const bool advanced = live.optimization_level () >= 2;
  if (advanced)
    for (auto &node : graph.loops ())
      if (node.block != no_machine_node && node.parent != no_machine_node) {
        auto &parent = graph.loops ()[node.parent];
        if (parent.entry != node.block) continue;
        bool inner = true;
        for (auto child : parent.children)
          if (graph.loops ()[child].block == no_machine_node) inner = false;
        if (inner && graph.cfg ().blocks[node.block])
          result.aligned_loops_.push_back (graph.cfg ().blocks[node.block]);
      }
  MachinePointLocations globals;
  for (auto p : live.global_registers ()) globals.physical.insert (p.encoding ());
  result.used_.assign (static_cast<std::size_t> (ranges.point_count ()) + 1, globals);
  if (advanced) result.busy_ = result.used_;
  std::vector<long> frequencies (count);
  SparseBitSet addressable;
  for (std::size_t r = physical; r < count; ++r) {
    auto reg = std::get<const MachineVirtualRegister *> (result.registers_[r]);
    result.virtuals_.emplace (reg, r);
    auto frequency = live.frequency (reg);
    if (!frequency) return frequency.error ();
    frequencies[r] = *frequency;
    if (reg->is_memory_backed ()) addressable.insert (r);
  }
  // ADDR makes storage observable even at O0/O1, before address conversion.
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      auto code = i.opcode ();
      auto op = std::get_if<ir::Opcode> (&code);
      if (op
          && (*op == ir::Opcode::Addr || *op == ir::Opcode::Addr8 || *op == ir::Opcode::Addr16
              || *op == ir::Opcode::Addr32)) {
        auto local = i.operands ()[1].get<const MachineVirtualRegister *> ();
        if (!local) return invalid (f, "local address requires a virtual register");
        addressable.insert (result.virtuals_.at (*local));
      }
    }
  auto reg_at = [&] (std::size_t r) {
    return std::get<const MachineVirtualRegister *> (result.registers_[r]);
  };
  auto type_at = [&] (std::size_t r) { return reg_at (r)->type (); };
  auto new_stack = [&] (ir::Type type) {
    auto size = std::size_t (stack_units (type)) * 8;
    auto offset = (result.stack_bytes_ + size - 1) / size * size;
    result.stack_bytes_ = offset + size;
    return StackLocation{offset, size, size};
  };
  std::vector<std::size_t> order;
  for (std::size_t r = physical; r < count; ++r) {
    if (addressable.contains (r)) {
      auto slot = new_stack (type_at (r));
      result.locations_[r] = slot;
      result.reusable_stack_ = slot.offset + slot.size;
    } else {
      order.push_back (r);
      for (auto &lr : ranges.all_ranges ()[r])
        result.lengths_[r] += static_cast<std::size_t> (lr.finish - lr.start) + 1;
    }
  }
  std::sort (order.begin (), order.end (), [&] (auto a, auto b) {
    auto tied_a = reg_at (a)->binding ().has_value (), tied_b = reg_at (b)->binding ().has_value ();
    if (tied_a != tied_b) return tied_a;
    // Break ties between hardware bindings by stable declaration order
    // to keep the comparator strictly ordered.
    if (tied_a) return a < b;
    if (frequencies[a] != frequencies[b]) return frequencies[a] > frequencies[b];
    if (result.lengths_[a] != result.lengths_[b]) return result.lengths_[a] > result.lengths_[b];
    return a < b;
  });
  auto reserve = [&] (const MachineLocation &location, const MachineLiveRange &lr,
                      std::vector<MachinePointLocations> &points, bool set = true) {
    for (int p = lr.start; p <= lr.finish; ++p) {
      if (auto physical = std::get_if<target::PhysicalRegister> (&location)) {
        if (set)
          points[p].physical.insert (physical->encoding ());
        else
          points[p].physical.erase (physical->encoding ());
      } else if (auto slot = std::get_if<StackLocation> (&location)) {
        for (auto k = slot->offset / 8; k < (slot->offset + slot->size) / 8; ++k) {
          if (set)
            points[p].stack.insert (k);
          else
            points[p].stack.erase (k);
        }
      }
    }
  };
  for (std::size_t r = 0; r < physical; ++r)
    for (auto &lr : ranges.all_ranges ()[r]) {
      auto location = std::get<target::PhysicalRegister> (result.registers_[r]);
      reserve (location, lr, result.used_);
      if (advanced) reserve (location, lr, result.busy_);
    }
  std::map<std::pair<unsigned, int>, Gap> gaps;
  std::unordered_map<const MachineBasicBlock *, std::size_t> blocks;
  for (std::size_t b = 2; b < graph.cfg ().blocks.size (); ++b)
    blocks.emplace (graph.cfg ().blocks[b], b);
  auto range = [&] (const Gap &g) -> const MachineLiveRange & {
    return ranges.all_ranges ()[g.reg][g.range];
  };
  // A whole-block range's boundary cost is independent of the candidate
  // physical register. Compute it once across all split trials.
  std::unordered_map<const MachineLiveRange *, std::int64_t> spill_costs;
  auto spill_cost = [&] (const MachineLiveRange &lr) {
    if (auto found = spill_costs.find (&lr); found != spill_costs.end ()) return found->second;
    SparseBitSet members;
    for (auto b : lr.blocks) members.insert (blocks.at (b));
    std::int64_t cost = 0;
    for (auto node : lr.blocks) {
      auto b = blocks.at (node);
      auto level = static_cast<std::int64_t> (graph.loop_depth ()[b]) + 1;
      for (bool outgoing : {true, false}) {
        std::int64_t maximum = -1;
        for (auto e : outgoing ? graph.cfg ().outgoing[b] : graph.cfg ().incoming[b]) {
          auto next = outgoing ? graph.cfg ().edges[e].destination : graph.cfg ().edges[e].source;
          if (!members.contains (next))
            maximum = std::max (maximum, static_cast<std::int64_t> (graph.loop_depth ()[next]) + 1);
        }
        if (maximum >= 0) {
          cost += std::min (level, maximum) * 2;
          if (cost > INT_MAX) {
            cost = std::int64_t (INT_MAX) + 1;
            spill_costs.emplace (&lr, cost);
            return cost;
          }
        }
      }
    }
    spill_costs.emplace (&lr, cost);
    return cost;
  };
  auto available
    = [&] (target::PhysicalRegister r, const MachineLiveRange &lr, const auto &points) {
        for (int p = lr.start; p <= lr.finish; ++p)
          if (points[p].physical.contains (r.encoding ())) return false;
        return true;
      };
  auto find_gaps = [&] (target::PhysicalRegister r, const MachineLiveRange &lr,
                        std::vector<Gap> &found) {
    const MachineLiveRange *last = found.empty () ? nullptr : &range (found.back ());
    std::int64_t total = 0;
    auto append = [&] (const Gap &gap) {
      auto &value = range (gap);
      if (last == &value) return;
      found.push_back (gap);
      total = std::min (std::int64_t (INT_MAX) + 1, total + spill_cost (value));
      last = &value;
    };
    auto upper = gaps.upper_bound ({r.encoding (), lr.start});
    int next = lr.start + 1;
    if (upper != gaps.begin ()) {
      auto previous = std::prev (upper);
      if (previous->first.first == r.encoding () && range (previous->second).finish >= lr.start) {
        next = range (previous->second).finish + 1;
        append (previous->second);
      }
    }
    for (auto g = gaps.lower_bound ({r.encoding (), next});
         g != gaps.end () && g->first.first == r.encoding () && g->first.second <= lr.finish;) {
      auto finish = range (g->second).finish;
      append (g->second);
      g = gaps.lower_bound ({r.encoding (), finish + 1});
    }
    return total;
  };
  auto clobbered = [&] (target::PhysicalRegister r) {
    return f.target ().call_clobbers (r, ir::Type (ir::Type::Kind::Undef));
  };
  SparseBitSet used_registers;
  for (auto reg : order) {
    result.order_.push_back (reg_at (reg));
    auto type = type_at (reg);
    if (auto binding = reg_at (reg)->binding ()) {
      result.locations_[reg] = *binding;
      continue;
    }
    if (ranges.all_ranges ()[reg].empty ()) continue;
    MachinePointLocations conflicts;
    for (auto &lr : ranges.all_ranges ()[reg])
      for (int p = lr.start; p <= lr.finish; ++p) {
        conflicts.physical.union_with (result.used_[p].physical);
        conflicts.stack.union_with (result.used_[p].stack);
      }
    std::optional<target::PhysicalRegister> best;
    for (auto &p : f.target ().registers ())
      if (!p.reserved && f.target ().accepts (p.reg, type)
          && !conflicts.physical.contains (p.reg.encoding ())) {
        if (!best || (!clobbered (*best) && clobbered (p.reg))) best = p.reg;
      }
    std::vector<Gap> spilled;
    if (!best && advanced) {
      std::int64_t best_profit = 0;
      std::size_t best_gap = 0;
      for (auto &p : f.target ().registers ()) {
        if (p.reserved || !f.target ().accepts (p.reg, type)) continue;
        std::vector<Gap> candidate;
        bool possible = true;
        std::size_t gap_size = 0;
        // Preserve the original supported-host long-to-int narrowing, while
        // diagnosing subsequent signed arithmetic overflow instead of UB.
        std::int64_t profit
          = std::bit_cast<std::int32_t> (static_cast<std::uint32_t> (frequencies[reg]));
        for (std::size_t n = 0; n < ranges.all_ranges ()[reg].size (); ++n) {
          auto &lr = ranges.all_ranges ()[reg][n];
          if (available (p.reg, lr, result.used_)) continue;
          std::int64_t deduction;
          if (available (p.reg, lr, result.busy_)) {
            deduction = find_gaps (p.reg, lr, candidate);
            gap_size += lr.finish - lr.start + 1;
          } else if (lr.blocks.empty ()) {
            possible = false;
            break;
          } else {
            deduction = spill_cost (lr);
            candidate.push_back ({p.reg, reg, n});
          }
          if (deduction > INT_MAX) return invalid (f, "spill-cost arithmetic overflow");
          profit -= deduction;
          if (profit < INT_MIN || profit > INT_MAX)
            return invalid (f, "spill-profit arithmetic overflow");
        }
        if (!possible || profit < 0) continue;
        if (!best || profit > best_profit
            || (profit == best_profit && !clobbered (*best) && clobbered (p.reg))
            || (profit == best_profit && clobbered (*best) == clobbered (p.reg)
                && gap_size > best_gap)) {
          best = p.reg;
          best_profit = profit;
          best_gap = gap_size;
          spilled = std::move (candidate);
        }
      }
    }
    MachineLocation location;
    if (best) {
      location = *best;
      used_registers.insert (best->encoding ());
    } else {
      auto size = std::size_t (stack_units (type)) * 8;
      std::optional<StackLocation> reuse;
      for (auto offset = result.reusable_stack_; offset + size <= result.stack_bytes_;
           offset += 8) {
        if (offset % size) continue;
        bool free = true;
        for (auto k = offset / 8; k < (offset + size) / 8; ++k)
          free &= !conflicts.stack.contains (k);
        if (free) {
          reuse = StackLocation{offset, size, size};
          break;
        }
      }
      location = reuse ? *reuse : new_stack (type);
    }
    SparseBitSet own_spills;
    for (auto g = spilled.rbegin (); g != spilled.rend (); ++g) {
      auto &lr = range (*g);
      for (auto b : lr.blocks) result.spill_[blocks.at (b)].gen.insert (g->reg);
      if (g->reg == reg) {
        own_spills.insert (lr.start);
        continue;
      }
      gaps.erase ({g->physical.encoding (), lr.start});
      reserve (g->physical, lr, result.used_, false);
    }
    result.locations_[reg] = location;
    const bool simple = !advanced || std::holds_alternative<StackLocation> (location);
    for (std::size_t n = 0; n < ranges.all_ranges ()[reg].size (); ++n) {
      auto &lr = ranges.all_ranges ()[reg][n];
      bool reserved = simple || !own_spills.contains (lr.start);
      if (reserved) reserve (location, lr, result.used_);
      if (simple) continue;
      if (lr.blocks.empty ())
        reserve (location, lr, result.busy_);
      else if (reserved)
        gaps.insert_or_assign ({best->encoding (), lr.start}, Gap{*best, reg, n});
    }
  }
  used_registers.for_each (
    [&] (auto r) { result.used_registers_.push_back (f.target ().registers ()[r].reg); });
  for (auto &[key, gap] : gaps)
    result.gaps_.push_back ({gap.physical, reg_at (gap.reg), range (gap)});
  return result;
}
}  // namespace mirnext::codegen
