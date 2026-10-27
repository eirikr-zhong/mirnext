/* Licensed under the MIT License. Ephemeral x86 frame emission plan. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/Target/X86ABI.hpp"
namespace mirnext::codegen::detail {
// Pure target facts permit testing both ABIs without a foreign TargetMachine.
// The sequence is consumed once by frame formation, never a second function body.
struct X86FrameInput {
  target::ABI abi;
  std::size_t spill_bytes = 0;
  std::span<const target::X86Register> saved_registers;
  bool leaf = true, uses_alloca = false, block_arguments = false, variadic = false;
  bool frame_pointer = false, jump_return = false, has_return = true, no_red_zone = false;
};
struct X86FrameMemory {
  ir::Type type;
  target::X86Register base;
  std::int64_t displacement;
};
using X86FrameOperand = std::variant<target::X86Register, std::int64_t, X86FrameMemory>;
struct X86FrameEmission {
  MachineOpcode opcode;
  std::vector<X86FrameOperand> operands;
};
struct X86FrameSave {
  target::X86Register reg;
  std::int64_t offset;
  std::size_t size;
};
struct X86FramePlan {
  std::size_t frame_bytes = 0, block_bytes = 0, variadic_save_bytes = 0;
  bool saves_frame_pointer = false;
  std::vector<X86FrameSave> saves;
  std::vector<X86FrameEmission> prologue, epilogue;
};
Result<X86FramePlan> plan_x86_frame (const X86FrameInput &);
}  // namespace mirnext::codegen::detail
