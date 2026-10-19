/* Licensed under the MIT License. Checked host-to-generated-code checkpoints. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
#include <array>
#include <cstddef>
namespace mirnext::codegen {
// Private native ABI shared by the entry emitter and invocation service. This
// is CPU state, never an IR object or a serialized representation. The x86
// extended area holds FXSAVE64; AArch64 uses the general/vector/FP fields.
struct alignas(16) NativeInvocationState {
  std::array<std::uint64_t,32> general;
  std::array<std::array<std::uint64_t,2>,32> vectors;
  std::uint64_t original_sp,return_address,fpcr,fpsr;
  alignas(16) std::array<std::uint8_t,512> extended;
  std::uint64_t x87_results;
};
static_assert(offsetof(NativeInvocationState,vectors) == 256);
static_assert(offsetof(NativeInvocationState,original_sp) == 768);
static_assert(offsetof(NativeInvocationState,return_address) == 776);
static_assert(offsetof(NativeInvocationState,fpcr) == 784);
static_assert(offsetof(NativeInvocationState,fpsr) == 792);
static_assert(offsetof(NativeInvocationState,extended) == 800);
static_assert(offsetof(NativeInvocationState,x87_results) == 1312);
struct InvocationEntryCode { std::vector<std::uint8_t> invoke,recover; };
// invoke has the native ABI (target, arg0, arg1, state*) -> void/i64/u64/double.
// target is a host-preserving ABI adapter with up to two pointer arguments.
// recover is entered only AFTER a failed cold resolver has returned through
// all C++ frames. The no-argument callback supplies the innermost active state.
// Recovering abandons generated frames only; every host reentry must establish
// its own checkpoint. It is not a C++ exception or general stack unwinder.
Result<InvocationEntryCode> make_invocation_entries(const target::TargetMachine &,std::uintptr_t current_state);
}
