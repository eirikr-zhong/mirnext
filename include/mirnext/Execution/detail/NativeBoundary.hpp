/* Licensed under the MIT License. Owning signature-independent native entries. */
#pragma once
#include "mirnext/CodeGen/NativeBoundary.hpp"
#include "mirnext/Execution/detail/Recovery.hpp"
namespace mirnext::execution::detail {
struct NativeBoundaryTarget { std::uintptr_t address; unsigned x87_results = 0; };
struct NativeBoundaryStateStatistics { std::size_t depth,cached_frames; };
NativeBoundaryStateStatistics native_boundary_state_statistics();
// Gates retain the exact native argument stack, including unknown variadics.
// Every entry installs a nested checkpoint; normal returns retain MIR physical
// register effects, while failure restores host nonvolatiles and zero results.
// Raw calls use the caller's ExecutionInvocation for diagnostics. Checked
// adapters can additionally use recovery() for a typed Result boundary.
// Entries/descriptors live until this owner dies. Publication and calls require
// the owning program's external synchronization. Immutable entries can be used
// by independent threads; checkpoint storage belongs to the calling thread.
class NativeBoundary {
public:
  static Result<NativeBoundary> create(std::shared_ptr<CodeMemoryProvider> = native_code_memory_provider());
  NativeBoundary(NativeBoundary &&) noexcept = default;
  NativeBoundary &operator=(NativeBoundary &&) noexcept = default;
  NativeBoundary(const NativeBoundary &) = delete;
  NativeBoundary &operator=(const NativeBoundary &) = delete;
  Result<std::vector<std::uintptr_t>> add_entries(std::span<const NativeBoundaryTarget>);
  const NativeRecovery &recovery() const { return recovery_; }
  std::size_t code_bytes() const;
  std::size_t code_batches() const { return batches_; }
private:
  struct Descriptor { std::uintptr_t target,continuation; unsigned x87_results; };
  static std::uintptr_t enter(const codegen::NativeInvocationState *,const Descriptor *) noexcept;
  static std::uintptr_t leave() noexcept;
  NativeBoundary(NativeRecovery recovery,ExecutableCode enter,ExecutableCode leave,std::shared_ptr<CodeMemoryProvider> provider)
    : recovery_(std::move(recovery)),enter_(std::move(enter)),leave_(std::move(leave)),memory_(std::move(provider)) {}
  NativeRecovery recovery_;
  ExecutableCode enter_,leave_;
  CodeMemory memory_;
  std::vector<std::unique_ptr<Descriptor>> descriptors_;
  std::vector<ExecutableCode> entries_;
  std::size_t batches_ = 2;
};
}
