/* Licensed under the MIT License. Unified owning C++ execution sessions. */
#pragma once
#include "mirnext/Execution/Interpreter.hpp"
#include "mirnext/Execution/Function.hpp"
namespace mirnext::execution {
struct ExecutionOptions {
  ExecutionMode mode = ExecutionMode::JIT;
  // Absent: JIT uses O2; interpretation executes the materialized original IR.
  std::optional<transforms::OptimizationOptions> optimization;
  LinkOptions link;
  std::size_t instruction_limit = 0, call_depth_limit = 0;
  bool native_calls = true;
  bool native_entries = false;
  // A selector uses one optimized program for every engine (O2 by default).
  // Native entries for interpreted definitions are published automatically
  // when native and interpreted definitions coexist. Mixed is a reported mode,
  // not a default policy that can be passed in mode above.
  EngineSelector select_engine;
};
// Each session owns one loaded program. Definition engine choices are immutable.
// Calls and global mutation require external synchronization. Function/data
// handles retain their program; foreign session handles are rejected.
class ExecutionSession {
 public:
  static Result<ExecutionSession> create (const ir::Context &, ExecutionOptions = {});
  static Result<ExecutionSession> create (const ir::Module &, ExecutionOptions = {});
  static Result<ExecutionSession> create (transforms::OptimizedProgram, ExecutionOptions = {});
  ExecutionSession (ExecutionSession &&) noexcept;
  ExecutionSession &operator= (ExecutionSession &&) noexcept;
  ~ExecutionSession ();
  ExecutionSession (const ExecutionSession &) = delete;
  ExecutionSession &operator= (const ExecutionSession &) = delete;
  // These accessors require a non-moved-from session.
  const ir::Context &context () const;
  ExecutionMode mode () const;
  const LinkStatistics &statistics () const;
  const InterpreterStatistics *interpreter_statistics () const;
  std::size_t host_entry_count () const;
  bool owns (const FunctionHandle &) const noexcept;
  Result<FunctionHandle> find_function (std::string_view module, std::string_view name) const;
  Result<LinkedData> find_data (std::string_view module, std::string_view name) const;
  Result<NativeEntry> native_entry (const FunctionHandle &) const;
  Result<std::variant<FunctionHandle, LinkedData>> find_export (std::string_view name) const;
  Result<std::vector<ScalarValue>> call (const FunctionHandle &,
                                         std::span<const ExecutionArgument> = {}) const;
  // Fixed signatures with one matching result and homogeneous scalar args.
  // Integer helpers accept narrow integers, bool and pointer bit patterns.
  // Native wrappers preserve host registers without changing MIR call effects.
  Result<std::int64_t> call_i64 (const FunctionHandle &, std::span<const std::int64_t> = {}) const;
  Result<std::uint64_t> call_u64 (const FunctionHandle &,
                                  std::span<const std::uint64_t> = {}) const;
  Result<double> call_double (const FunctionHandle &, std::span<const double> = {}) const;
  Result<void> status () const;

 private:
  explicit ExecutionSession (std::shared_ptr<detail::LoadedProgram>);
  static Result<ExecutionSession> finish (Result<LinkedProgram>);
  static Result<ExecutionSession> finish (Result<InterpretedProgram>);
  std::shared_ptr<detail::LoadedProgram> state_;
};
}  // namespace mirnext::execution
