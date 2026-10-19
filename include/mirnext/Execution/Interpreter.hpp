/* Licensed under the MIT License. Owning interpreter on the canonical C++ IR. */
#pragma once
#include "mirnext/Execution/Linker.hpp"
namespace mirnext::execution {
namespace detail {
struct LoadedProgram;
}
struct InterpreterOptions {
  LinkOptions link;
  // Zero means unlimited. Limits apply to the whole nested invocation.
  std::size_t instruction_limit = 0, call_depth_limit = 0;
  // Native addresses use lazily generated C++ ABI adapters. Disabling this
  // retains typed C++ callbacks and never requests native code for imports.
  // Ordinary function pointers supplied by the caller use the call's checked
  // MIR signature; known loaded targets retain signature/convention checks.
  // Cold adapters retain the memory provider and observer with the program.
  bool native_calls = true;
  // Publish ordinary-ABI entries before resolving data references. The entry
  // adapts the host ABI; function bodies still execute in the interpreter.
  bool native_entries = false;
};
struct InterpreterStatistics {
  std::size_t functions = 0, instructions = 0;
};
class InterpretedFunction {
 public:
  std::string_view module_name () const;
  std::string_view name () const;
  const ir::Signature &signature () const;
  bool is_jump_return () const;

 private:
  friend class InterpretedProgram;
  InterpretedFunction (std::shared_ptr<detail::LoadedProgram> owner, std::size_t index)
    : owner_ (std::move (owner)), index_ (index) {}
  std::shared_ptr<detail::LoadedProgram> owner_;
  std::size_t index_;
};
// Owns a materialized canonical IR snapshot and its derived execution cache.
// IR bodies are never compiled. Native imports can use C++ ABI adapters;
// native_entries separately enables host-callable interpreted entries.
// Calls/global mutation require external synchronization.
class InterpretedProgram {
 public:
  static Result<InterpretedProgram> create (const ir::Context &, InterpreterOptions = {});
  static Result<InterpretedProgram> create (const ir::Module &, InterpreterOptions = {});
  static Result<InterpretedProgram> create (transforms::OptimizedProgram, InterpreterOptions = {});
  InterpretedProgram (InterpretedProgram &&) noexcept = default;
  InterpretedProgram &operator= (InterpretedProgram &&) noexcept = default;
  InterpretedProgram (const InterpretedProgram &) = delete;
  InterpretedProgram &operator= (const InterpretedProgram &) = delete;
  const ir::Context &context () const;
  const InterpreterStatistics &statistics () const;
  const LinkStatistics &link_statistics () const;
  std::size_t native_entry_count () const;
  bool owns (const InterpretedFunction &f) const noexcept { return owner_ && owner_ == f.owner_; }
  Result<InterpretedFunction> find_function (std::string_view module, std::string_view name) const;
  Result<LinkedData> find_data (std::string_view module, std::string_view name) const;
  Result<NativeEntry> native_entry (const InterpretedFunction &) const;
  Result<std::variant<InterpretedFunction, LinkedData>> find_export (std::string_view name) const;
  Result<std::vector<ScalarValue>> call (const InterpretedFunction &,
                                         std::span<const ExecutionArgument> = {}) const;
  Result<std::int64_t> call_i64 (const InterpretedFunction &,
                                 std::span<const std::int64_t> = {}) const;

 private:
  friend class ExecutionSession;
  explicit InterpretedProgram (std::shared_ptr<detail::LoadedProgram> owner)
    : owner_ (std::move (owner)) {}
  static Result<InterpretedProgram> load (std::shared_ptr<const ir::Context>, InterpreterOptions);
  std::shared_ptr<detail::LoadedProgram> owner_;
};
}  // namespace mirnext::execution
