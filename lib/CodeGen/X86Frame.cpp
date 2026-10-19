/* MIR frame rules, Copyright (C) 2018-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/Detail/X86Frame.hpp"
#include <array>
#include <climits>
namespace mirnext::codegen::detail {
namespace {
using R = target::X86Register; using ir::Opcode; using ir::Type;
Error invalid(std::string why) { return {ErrorCode::InvalidOperand,"x86 frame: " + std::move(why)}; }
std::size_t align16(std::size_t n) { return (n + 15) / 16 * 16; }
}
Result<X86FramePlan> plan_x86_frame(const X86FrameInput &in) {
  const bool win = in.abi == target::ABI::X86Windows;
  if (!win && in.abi != target::ABI::X86SysV) return invalid("unsupported ABI");
  if (in.spill_bytes % 8 || in.spill_bytes > INT32_MAX - 4096u) return invalid("spill area exceeds target offset range");
  if ((in.variadic || in.uses_alloca || in.block_arguments) && !in.frame_pointer) return invalid("variadic, dynamic-stack or block-argument function requires a frame pointer");
  std::array<bool,32> saved{}; std::size_t saved_bytes = 0;
  for (auto r : in.saved_registers) {
    auto n = static_cast<unsigned>(r);
    if (!target::x86_callee_saved_register(in.abi,r) || n >= saved.size() || saved[n]) return invalid("invalid or duplicate saved register");
    saved[n] = true; saved_bytes += n < 16 ? 8 : 16;
  }
  X86FramePlan out;
  if (in.leaf && !in.uses_alloca && !in.block_arguments && !saved_bytes && !in.variadic && !in.spill_bytes) return out;
  out.saves_frame_pointer = in.frame_pointer;
  out.variadic_save_bytes = in.variadic && !win ? 176 : 0;
  const auto slots = in.frame_pointer ? in.spill_bytes : align16(in.spill_bytes);
  out.block_bytes = align16(slots + saved_bytes);
  // A jump frame has no return-PC slot. With BP it needs another aligned
  // allocation below the lowest FP-relative slot when red-zone use is forbidden.
  const auto service = out.variadic_save_bytes + (in.jump_return ? (in.no_red_zone && in.frame_pointer ? 16 : 0) : 8);
  out.frame_bytes = out.block_bytes + service;
  auto emit = [](auto &sequence,MachineOpcode opcode,std::initializer_list<X86FrameOperand> operands) { sequence.push_back({opcode,operands}); };
  auto mem = [](Type type,R base,std::int64_t disp) { return X86FrameMemory{type,base,disp}; };
  auto integer = [&](R base,std::int64_t disp) { return mem(Type::i64(),base,disp); };
  if (in.frame_pointer) {
    if (in.no_red_zone) emit(out.prologue,Opcode::Add,{R::R10,R::SP,std::int64_t{-8}});
    else {
      emit(out.prologue,Opcode::Mov,{integer(R::SP,-8),R::BP});
      emit(out.prologue,Opcode::Add,{R::BP,R::SP,std::int64_t{-8}});
    }
  }
  if (win && in.variadic) {
    // With no red zone BP is installed only after allocation. R10 already
    // contains the new BP, so homes must use it rather than the caller's BP.
    const auto base = in.no_red_zone ? R::R10 : R::BP;
    for (unsigned n = 0; n < 4; ++n) emit(out.prologue,Opcode::Mov,{integer(base,16 + n * 8),*target::x86_integer_argument_register(in.abi,n)});
  }
  emit(out.prologue,Opcode::Sub,{R::SP,R::SP,static_cast<std::int64_t>(out.frame_bytes)});
  if (in.no_red_zone && in.frame_pointer) {
    emit(out.prologue,Opcode::Mov,{integer(R::SP,static_cast<std::int64_t>(out.frame_bytes) - 8),R::BP});
    emit(out.prologue,Opcode::Mov,{R::BP,R::R10});
  }
  if (in.variadic && !win) {
    for (unsigned n = 0; n < 6; ++n) emit(out.prologue,Opcode::Mov,{integer(R::SP,out.block_bytes + n * 8),*target::x86_integer_argument_register(in.abi,n)});
    for (unsigned n = 0; n < 8; ++n) emit(out.prologue,Opcode::DMov,{mem(Type(Type::Kind::F64),R::SP,out.block_bytes + 48 + n * 16),static_cast<R>(static_cast<unsigned>(R::XMM0) + n)});
  }
  auto offset = in.frame_pointer ? -static_cast<std::int64_t>(out.block_bytes + out.variadic_save_bytes) : static_cast<std::int64_t>(slots);
  const auto base = in.frame_pointer ? R::BP : R::SP;
  auto save = [&](unsigned n) {
    const auto vector = n >= 16; const auto size = vector ? 16u : 8u;
    const auto r = static_cast<R>(n); const auto memory = mem(vector ? Type(Type::Kind::F64) : Type::i64(),base,offset);
    const auto op = vector ? MachineOpcode(in.jump_return && in.frame_pointer ? MachinePseudoOpcode::X86Mov128Unaligned : MachinePseudoOpcode::X86Mov128) : MachineOpcode(Opcode::Mov);
    out.saves.push_back({r,offset,size}); emit(out.prologue,op,{memory,r});
    if (in.has_return) emit(out.epilogue,op,{r,memory}); offset += size;
  };
  // The retained ABI saves vectors before integer registers, independent of
  // the allocation bitmap's order. XMM saves occupy the complete 128 bits.
  for (unsigned n = 16; n < 32; ++n) if (saved[n]) save(n);
  for (unsigned n = 0; n < 16; ++n) if (saved[n]) save(n);
  if (in.has_return) {
    if (!in.frame_pointer) emit(out.epilogue,Opcode::Add,{R::SP,R::SP,static_cast<std::int64_t>(out.frame_bytes)});
    else if (in.no_red_zone) {
      emit(out.epilogue,Opcode::Mov,{R::R10,R::BP});
      emit(out.epilogue,Opcode::Mov,{R::BP,integer(R::R10,0)});
      emit(out.epilogue,Opcode::Add,{R::SP,R::R10,std::int64_t{8}});
    } else {
      emit(out.epilogue,Opcode::Add,{R::SP,R::BP,std::int64_t{8}});
      emit(out.epilogue,Opcode::Mov,{R::BP,integer(R::SP,-8)});
    }
  }
  return out;
}
}
