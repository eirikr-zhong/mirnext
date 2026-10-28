#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
/* Allocation rewriting derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. No instruction/CFG transport projection. */
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include "mirnext/CodeGen/Detail/RegisterEffects.hpp"
#include "mirnext/CodeGen/Detail/InstructionPatterns.hpp"
#include "mirnext/CodeGen/SpillEmitter.hpp"
#include <algorithm>
#include <bit>
#include <climits>

namespace mirnext::codegen {
namespace {
using ir::Opcode;
using ir::Type;
using K = Type::Kind;
bool address (MachineOpcode c) {
  auto p = std::get_if<Opcode> (&c);
  return p
         && (*p == Opcode::Addr || *p == Opcode::Addr8 || *p == Opcode::Addr16
             || *p == Opcode::Addr32);
}
bool move (MachineOpcode c) {
  auto p = std::get_if<Opcode> (&c);
  return p
         && (*p == Opcode::Mov || *p == Opcode::FMov || *p == Opcode::DMov || *p == Opcode::LDMov);
}
Opcode move_type (Type t) {
  return t.kind () == K::F32          ? Opcode::FMov
         : t.kind () == K::F64        ? Opcode::DMov
         : t.kind () == K::LongDouble ? Opcode::LDMov
                                      : Opcode::Mov;
}
bool equal (const MachineOperand &a, const MachineOperand &b) {
  if (auto x = detail::operand_access (a).get<MachineMemory> ()) {
    auto y = detail::operand_access (b).get<MachineMemory> ();
    return y && x->type == y->type && x->displacement == y->displacement && x->base == y->base
           && x->index == y->index && (!x->index || x->scale == y->scale);
  }
  return a == b;
}
Error invalid (const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string (f.module_name ()) + "/" + std::string (f.name ())
                                       + " [" + std::string (f.target ().name ())
                                       + ", allocation-rewrite]: " + std::move (why)};
}
struct DataMode {
  Type type = Type::i64 ();
  bool output = false, declared = false;
};
DataMode data_mode (MachineOpcode code, std::span<const MachineOperand> ops, std::size_t p) {
  if (code == MachineOpcode (MachinePseudoOpcode::Use)) return {Type::i64 (), false, true};
  if (code == MachineOpcode (MachinePseudoOpcode::X86Mov128)
      || code == MachineOpcode (MachinePseudoOpcode::X86Mov128Unaligned))
    return {Type (K::F64), p == 0};
  if (address (code)) return {Type::i64 (), p == 0, p != 0};
  auto op = std::get<Opcode> (code);
  if (op == Opcode::Ret) return {Type::i64 (), false, true};
  if (op == Opcode::LAddr) return {};
  if (detail::is_call (code)) {
    if (p < 2) return {};
    auto &s = *detail::NodeAccess::symbol (
                 *(detail::operand_access (ops[0]).get<MachineSymbolReference> ()))
                 ->signature;
    auto n = p - 2;
    if (n < s.results.size ()) return {s.results[n], true};
    n -= s.results.size ();
    return {n < s.parameters.size () ? s.parameters[n] : Type::i64 (), false};
  }
  auto d = ir::instruction_descriptor (op).get ();
  if (d->count != ir::OperandCount::Fixed) return {};
  auto r = d->roles[p];
  return {Type (r == 'F' || r == 'f'   ? K::F32
                : r == 'D' || r == 'd' ? K::F64
                : r == 'E' || r == 'e' ? K::LongDouble
                                       : K::I64),
          r >= 'A' && r <= 'Z'};
}
}  // namespace
namespace detail {
struct AllocationRewriteEngine {
  MachineFunction &f;
  MachineRewriter rw;
  const MachineGraphAnalysis &graph;
  const MachineLiveness &live;
  const MachineLiveRanges &ranges;
  MachineRewriteResult result;
  SparseBitSet used;
  MachineSpillEmitter spill;
  struct Reload {
    const MachineVirtualRegister *reg;
    std::optional<target::PhysicalRegister> physical;
  };
  std::vector<Reload> in, out;
  std::vector<std::pair<MachineInstruction *, std::vector<CallArgumentDependency>>>
    pending_dependencies;
  std::optional<Error> error;
  bool advanced;
  AllocationRewriteEngine (MachineFunction &function, const MachineGraphAnalysis &g,
                           const MachineLiveness &l, const MachineLiveRanges &r,
                           MachineAllocation a)
    : f (function),
      rw (f),
      graph (g),
      live (l),
      ranges (r),
      result (std::move (a)),
      spill (f, result, used, "allocation-rewrite"),
      advanced (l.optimization_level () >= 2) {
    for (auto p : result.allocation_.used_registers_) used.insert (p.encoding ());
    for (std::size_t b = 0; b < detail::NodeAccess::blocks (g.cfg ()).size (); ++b)
      result.blocks_.push_back (
        {::mirnext::detail::borrow_optional (detail::NodeAccess::blocks (g.cfg ())[b]),
         l.blocks ()[b].in, result.allocation_.spill_[b].gen, result.allocation_.spill_[b].kill});
  }
  void fail (std::string why) {
    if (!error) error = invalid (f, std::move (why));
  }
  template <class T>
  void check (const Result<T> &r) {
    if (!r.has_value () && !error) error = invalid (f, r.error ().message);
  }
  target::PhysicalRegister physical (unsigned n) const { return f.target ().registers ()[n].reg; }
  std::optional<target::PhysicalRegister> temporary (Type t, bool first) const {
    return spill.temporary (t, first);
  }
  std::size_t index (const MachineVirtualRegister *r) const {
    return result.allocation_.virtuals_.at (r);
  }
  std::size_t index (MachineRegister r) const {
    if (auto v = std::get_if<const MachineVirtualRegister *> (detail::NodeAccess::storage (&r)))
      return index (*v);
    return std::get<target::PhysicalRegister> (detail::NodeAccess::storage (r)).encoding ();
  }
  MachineLocation location (const MachineVirtualRegister *r) const {
    return result.allocation_.locations_[index (r)];
  }
  bool spilled (const MachineVirtualRegister *r) const {
    return std::holds_alternative<StackLocation> (location (r));
  }
  std::int64_t offset (StackLocation slot, Type type, std::size_t extra = 0) {
    auto r = spill.offset (slot, type, extra);
    check (r);
    return (r).has_value () ? (r).value () : 0;
  }
  bool valid_offset (Type t, std::int64_t value) const {
    return MachineSpillEmitter::valid_offset (f.target ().architecture (), t, value);
  }
  void insert (MachineInstruction &i, MachineOpcode code, std::vector<MachineOperand> operands,
               bool after) {
    if (error) return;
    auto r = after ? rw.insert_after (i, code, std::move (operands))
                   : rw.insert_before (i, code, std::move (operands));
    check (r);
  }
  std::optional<target::PhysicalRegister> reload (const MachineVirtualRegister *r, Type t,
                                                  bool output) {
    auto &reloads = output ? out : in;
    unsigned occupied = 0;
    for (auto &v : reloads) {
      if (r && v.reg == r) return v.physical;
      if (!occupied && v.physical == temporary (t, true))
        occupied = 1;
      else if (v.physical == temporary (t, false))
        occupied = 2;
    }
    if (occupied > 1 || reloads.size () >= 8) {
      fail ("temporary reload registers exhausted");
      return temporary (t, true);
    }
    auto p = temporary (t, !occupied);
    reloads.push_back ({r, p});
    return p;
  }
  MachineOperand memory (Type t, std::int64_t displacement, target::PhysicalRegister base) const {
    return detail::NodeAccess::make_MachineOperand (
      detail::NodeAccess::make_MachineMemory (t, detail::NodeAccess::make_MachineRegister (base),
                                              {}, displacement, 1, {}, {}));
  }
  void load_store (StackLocation slot, Type t, target::PhysicalRegister p,
                   target::PhysicalRegister address, MachineInstruction &i, bool after,
                   bool store) {
    check (spill.access (slot, t, p, address, const_cast<MachineBasicBlock &> (i.block ()), &i,
                         after, store));
  }
  MachineOperand change (const MachineVirtualRegister *r, Type data, MachineInstruction &i,
                         bool output) {
    auto loc = location (r);
    if (auto p = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&loc)))
      return detail::NodeAccess::make_MachineOperand (*p);
    auto type = r->type ();
    auto p = reload (r, type, output);
    auto address = output || type != Type::i64 () ? reload (nullptr, Type::i64 (), output) : p;
    if (!p) {
      // x87 arithmetic consumes stack memory directly; it has no reload register.
      return memory (data, offset (std::get<StackLocation> (loc), data), spill.base ());
    }
    load_store (std::get<StackLocation> (loc), data, *p, *address, i, output, output);
    return detail::NodeAccess::make_MachineOperand (*p);
  }
  void preserve (const MachineVirtualRegister *r, MachineInstruction &i, bool after, bool restore) {
    check (spill.preserve (r, const_cast<MachineBasicBlock &> (i.block ()), &i, after, restore));
  }
  void reload_address (MachineInstruction &i, std::vector<MachineOperand> &ops, int input,
                       int output) {
    if (input < 0 && output < 0) {
      fail ("address reload lacks a memory operand");
      return;
    }
    auto mem = *detail::operand_access (ops[output >= 0 ? output : input]).get<MachineMemory> ();
    if (input >= 0 && output >= 0 && !equal (ops[input], ops[output])) {
      fail ("incompatible input/output address reloads");
      return;
    }
    auto a = std::get<const MachineVirtualRegister *> (detail::NodeAccess::storage (*mem.base)),
         b = std::get<const MachineVirtualRegister *> (detail::NodeAccess::storage (*mem.index));
    auto first = *temporary (Type::i64 (), true), second = *temporary (Type::i64 (), false);
    load_store (std::get<StackLocation> (location (a)), Type::i64 (), first, first, i, false,
                false);
    load_store (std::get<StackLocation> (location (b)), Type::i64 (), second, second, i, false,
                false);
    if (mem.scale != 1)
      insert (i, Opcode::Lsh,
              {detail::NodeAccess::make_MachineOperand (second),
               detail::NodeAccess::make_MachineOperand (second),
               detail::NodeAccess::make_MachineOperand (
                 std::int64_t (std::countr_zero (mem.scale)))},
              false);
    insert (i, Opcode::Add,
            {detail::NodeAccess::make_MachineOperand (first),
             detail::NodeAccess::make_MachineOperand (first),
             detail::NodeAccess::make_MachineOperand (second)},
            false);
    for (auto p : {input, output})
      if (p >= 0) {
        auto m = *detail::operand_access (ops[p]).get<MachineMemory> ();
        m.base = first;
        m.index.reset ();
        ops[p] = detail::NodeAccess::make_MachineOperand (std::move (m));
      }
  }
  void rewrite (MachineInstruction &i, std::size_t block, SparseBitSet *living,
                SparseBitSet *saves) {
    auto code = i.opcode ();
    std::vector<MachineOperand> ops (i.operands ().begin (), i.operands ().end ());
    std::vector<CallArgumentDependency> dependencies;
    int input_memory = -1, output_memory = -1, inputs = 0;
    auto touch = [&] (MachineRegister r, bool output) {
      if (!living) return;
      auto n = index (r);
      if (output)
        living->erase (n);
      else
        living->insert (n);
      if (auto v = std::get_if<const MachineVirtualRegister *> (detail::NodeAccess::storage (&r));
          v && saves->contains (n)) {
        saves->erase (n);
        preserve (*v, i, true, false);
      }
    };
    for (unsigned iteration = 0; iteration <= 1; ++iteration) {
      for (std::size_t p = 0; p < ops.size (); ++p) {
        auto mode = data_mode (code, ops, p);
        auto m = detail::operand_access (ops[p]).get<MachineMemory> ();
        if ((!iteration && (!mode.output || m)) || (iteration && mode.output && !m)) continue;
        if (auto v = detail::operand_access (ops[p]).get<const MachineVirtualRegister *> ()) {
          if (!mode.output && spilled (*v) && mode.type == Type::i64 ()) ++inputs;
          touch (detail::NodeAccess::make_MachineRegister (*v), mode.output);
        } else if (auto h = detail::operand_access (ops[p]).get<target::PhysicalRegister> ()) {
          used.insert (h->encoding ());
          touch (*h, mode.output);
        } else if (m) {
          for (auto r : {m->base, m->index})
            if (r)
              if (auto h
                  = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&*r)))
                used.insert (h->encoding ());
          auto virtual_base = m->base ? std::get_if<const MachineVirtualRegister *> (
                                          detail::NodeAccess::storage (&*m->base))
                                      : nullptr;
          auto virtual_index = m->index ? std::get_if<const MachineVirtualRegister *> (
                                            detail::NodeAccess::storage (&*m->index))
                                        : nullptr;
          if (virtual_base && virtual_index && spilled (*virtual_base)
              && spilled (*virtual_index)) {
            auto &which = mode.output ? output_memory : input_memory;
            auto other = mode.output ? input_memory : output_memory;
            if (which >= 0) {
              fail ("multiple spilled-address memory operands");
              return;
            }
            which = static_cast<int> (p);
            if (other < 0
                || m->base != detail::operand_access (ops[other]).get<MachineMemory> ()->base
                || m->index != detail::operand_access (ops[other]).get<MachineMemory> ()->index)
              inputs += 2;
          }
          if (m->base) touch (*m->base, false);
          if (m->index) touch (*m->index, false);
        }
      }
      if (living && !iteration) {
        for (auto p : early_clobbers (f.target (), code))
          if (p) living->erase (p->encoding ());
        if (is_call (code)) {
          for (auto &p : f.target ().registers ())
            if (f.target ().call_clobbers (p.reg, Type (K::Undef)))
              living->erase (p.reg.encoding ());
          for (auto p : live.global_registers ()) living->insert (p.encoding ());
          for (auto p : i.call_arguments ()) living->insert (p.encoding ());
          living->for_each ([&] (auto n) {
            auto r = std::get_if<const MachineVirtualRegister *> (
              detail::NodeAccess::storage (&result.allocation_.registers_[n]));
            if (!r || result.blocks_[block].spill_gen.contains (n)) return;
            auto loc = location (*r);
            auto h = std::get_if<target::PhysicalRegister> (detail::NodeAccess::storage (&loc));
            if (h && f.target ().call_clobbers (*h, (*r)->type ()) && !saves->contains (n)) {
              saves->insert (n);
              preserve (*r, i, true, true);
            }
          });
        }
      }
    }
    in.clear ();
    out.clear ();
    if (inputs > 2) {
      reload_address (i, ops, input_memory, output_memory);
      reload (nullptr, Type::i64 (), false);
    }
    if (address (code)) {
      auto r = *detail::operand_access (ops[1]).get<const MachineVirtualRegister *> ();
      auto loc = location (r);
      auto slot = std::get_if<StackLocation> (&loc);
      if (!slot) {
        fail ("local address requires addressable storage");
        return;
      }
      auto p = *reload (nullptr, Type::i64 (), false);
      std::size_t extra = 0;
      if constexpr (std::endian::native == std::endian::big) {
        auto op = std::get<Opcode> (code);
        extra = op == Opcode::Addr8 ? 7 : op == Opcode::Addr16 ? 6 : op == Opcode::Addr32 ? 4 : 0;
      }
      insert (i, Opcode::Mov,
              {detail::NodeAccess::make_MachineOperand (p),
               detail::NodeAccess::make_MachineOperand (offset (*slot, r->type (), extra))},
              false);
      insert (i, Opcode::Add,
              {detail::NodeAccess::make_MachineOperand (p),
               detail::NodeAccess::make_MachineOperand (p),
               detail::NodeAccess::make_MachineOperand (spill.base ())},
              false);
      code = Opcode::Mov;
      ops[1] = detail::NodeAccess::make_MachineOperand (p);
    }
    for (std::size_t p = 0; p < ops.size () && !error; ++p) {
      auto mode = data_mode (code, ops, p);
      if (auto v = detail::operand_access (ops[p]).get<const MachineVirtualRegister *> ()) {
        auto r = *v;
        auto loc = location (r);
        if (!p && std::holds_alternative<StackLocation> (loc)) {
          auto displacement = offset (std::get<StackLocation> (loc), r->type ());
          if (valid_offset (r->type (), displacement)) {
            auto trial = ops;
            auto mem = memory (r->type (), displacement, spill.base ());
            for (auto &o : trial)
              if (auto v = detail::operand_access (o).get<const MachineVirtualRegister *> ();
                  v && *v == r)
                o = mem;
            if (instruction_pattern_matches (f.target (), code, trial)) {
              ops = std::move (trial);
              continue;
            }
          }
        }
        ops[p] = change (r, mode.declared ? r->type () : mode.type, i, mode.output);
      } else if (auto mem = detail::operand_access (ops[p]).get<MachineMemory> ();
                 mem && !(is_call (code) && mem->type.kind () == K::Block)) {
        auto m = *mem;
        if (is_call (code) && m.type.kind () == K::Undef && m.base && !m.index) {
          if (auto v = std::get_if<const MachineVirtualRegister *> (
                detail::NodeAccess::storage (&*m.base))) {
            dependencies.push_back (detail::NodeAccess::make_CallArgumentDependency (p, *v));
            m.base.reset ();
            ops[p] = detail::NodeAccess::make_MachineOperand (std::move (m));
            continue;
          }
        }
        for (auto r : {&m.base, &m.index})
          if (*r)
            if (auto v = std::get_if<const MachineVirtualRegister *> (
                  detail::NodeAccess::storage (&**r))) {
              auto changed = change (*v, Type::i64 (), i, false);
              *r = *detail::operand_access (changed).get<target::PhysicalRegister> ();
            }
        ops[p] = detail::NodeAccess::make_MachineOperand (std::move (m));
      }
    }
    if (error) return;
    if (move (code) && equal (ops[0], ops[1])) {
      auto &b = const_cast<MachineBasicBlock &> (i.block ());
      // Checking just the successor keeps consecutive no-op deletion linear.
      if (live.optimization_level () && ir::detail::ReferenceAccess::pointers (b.labels ()).empty ()
          && std::next (b.instructions ().begin ()) == b.instructions ().end ())
        insert (i, MachinePseudoOpcode::Use, {}, false);
      check (rw.erase (i));
      ++result.deleted_;
    } else {
      check (rw.replace (i, code, std::move (ops)));
      if (!error && !dependencies.empty ())
        pending_dependencies.emplace_back (&i, std::move (dependencies));
    }
  }
  Result<MachineRewriteResult> run () {
    for (std::size_t b = 2; b < detail::NodeAccess::blocks (graph.cfg ()).size () && !error; ++b) {
      auto &block = *const_cast<MachineBasicBlock *> (detail::NodeAccess::blocks (graph.cfg ())[b]);
      auto body = block.instructions ();
      if (!advanced) {
        for (auto it = body.begin (); it != body.end () && !error;) {
          auto &i = *it++;
          rewrite (i, b, nullptr, nullptr);
        }
      } else {
        auto living = live.blocks ()[b].out;
        SparseBitSet saves;
        for (auto it = body.end (); it != body.begin () && !error;) {
          auto current = --it;
          bool first = current == body.begin ();
          auto previous = first ? body.end () : std::prev (current);
          rewrite (*current, b, &living, &saves);
          if (first) break;
          it = std::next (previous);
        }
        if (error) break;
        if (living != live.blocks ()[b].in) {
          fail ("reverse rewrite scan disagrees with live-in");
          break;
        }
        saves.for_each ([&] (auto n) {
          auto first = block.instructions ().begin ();
          if (!result.blocks_[b].spill_kill.contains (n) || first == block.instructions ().end ()) {
            fail ("invalid block-entry preservation");
            return;
          }
          preserve (std::get<const MachineVirtualRegister *> (
                      detail::NodeAccess::storage (result.allocation_.registers_[n])),
                    *first, false, false);
        });
      }
    }
    if (error) return *error;
    for (auto p : live.global_registers ()) used.erase (p.encoding ());
    result.allocation_.used_registers_.clear ();
    used.for_each ([&] (auto n) { result.allocation_.used_registers_.push_back (physical (n)); });
    auto state = f.state ();
    state.operands_rewritten = true;
    auto changed = rw.set_state (state);
    if (!(changed).has_value ()) return changed.error ();
    for (auto &[call, dependencies] : pending_dependencies) {
      auto attached = rw.set_call_dependencies (*call, dependencies);
      if (!(attached).has_value ()) return attached.error ();
    }
    auto valid = f.verify ();
    if (!(valid).has_value ()) return invalid (f, valid.error ().message);
    result.allocation_.revision_ = f.revision ();
    return std::move (result);
  }
};
}  // namespace detail
Result<std::optional<StackLocation>> MachineRewriteResult::spill_slot (
  const MachineVirtualRegister &r) const {
  auto checked = allocation_.location (r);
  if (!(checked).has_value ()) return checked.error ();
  auto found = spill_slots_.find (&r);
  return found == spill_slots_.end () ? std::optional<StackLocation>{}
                                      : std::optional (found->second);
}
Result<MachineRewriteResult> rewrite_allocated (MachineFunction &f,
                                                const MachineGraphAnalysis &graph,
                                                const MachineLiveness &live,
                                                const MachineLiveRanges &ranges,
                                                MachineAllocation allocation) {
  if (f.target ().architecture () != target::Architecture::AArch64
      && f.target ().architecture () != target::Architecture::X86_64)
    return invalid (f, "unsupported target");
  if (!f.state ().operands_legalized || f.state ().operands_rewritten)
    return invalid (f, "invalid rewrite phase");
  if (!graph.current (f) || !live.current (f) || !ranges.current (f) || !allocation.current (f))
    return invalid (f, "stale or foreign allocation input");
  if (live.is_compact () || ranges.is_compact ()
      || live.optimization_level () != ranges.optimization_level ()
      || live.optimization_level () != allocation.optimization_level ()
      || !std::ranges::equal (live.registers (), allocation.registers ())
      || !std::ranges::equal (live.global_registers (), ranges.global_registers ())
      || !std::ranges::equal (live.global_registers (), allocation.global_registers ()))
    return invalid (f, "inconsistent allocation analyses");
  auto checked = f.verify ();
  if (!(checked).has_value ()) return checked.error ();
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      for (auto &o : i.operands ())
        if (detail::operand_access (o).get<StackSlotReference> ())
          return invalid (f, "explicit stack objects require frame lowering");
      std::optional<Error> error;
      auto assigned = [&] (MachineRegister r) {
        if (auto v
            = std::get_if<const MachineVirtualRegister *> (detail::NodeAccess::storage (&r))) {
          auto location = allocation.location (**v);
          if (!(location).has_value ())
            error = location.error ();
          else if (std::holds_alternative<std::monostate> ((location).value ()))
            error = invalid (f, "unassigned register operand");
        }
      };
      detail::visit_registers (i, true, assigned);
      detail::visit_registers (i, false, assigned);
      if (error) return *error;
    }
  detail::AllocationRewriteEngine engine (f, graph, live, ranges, std::move (allocation));
  return engine.run ();
}
}  // namespace mirnext::codegen
