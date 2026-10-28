/* Licensed under the MIT License. Owning linked IR, globals, symbols and code. */
#pragma once
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Execution/Value.hpp"
#include "mirnext/Execution/Entry.hpp"
#include "mirnext/Execution/BlockVersioning.hpp"
#include "mirnext/Execution/Engine.hpp"
namespace mirnext::execution {
struct DataPlacement {
  DataPlacement (const ir::GlobalData &data, std::size_t offset, std::size_t size)
    : data_ (&data), offset (offset), size (size) {}
  const ir::GlobalData &data () const { return *data_; }
  std::size_t offset, size;

 private:
  const ir::GlobalData *data_;
};
struct DataSectionLayout {
  std::size_t size = 0;
  std::vector<DataPlacement> items;
};
// Borrows IR identities. Anonymous consecutive data shares a section without
// internal padding; non-data items and named data start a new section.
Result<std::vector<DataSectionLayout>> layout_module_data (const ir::Module &,
                                                           const target::TargetMachine &);
struct ExternalData {
  std::size_t size;
  bool writable = true;
  std::optional<ir::Type> scalar_type;
};
struct ExternalSymbol {
  std::uintptr_t address = 0;
  // Exactly one of signature/data identifies the host ABI binding.
  std::optional<ir::Signature> signature;
  std::optional<ExternalData> data;
  bool jump_return = false;
  // Optional typed C++ implementation for interpreted calls. Wholly native
  // programs require an ABI address. Mixed sessions can publish a checked ABI
  // entry for a callback-only import, as for interpreted definitions.
  InterpreterHostFunction interpret;
};
struct ImportRequest {
  std::string_view module, name;
  ImportRequest (std::string_view module, std::string_view name,
                 OptionalRef<const ir::Signature> signature = {},
                 OptionalRef<const ir::GlobalVariable> variable = {})
    : module (module), name (name), signature_ (signature), variable_ (variable) {}
  OptionalRef<const ir::Signature> signature () const { return signature_; }
  OptionalRef<const ir::GlobalVariable> variable () const { return variable_; }

 private:
  OptionalRef<const ir::Signature> signature_;
  OptionalRef<const ir::GlobalVariable> variable_;
};
enum class JITCompilation { Eager, Lazy, BlockVersioning };
struct LinkOptions {
  // Called once per distinct unresolved name. Returned host storage/code must
  // outlive all handles to this program. Definitions in the program win.
  std::function<Result<ExternalSymbol> (const ImportRequest &)> resolve;
  std::shared_ptr<CodeMemoryProvider> memory_provider = native_code_memory_provider ();
  std::function<Result<void> (const codegen::CompilationSnapshot &)> compilation_observer;
  // Lazy mode retains stable entries; label-data owners, jump-return bodies
  // and expression initialization are prepared at load time as required.
  JITCompilation compilation = JITCompilation::Eager;
  // BlockVersioning prepares machine functions at load, then compiles reached
  // blocks on demand. Stable native entries also protect raw host reentry.
  BlockCompilationOptions block_versioning;
};
struct LinkStatistics {
  std::size_t sections = 0, functions = 0, expressions = 0, external_resolutions = 0,
              code_batches = 0, code_bytes = 0, compiled_functions = 0;
  // In BBV mode compiled_functions counts published call-entry blocks.
  std::size_t compiled_blocks = 0, block_compilation_attempts = 0;
};
struct InterpreterStatistics {
  std::size_t functions = 0, instructions = 0;
};
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
}  // namespace mirnext::execution
