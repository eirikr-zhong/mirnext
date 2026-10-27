/* Licensed under the MIT License. Register-preserving raw native boundaries. */
#pragma once
#include "mirnext/CodeGen/InvocationEntry.hpp"
namespace mirnext::codegen {
inline constexpr std::size_t native_boundary_gate_bytes = 32;
struct NativeBoundaryCode {
  std::vector<std::uint8_t> enter, leave;
};
// enter_callback takes (const NativeInvocationState*, descriptor address) and
// returns an owned checkpoint address. leave_callback pops that checkpoint and
// returns its original continuation. Callbacks finish before entering MIR or
// its caller. Only the private native ABI carries addresses; IR stays typed.
Result<NativeBoundaryCode> make_native_boundary_entries (const target::TargetMachine &,
                                                         std::uintptr_t enter_callback,
                                                         std::uintptr_t leave_callback);
Result<std::vector<std::uint8_t>> make_native_boundary_gate (const target::TargetMachine &,
                                                             std::uintptr_t descriptor,
                                                             std::uintptr_t enter);
namespace detail {
Result<NativeBoundaryCode> make_x86_native_boundary_entries (const target::TargetMachine &,
                                                             std::uintptr_t, std::uintptr_t);
Result<std::vector<std::uint8_t>> make_x86_native_boundary_gate (const target::TargetMachine &,
                                                                 std::uintptr_t, std::uintptr_t);
}  // namespace detail
}  // namespace mirnext::codegen
