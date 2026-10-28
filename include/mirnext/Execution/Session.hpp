/* Licensed under the MIT License. Unified owning C++ execution sessions. */
#pragma once
#include "mirnext/Execution/Options.hpp"
#include "mirnext/Execution/Data.hpp"
#include "mirnext/Execution/Function.hpp"
#include "mirnext/IR/Builder.hpp"
namespace mirnext::execution {
// Each session owns one loaded program. Definition engine choices are immutable.
// Calls and global mutation require external synchronization. Function/data
// handles retain their program; foreign session handles are rejected.
class ExecutionSession {
 public:
  static Result<ExecutionSession> create (const ir::Program &, ExecutionOptions = {});
  static Result<ExecutionSession> create (ir::Program &&, ExecutionOptions = {});
  static Result<ExecutionSession> create (const ir::Module &, ExecutionOptions = {});
  static Result<ExecutionSession> create (transforms::OptimizedProgram, ExecutionOptions = {});
  ExecutionSession (ExecutionSession &&) noexcept;
  ExecutionSession &operator= (ExecutionSession &&) noexcept;
  ~ExecutionSession ();
  ExecutionSession (const ExecutionSession &) = delete;
  ExecutionSession &operator= (const ExecutionSession &) = delete;
  // These accessors require a non-moved-from session.
  const ir::Program &program () const;
  ExecutionMode mode () const;
  const LinkStatistics &statistics () const;
  OptionalRef<const InterpreterStatistics> interpreter_statistics () const;
  std::size_t host_entry_count () const;
  bool owns (const FunctionHandle &) const noexcept;
  Result<FunctionHandle> find_function (std::string_view module, std::string_view name) const;
  Result<DataHandle> find_data (std::string_view module, std::string_view name) const;
  Result<NativeEntry> native_entry (const FunctionHandle &) const;
  Result<std::variant<FunctionHandle, DataHandle>> find_export (std::string_view name) const;
  Result<std::vector<ScalarValue>> call (const FunctionHandle &,
                                         std::span<const ExecutionArgument> = {}) const;
  // Fixed signatures with one matching result and homogeneous scalar args.
  // Integer helpers accept narrow integers, bool and pointer bit patterns.
  // Native wrappers preserve host registers without changing MIR call effects.
  Result<std::int64_t> call_i64 (const FunctionHandle &, std::span<const std::int64_t> = {}) const;
  Result<std::uint64_t> call_u64 (const FunctionHandle &,
                                  std::span<const std::uint64_t> = {}) const;
  Result<double> call_double (const FunctionHandle &, std::span<const double> = {}) const;
  Result<std::int64_t> call_i64 (std::string_view module, std::string_view name,
                                 std::span<const std::int64_t> = {}) const;
  Result<std::uint64_t> call_u64 (std::string_view module, std::string_view name,
                                  std::span<const std::uint64_t> = {}) const;
  Result<double> call_double (std::string_view module, std::string_view name,
                              std::span<const double> = {}) const;
#define MIRNEXT_SESSION_LIST_CALL(name, type)                                                  \
  Result<type> name (std::string_view module, std::string_view function,                       \
                     std::initializer_list<type> args) const {                                 \
    return name (module, function, std::span<const type> (args.begin (), args.size ()));       \
  }                                                                                            \
  Result<type> name (const FunctionHandle &function, std::initializer_list<type> args) const { \
    return name (function, std::span<const type> (args.begin (), args.size ()));               \
  }
  MIRNEXT_SESSION_LIST_CALL (call_i64, std::int64_t)
  MIRNEXT_SESSION_LIST_CALL (call_u64, std::uint64_t)
  MIRNEXT_SESSION_LIST_CALL (call_double, double)
#undef MIRNEXT_SESSION_LIST_CALL
  Result<void> status () const;

 private:
  explicit ExecutionSession (std::shared_ptr<detail::LoadedProgram>);
  std::shared_ptr<detail::LoadedProgram> state_;
};
}  // namespace mirnext::execution
