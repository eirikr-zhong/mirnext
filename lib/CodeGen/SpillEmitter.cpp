/* MIR spill generation, Copyright (C) 2018-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/Detail/SpillEmitter.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include <array>
#include <climits>
namespace mirnext::codegen::detail {
using ir::Type; using ir::Opcode; using K = Type::Kind;
Error MachineSpillEmitter::invalid(std::string why) const {
  return {ErrorCode::InvalidOperand, std::string(f.module_name()) + "/" + std::string(f.name()) + " ["
    + std::string(f.target().name()) + ", " + std::string(stage) + "]: " + std::move(why)};
}
target::PhysicalRegister MachineSpillEmitter::base() const {
  auto n = f.target().architecture() == target::Architecture::X86_64
    ? (f.state().requires_frame_pointer ? 5u : 4u) : 29u;
  return f.target().registers()[n].reg;
}
std::optional<target::PhysicalRegister> MachineSpillEmitter::temporary(Type type, bool first) const {
  if (f.target().architecture() == target::Architecture::X86_64) {
    auto r = target::x86_reload_register(f.target().abi(), type, first);
    return r ? std::optional(f.target().registers()[static_cast<unsigned>(*r)].reg) : std::nullopt;
  }
  return f.target().registers()[type.is_float() ? (first ? 48 : 49) : (first ? 9 : 10)].reg;
}
Result<std::int64_t> MachineSpillEmitter::offset(StackLocation slot, Type type, std::size_t extra) const {
  if (f.target().architecture() == target::Architecture::X86_64) {
    if (extra > 8) return invalid("invalid stack subslot");
    auto disp = target::x86_spill_displacement(f.target().abi(),
      f.state().requires_frame_pointer,
      f.signature().variadic, type, slot.offset);
    if (!disp) return invalid(disp.error().message);
    if (*disp > INT64_MAX - static_cast<std::int64_t>(extra)) return invalid("stack displacement overflow");
    auto value = *disp + static_cast<std::int64_t>(extra);
    if (!valid_offset(f.target().architecture(), type, value)) return invalid("stack displacement exceeds x86 signed 32-bit addressing");
    return value;
  }
  const auto header = f.signature().variadic || f.state().has_block_arguments ? 32u : 16u;
  if (extra > 8 || slot.offset > std::size_t(INT64_MAX) - header - extra) return invalid("stack displacement overflow");
  return static_cast<std::int64_t>(slot.offset + header + extra);
}
bool MachineSpillEmitter::valid_offset(target::Architecture architecture, Type t, std::int64_t value) {
  if (architecture == target::Architecture::X86_64) return value >= INT32_MIN && value <= INT32_MAX;
  auto scale = t.kind() == K::I8 || t.kind() == K::U8 ? 1 : t.kind() == K::I16 || t.kind() == K::U16 ? 2
    : t.kind() == K::I32 || t.kind() == K::U32 || t.kind() == K::F32 ? 4 : t.kind() == K::LongDouble ? 16 : 8;
  return value >= 0 && value % scale == 0 && value / scale < 4096;
}
Result<void> MachineSpillEmitter::access(StackLocation slot, Type type, target::PhysicalRegister data,
    target::PhysicalRegister address, MachineBasicBlock &block, MachineInstruction *anchor, bool after, bool store) {
  auto disp = offset(slot, type); if (!disp) return disp.error();
  const bool direct = valid_offset(f.target().architecture(), type, *disp); auto stack_base = base();
  MachineOperand memory(MachineMemory{type, MachineRegister(direct ? stack_base : address), {}, direct ? *disp : 0, 1, {}, {}});
  auto move = type.kind() == K::F32 ? Opcode::FMov : type.kind() == K::F64 ? Opcode::DMov : type.kind() == K::LongDouble ? Opcode::LDMov : Opcode::Mov;
  used.insert(data.encoding());
  struct Emission { Opcode code; std::vector<MachineOperand> operands; };
  std::array<Emission, 3> code; std::size_t count = 0;
  if (!direct) {
    code[count++] = {Opcode::Mov, {MachineOperand(address), MachineOperand(*disp)}};
    code[count++] = {Opcode::Add, {MachineOperand(address), MachineOperand(address), MachineOperand(stack_base)}};
  }
  code[count++] = {move, store ? std::vector{memory, MachineOperand(data)} : std::vector{MachineOperand(data), memory}};
  if (anchor && after) std::reverse(code.begin(), code.begin() + count);
  for (auto &i : std::span(code.data(), count)) {
    auto r = !anchor ? rw.append(block, i.code, std::move(i.operands)) : after ? rw.insert_after(*anchor, i.code, std::move(i.operands)) : rw.insert_before(*anchor, i.code, std::move(i.operands));
    if (!r) return invalid(r.error().message);
  }
  return {};
}
Result<void> MachineSpillEmitter::preserve(const MachineVirtualRegister *r, MachineBasicBlock &block,
    MachineInstruction *anchor, bool after, bool restore) {
  auto &a = result.allocation_; auto index = a.virtuals_.find(r);
  if (index == a.virtuals_.end()) return invalid("foreign preserved register");
  auto p = std::get_if<target::PhysicalRegister>(&a.locations_[index->second]);
  if (!p) return invalid("preserved register is not assigned to hardware");
  auto found = result.spill_slots_.find(r);
  if (found == result.spill_slots_.end()) {
    SparseBitSet conflicts; const auto &ranges = (*a.live_ranges_)[index->second];
    for (auto &lr : ranges) for (int n = lr.start; n <= lr.finish; ++n) conflicts.union_with(a.used_[n].stack);
    auto size = r->type().kind() == K::LongDouble ? 16u : 8u; std::optional<StackLocation> slot;
    for (auto offset = a.reusable_stack_; offset + size <= a.stack_bytes_; offset += 8) {
      if (offset % size) continue; bool available = true;
      for (auto k = offset / 8; k < (offset + size) / 8; ++k) available &= !conflicts.contains(k);
      if (available) { slot = StackLocation{offset, size, size}; break; }
    }
    if (!slot) {
      if (a.stack_bytes_ > std::size_t(INT64_MAX) - 64) return invalid("spill area overflow");
      auto aligned = (a.stack_bytes_ + size - 1) / size * size; slot = StackLocation{aligned, size, size}; a.stack_bytes_ = aligned + size;
    }
    for (auto &lr : ranges) for (int n = lr.start; n <= lr.finish; ++n) for (auto k = slot->offset / 8; k < (slot->offset + slot->size) / 8; ++k) a.used_[n].stack.insert(k);
    found = result.spill_slots_.emplace(r, *slot).first;
  }
  return access(found->second, r->type(), *p, *temporary(Type::i64(), true), block, anchor, after, !restore);
}
}
