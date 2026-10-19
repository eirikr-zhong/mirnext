/* Licensed under the MIT License. One owning loaded program for all execution services. */
#pragma once
#include "mirnext/Execution/detail/Interpreter.hpp"
#include "mirnext/Execution/detail/BBVRuntime.hpp"
#include "mirnext/Execution/detail/NativeBoundary.hpp"
#include "mirnext/Execution/Engine.hpp"
namespace mirnext::execution::detail {
struct LoadedProgram : ProgramImage {
  std::shared_ptr<const transforms::OptimizedProgram> optimized;
  InterpreterOptions options;
  InterpreterStatistics prepared;
  struct Location {
    InterpreterBody *body;
    std::size_t pc;
  };
  std::unordered_map<std::uintptr_t, Location> locations;
  std::unordered_map<std::uintptr_t, std::vector<std::size_t>> callees;
  std::unordered_set<std::uintptr_t> virtual_functions;
  std::vector<InterpreterSlot> hardware;
  std::vector<std::pair<ir::Type, std::string_view>> hardware_bindings;
  std::unordered_map<unsigned, std::size_t> hardware_indices;
  std::vector<std::pair<std::uintptr_t, std::uintptr_t>> readonly;
  std::unique_ptr<NativeCalls> native_calls;
  // Declared before block runtimes: their recovery continuation and descriptors
  // stay alive until all borrowing runtimes and entries have been destroyed.
  std::optional<NativeBoundary> native_boundary;
  struct EntryDescriptor {
    LoadedProgram &state;
    std::size_t binding;
  };
  std::vector<std::unique_ptr<EntryDescriptor>> entry_descriptors;
  struct EntryCode {
    codegen::CompiledFunction artifact;
    ExecutableCode code;
  };
  std::vector<EntryCode> entries;
  ExecutionEngine default_engine;
  bool has_native = false, has_interpreted = false;
  LinkStatistics total_statistics;
  enum class CodeState { Uncompiled, Compiling, Compiled };
  struct Code {
    const ir::Function *source;
    ExecutionEngine engine;
    std::unique_ptr<InterpreterBody> interpreter;
    CodeState state = CodeState::Uncompiled;
    std::optional<codegen::CompiledFunction> artifact;
    std::optional<ExecutableCode> code;
    std::uintptr_t destination = 0;
    std::unique_ptr<BBVRuntime> blocks;
  };
  // Records and their destination slots remain stable as compilation proceeds.
  std::vector<std::unique_ptr<Code>> functions;
  struct LazyDescriptor {
    LoadedProgram &state;
    std::size_t function;
  };
  std::vector<std::unique_ptr<LazyDescriptor>> lazy_descriptors;
  std::vector<ExecutableCode> lazy_entries;
  struct JumpDescriptor {
    LoadedProgram &state;
    std::size_t binding;
    std::uintptr_t destination = 0;
  };
  std::vector<std::unique_ptr<JumpDescriptor>> jump_descriptors;
  std::vector<ExecutableCode> jump_entries;
  std::unordered_map<const ir::Label *, std::pair<std::size_t, std::size_t>> label_offsets;
  std::unordered_map<const ir::Label *, const codegen::MachineLabel *> block_labels;
  LoadedProgram (std::shared_ptr<const ir::Context> c, target::TargetMachine t,
                 InterpreterOptions o, std::shared_ptr<const transforms::OptimizedProgram> p = {},
                 ExecutionEngine engine = ExecutionEngine::Interpreter)
    : ProgramImage (std::move (c), std::move (t)),
      optimized (std::move (p)),
      options (std::move (o)),
      default_engine (engine) {}
  ExecutionEngine engine (std::size_t) const;
  static Result<ExecutionEngine> jit_engine (JITCompilation);
  ExecutionMode mode () const;
  Result<void> load (EngineSelector = {});
  Result<std::size_t> find_function (std::string_view module, std::string_view name) const;
  Result<std::size_t> find_native (std::string_view module, std::string_view name) const;
  const codegen::CompiledFunction *artifact (std::size_t index) const;
  std::size_t entry_count () const {
    return entries.size () + (native_calls ? native_calls->size () : 0);
  }
  Result<std::vector<ScalarValue>> invoke (std::size_t, std::span<const ExecutionArgument>);
  Result<void> register_functions (const EngineSelector &);
  Result<void> prepare_hardware ();
  Result<void> initialize ();
  Result<void> index_callees ();
  Result<void> protect_native_entries ();
  Result<void> publish_jump_guards ();
  Result<void> prepare_block_versioning ();
  const LinkStatistics &link_statistics ();
  Result<std::optional<CodeReservation>> prepare_eager ();
  Result<void> publish_eager (CodeReservation &);
  Result<void> compile_native (std::size_t);
  Result<void> publish_lazy_entries ();
  Result<void> prepare_lazy_roots ();
  Result<std::uint64_t> resolve_code (const codegen::CodeSymbolRequest &) const;
  Result<std::uint64_t> native_label_address (const ir::Label &) const;
  Result<void> prepare ();
  Result<void> publish_entries ();
  Result<std::uintptr_t> label_address (const ir::Label &) const;
  Result<void> writable (std::uintptr_t, std::size_t) const;
};
}  // namespace mirnext::execution::detail
