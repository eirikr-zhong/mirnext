/* Licensed under the MIT License. Owning checked callable handles. */
#pragma once
#include "mirnext/Execution/Value.hpp"
#include "mirnext/Execution/Entry.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/Execution/BlockVersioning.hpp"
#include "mirnext/Execution/Engine.hpp"
namespace mirnext::execution {
namespace detail {
struct LoadedProgram;
}
enum class FunctionCompilationState {
  Interpreted,
  Uncompiled,
  Compiling,
  PartiallyCompiled,
  Compiled
};
// Metadata access requires a valid handle. Calls report moved-from handles
// through Result instead of dereferencing their released program.
class FunctionHandle {
 public:
  bool valid () const noexcept { return static_cast<bool> (owner_); }
  std::string_view module_name () const;
  std::string_view name () const;
  const ir::Signature &signature () const;
  bool is_jump_return () const;
  ExecutionMode mode () const;
  ExecutionEngine engine () const;
  // JIT definitions become Compiled only after publication. Imported aliases
  // share their definition. Explicit compilation does not change its entry.
  FunctionCompilationState compilation_state () const;
  // BBV compiles only the selected call-entry block. Subsequent blocks remain
  // demand driven, preserving the first-version specialization policy.
  Result<void> compile () const;
  std::optional<BlockCompilationStatistics> block_statistics () const;
  // Available only for a whole-function compiled definition, not BBV. Runtime
  // state belongs to this handle's loaded program, never to a copied engine-specific handle.
  OptionalRef<const codegen::CompiledFunction> artifact () const;
  // Checked calls and generated adapters retain their loaded program, even
  // after the creating session has been destroyed. Mutation requires external
  // synchronization across all handles to the same program.
  Result<std::vector<ScalarValue>> call (std::span<const ExecutionArgument> = {}) const;
  Result<std::int64_t> call_i64 (std::span<const std::int64_t> = {}) const;
  Result<std::uint64_t> call_u64 (std::span<const std::uint64_t> = {}) const;
  Result<double> call_double (std::span<const double> = {}) const;
#define MIRNEXT_FUNCTION_LIST_CALL(name, type)                         \
  Result<type> name (std::initializer_list<type> args) const {         \
    return name (std::span<const type> (args.begin (), args.size ())); \
  }
  MIRNEXT_FUNCTION_LIST_CALL (call_i64, std::int64_t)
  MIRNEXT_FUNCTION_LIST_CALL (call_u64, std::uint64_t)
  MIRNEXT_FUNCTION_LIST_CALL (call_double, double)
#undef MIRNEXT_FUNCTION_LIST_CALL
  Result<NativeEntry> native_entry () const;
  std::uintptr_t address () const;

 private:
  friend class ExecutionSession;
  FunctionHandle (std::shared_ptr<detail::LoadedProgram> owner, std::size_t index)
    : owner_ (std::move (owner)), index_ (index) {}
  std::shared_ptr<detail::LoadedProgram> owner_;
  std::size_t index_;
  template <class T>
  Result<T> scalar_call (std::span<const T>) const;
};
}  // namespace mirnext::execution
