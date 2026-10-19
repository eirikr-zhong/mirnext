/* ABI rules derived from MIR. Licensed under the MIT License. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
#include <array>

namespace mirnext::target {
enum class X86Register : unsigned {
  AX, CX, DX, BX, SP, BP, SI, DI, R8, R9, R10, R11, R12, R13, R14, R15,
  XMM0, XMM1, XMM2, XMM3, XMM4, XMM5, XMM6, XMM7,
  XMM8, XMM9, XMM10, XMM11, XMM12, XMM13, XMM14, XMM15, ST0, ST1
};
struct X86ArgumentLocation {
  std::array<X86Register, 2> registers{};
  unsigned register_count = 0;
  std::size_t stack_offset = 0, size = 0;
  bool aggregate = false, by_value = false;
};
struct X86ArgumentLayout {
  std::vector<X86ArgumentLocation> arguments;
  std::size_t stack_bytes = 0, integer_count = 0, floating_count = 0;
  unsigned variadic_xmm_count = 0;
};
// These pure ABI facts can be checked on any host. They neither construct a
// foreign TargetMachine nor enable cross-target compilation or execution.
Result<X86ArgumentLayout> layout_x86_arguments(ABI, std::span<const ir::Type>);
Result<std::vector<X86Register>> layout_x86_results(ABI, std::span<const ir::Type>);
std::optional<X86Register> x86_integer_argument_register(ABI, std::size_t);
std::optional<X86Register> x86_floating_argument_register(ABI, std::size_t);
bool x86_callee_saved_register(ABI, X86Register);
// Allocation uses 8-byte slots; x87 values occupy two slots and stay in memory.
Result<std::int64_t> x86_spill_displacement(ABI, bool frame_pointer, bool variadic,
    ir::Type, std::size_t byte_offset);
std::optional<X86Register> x86_reload_register(ABI, ir::Type, bool first);
}
