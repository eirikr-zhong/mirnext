/* Aggressive coalescing derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. Rewrites the sole block-owned instruction body. */
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/Detail/RegisterIndex.hpp"
#include "mirnext/CodeGen/Detail/RegisterEffects.hpp"
#include <algorithm>
#include <numeric>

namespace mirnext::codegen {
namespace {
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", machine-coalescing]: " + std::move (why)};
}
std::optional<std::array<const MachineVirtualRegister *, 2>> move_pair (
  const MachineInstruction &i) {
  auto code = i.opcode ();
  auto op = std::get_if<ir::Opcode> (&code);
  if (!op
      || (*op != ir::Opcode::Mov && *op != ir::Opcode::FMov && *op != ir::Opcode::DMov
          && *op != ir::Opcode::LDMov))
    return {};
  auto a = i.operands ()[0].get<const MachineVirtualRegister *> (),
       b = i.operands ()[1].get<const MachineVirtualRegister *> ();
  if (!a || !b) return {};
  return std::array{*a, *b};
}
std::size_t conflict_bit (std::size_t a, std::size_t b, std::size_t dimension) {
  if (a > b) std::swap (a, b);
  // Only the symmetric upper triangle is queried. Keep diagonal entries so
  // singleton/self-copy groups use exactly the same conflict predicate.
  return a * dimension - a * (a + 1) / 2 + b;
}
}  // namespace
Result<MachineRegister> MachineCoalescing::representative (MachineRegister r) const {
  if (!current (*owner_)) return invalid (*owner_, "stale coalescing result");
  if (auto v = std::get_if<const MachineVirtualRegister *> (&r)) {
    auto at = virtuals_.find (*v);
    if (at != virtuals_.end ()) return representatives_[at->second];
  } else {
    auto p = std::get<target::PhysicalRegister> (r);
    auto at = physicals_.find (p.encoding ());
    if (at != physicals_.end () && registers_[at->second] == r) return representatives_[at->second];
  }
  return invalid (*owner_, "foreign register");
}
Result<bool> MachineCoalescing::conflicts (MachineRegister a, MachineRegister b) const {
  if (!current (*owner_)) return invalid (*owner_, "stale coalescing result");
  if (!enabled_) return invalid (*owner_, "conflict scan was not enabled");
  auto x = std::get_if<const MachineVirtualRegister *> (&a),
       y = std::get_if<const MachineVirtualRegister *> (&b);
  if (!x || !y || !compact_.contains (*x) || !compact_.contains (*y))
    return invalid (*owner_, "register outside compact conflict scan");
  auto bit = conflict_bit (compact_.at (*x), compact_.at (*y), scan_.size ());
  return (conflicts_[bit / 64] & (std::uint64_t (1) << (bit % 64))) != 0;
}
Result<MachineCoalescing> coalesce_registers (MachineFunction &f, const MachineGraphAnalysis &graph,
                                              unsigned level, std::size_t limit) {
  if (level > 3 || !limit || !graph.current (f))
    return invalid (f, "invalid level, register limit or stale/foreign CFG");
  auto valid = f.verify ();
  if (!valid) return valid.error ();
  detail::RegisterIndex index (f);
  MachineCoalescing result;
  result.owner_ = &f;
  result.registers_ = index.values;
  result.representatives_ = index.values;
  result.virtuals_ = index.virtuals;
  result.physicals_ = index.physicals;
  struct Move {
    MachineInstruction *instruction;
    std::size_t frequency, order;
  };
  std::vector<Move> moves;
  if (level >= 2)
    for (std::size_t b = 2; b < graph.cfg ().blocks.size (); ++b) {
      std::size_t frequency = 1;
      for (auto depth = graph.loop_depth ()[b]; depth && frequency < SIZE_MAX / 8; --depth)
        frequency *= 5;
      for (auto &i : const_cast<MachineBasicBlock *> (graph.cfg ().blocks[b])->instructions ()) {
        if (move_pair (i)) moves.push_back ({&i, frequency, i.creation_order ()});
      }
    }
  std::sort (moves.begin (), moves.end (), [] (auto &a, auto &b) {
    return a.frequency != b.frequency ? a.frequency > b.frequency : a.order < b.order;
  });
  result.selected_ = moves.size ();
  for (auto &m : moves) {
    auto pair = *move_pair (*m.instruction);
    for (auto r : pair) {
      if (!result.compact_.contains (r) && result.scan_.size () < limit) {
        result.compact_.emplace (r, result.scan_.size ());
        result.scan_.emplace_back (r);
      }
    }
  }
  result.enabled_ = !result.scan_.empty () && result.scan_.size () < limit;
  if (!result.enabled_) {
    result.revision_ = f.revision ();
    return result;
  }
  auto count = result.scan_.size ();
  if (count > (SIZE_MAX - 63) / count) return invalid (f, "conflict matrix size overflow");
  const auto bits = count % 2 ? count * ((count + 1) / 2) : (count / 2) * (count + 1);
  result.conflicts_.resize ((bits + 63) / 64);
  std::vector<std::size_t> full_scan (index.values.size (), SIZE_MAX);
  for (std::size_t s = 0; s < count; ++s) full_scan[*index.find (result.scan_[s])] = s;
  auto live
    = MachineLiveness::analyze (f, graph,
                                {.optimization_level = level, .scan_registers = result.scan_});
  if (!live) return live.error ();
  auto compact = [&] (MachineRegister r) -> std::optional<std::size_t> {
    if (auto v = std::get_if<const MachineVirtualRegister *> (&r)) {
      auto at = result.compact_.find (*v);
      if (at != result.compact_.end ()) return at->second;
    }
    return {};
  };
  for (std::size_t b = 2; b < graph.cfg ().blocks.size (); ++b) {
    auto alive = live->blocks ()[b].out;
    auto body = graph.cfg ().blocks[b]->instructions ();
    for (auto at = body.end (); at != body.begin ();) {
      auto &i = *--at;
      auto pair = move_pair (i);
      auto ignore = pair ? compact ((*pair)[1]) : std::nullopt;
      detail::visit_registers (i, true, [&] (auto r) {
        if (auto s = compact (r)) {
          alive.for_each ([&] (auto other) {
            if (other != *s && other != ignore.value_or (SIZE_MAX)) {
              auto bit = conflict_bit (*s, other, count);
              result.conflicts_[bit / 64] |= std::uint64_t (1) << (bit % 64);
            }
          });
          alive.erase (*s);
        }
      });
      detail::visit_registers (i, false, [&] (auto r) {
        if (auto s = compact (r)) alive.insert (*s);
      });
    }
    if (body.begin () != body.end () && alive != live->blocks ()[b].in)
      return invalid (f, "compact live-in disagrees with reverse instruction scan");
  }
  std::vector<std::size_t> first (index.values.size ()), next;
  std::iota (first.begin (), first.end (), 0);
  next = first;
  auto bound = [&] (std::size_t r) {
    return std::get<const MachineVirtualRegister *> (index.values[r])->binding ().has_value ();
  };
  auto conflict = [&] (std::size_t a, std::size_t b) {
    auto r1 = a;
    do {
      auto r2 = b;
      do {
        auto x = full_scan[r1], y = full_scan[r2];
        auto bit = conflict_bit (x, y, count);
        if (result.conflicts_[bit / 64] & (std::uint64_t (1) << (bit % 64))) return true;
        r2 = next[r2];
      } while (r2 != b);
      r1 = next[r1];
    } while (r1 != a);
    return false;
  };
  for (auto &m : moves) {
    auto pair = *move_pair (*m.instruction);
    auto source = *index.find (pair[1]), destination = *index.find (pair[0]);
    auto a = first[source], b = first[destination];
    if (a == b) {
      ++result.coalesced_;
      continue;
    }
    if (conflict (a, b) || (bound (a) && bound (b))) continue;
    ++result.coalesced_;
    if (bound (b) || (!bound (a) && b < a)) {
      std::swap (a, b);
      std::swap (source, destination);
    }
    auto last = destination;
    for (auto r = next[destination];; r = next[r]) {
      first[r] = a;
      if (r == destination) break;
      last = r;
    }
    auto saved = next[a];
    next[a] = destination;
    next[last] = saved;
  }
  MachineRewriter rw (f);
  for (auto &m : moves) {
    auto pair = *move_pair (*m.instruction);
    if (first[*index.find (pair[0])] != first[*index.find (pair[1])]) continue;
    auto &b = const_cast<MachineBasicBlock &> (m.instruction->block ());
    auto erased = rw.erase (*m.instruction);
    if (!erased) return erased.error ();
    ++result.removed_;
    if (b.instructions ().begin () == b.instructions ().end () && b.labels ().empty ()) {
      auto l = f.create_label (b);
      if (!l) return l.error ();
    }
  }
  auto substitute = [&] (MachineRegister r) { return index.values[first[*index.find (r)]]; };
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      bool changed = false;
      auto differs = [&] (auto r) {
        auto p = *index.find (r);
        changed |= first[p] != p;
      };
      detail::visit_registers (i, true, differs);
      detail::visit_registers (i, false, differs);
      if (!changed) continue;
      std::vector<MachineOperand> operands;
      operands.reserve (i.operands ().size ());
      for (auto &o : i.operands ()) {
        auto replacement = o;
        if (auto r = o.get<const MachineVirtualRegister *> ())
          replacement = MachineOperand (std::get<const MachineVirtualRegister *> (substitute (*r)));
        else if (auto memory = o.get<MachineMemory> ()) {
          auto m = *memory;
          if (m.base) m.base = substitute (*m.base);
          if (m.index) m.index = substitute (*m.index);
          replacement = MachineOperand (std::move (m));
        }
        operands.push_back (std::move (replacement));
      }
      if (changed) {
        auto replaced = rw.replace (i, i.opcode (), std::move (operands));
        if (!replaced) return replaced.error ();
      }
    }
  for (std::size_t r = 0; r < first.size (); ++r)
    result.representatives_[r] = index.values[first[r]];
  valid = f.verify ();
  if (!valid) return valid.error ();
  result.revision_ = f.revision ();
  return result;
}
}  // namespace mirnext::codegen
