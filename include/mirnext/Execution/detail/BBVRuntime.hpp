/* Licensed under the MIT License. Owning block compiler for session integration. */
#pragma once
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/CodeGen/CompiledBlock.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Execution/BlockVersioning.hpp"
namespace mirnext::execution::detail {
struct BBVRuntimeOptions {
  // Explicit resource budget, page rounded. No independent mapping fallback.
  std::size_t code_capacity = 0;
  // A real recovery checkpoint outside this function, valid both before the
  // prologue and at arbitrary internal blocks. Ordinary RET is not sufficient.
  // The owner of this continuation must outlive this runtime and all entries.
  std::uintptr_t failure_continuation = 0;
  std::shared_ptr<CodeMemoryProvider> memory_provider = native_code_memory_provider ();
  codegen::CodeSymbolResolver resolve;
  // Called for each compilation attempt before relocation/publication. Views
  // live only for the callback. Failure leaves the entry cold and retryable.
  std::function<Result<void> (const codegen::BBVVersion &, const codegen::CompiledBlock &)> observe;
};
using BBVRuntimeStatistics = BlockCompilationStatistics;
// Internal execution service. Owns preparation, first-version policy, stable
// descriptors, gates, publication reservations, code and immutable artifacts.
// Addresses use the MIR ABI and remain valid only while this owner lives.
// All operations and execution require external synchronization.
// The execution session supplies native entry boundaries and their recovery
// continuation, together with its shared typed symbol resolver.
class BBVRuntime {
 public:
  static Result<std::unique_ptr<BBVRuntime>> create (codegen::BBVProgram, BBVRuntimeOptions);
  ~BBVRuntime ();
  BBVRuntime (const BBVRuntime &) = delete;
  BBVRuntime &operator= (const BBVRuntime &) = delete;
  const codegen::BBVProgram &program () const;
  const codegen::BBVVersion &entry_version () const;
  std::uintptr_t entry_address () const;
  Result<std::uintptr_t> address (const codegen::BlockVersionId &) const;
  // Data-address roots select empty initial properties before execution.
  // LADDR edges instead retain their instruction's property snapshot.
  Result<std::uintptr_t> label_address (const codegen::MachineLabel &);
  Result<void> compile (const codegen::BlockVersionId &);
  const codegen::CompiledBlock *artifact (const codegen::BlockVersionId &) const;
  const BBVRuntimeStatistics &statistics () const;
  bool compiling () const;
  std::size_t remaining_code_bytes () const;

 private:
  struct State;
  explicit BBVRuntime (std::unique_ptr<State>);
  std::unique_ptr<State> state_;
};
}  // namespace mirnext::execution::detail
