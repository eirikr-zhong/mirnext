/* Licensed under the MIT License. Owning linked IR, globals, symbols and code. */
#pragma once
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Execution/Value.hpp"
#include "mirnext/Execution/Entry.hpp"
#include "mirnext/Execution/BlockVersioning.hpp"
namespace mirnext::execution {
struct DataPlacement { const ir::GlobalData *data; std::size_t offset,size; };
struct DataSectionLayout { std::size_t size = 0; std::vector<DataPlacement> items; };
// Borrows IR identities. Anonymous consecutive data shares a section without
// internal padding; non-data items and named data start a new section.
Result<std::vector<DataSectionLayout>> layout_module_data(const ir::Module &,const target::TargetMachine &);
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
  std::string_view module,name;
  const ir::Signature *signature;
  const ir::GlobalVariable *variable;
};
enum class JITCompilation { Eager, Lazy, BlockVersioning };
struct LinkOptions {
  // Called once per distinct unresolved name. Returned host storage/code must
  // outlive all handles to this program. Definitions in the program win.
  std::function<Result<ExternalSymbol>(const ImportRequest &)> resolve;
  std::shared_ptr<CodeMemoryProvider> memory_provider = native_code_memory_provider();
  std::function<Result<void>(const codegen::CompilationSnapshot &)> compilation_observer;
  // Lazy mode retains stable entries; label-data owners, jump-return bodies
  // and expression initialization are prepared at load time as required.
  JITCompilation compilation = JITCompilation::Eager;
  // BlockVersioning prepares machine functions at load, then compiles reached
  // blocks on demand. Stable native entries also protect raw host reentry.
  BlockCompilationOptions block_versioning;
};
struct ProgramOptions { transforms::OptimizationOptions optimization; LinkOptions link; };
struct LinkStatistics {
  std::size_t sections = 0,functions = 0,expressions = 0,external_resolutions = 0,code_batches = 0,code_bytes = 0,compiled_functions = 0;
  // In BBV mode compiled_functions counts published call-entry blocks.
  std::size_t compiled_blocks = 0,block_compilation_attempts = 0;
};
namespace detail { struct LoadedProgram; struct ProgramImage; }
class LinkedFunction {
public:
  std::string_view module_name() const;
  std::string_view name() const;
  const ir::Signature &signature() const;
  bool is_jump_return() const;
  // Native MIR ABI entry. Hardware bindings retain their MIR side effects;
  // a host caller must arrange preservation at its explicit ABI boundary.
  std::uintptr_t address() const;
  const codegen::CompiledFunction *artifact() const;
  std::optional<BlockCompilationStatistics> block_statistics() const;
private:
  friend class LinkedProgram;
  LinkedFunction(std::shared_ptr<detail::LoadedProgram> owner,std::size_t n) : owner_(std::move(owner)),index_(n) {}
  std::shared_ptr<detail::LoadedProgram> owner_;
  std::size_t index_;
};
class LinkedData {
public:
  std::string_view module_name() const;
  std::string_view name() const;
  std::span<const std::byte> bytes() const;
  Result<std::span<std::byte>> writable_bytes() const;
  std::uintptr_t address() const;
private:
  friend class LinkedProgram;
  friend class InterpretedProgram;
  friend class ExecutionSession;
  LinkedData(std::shared_ptr<detail::ProgramImage> owner,std::size_t n) : owner_(std::move(owner)),index_(n) {}
  std::shared_ptr<detail::ProgramImage> owner_;
  std::size_t index_;
};
class LinkedProgram {
public:
  static Result<LinkedProgram> create(const ir::Context &,ProgramOptions = {});
  static Result<LinkedProgram> create(const ir::Module &,ProgramOptions = {});
  static Result<LinkedProgram> link(transforms::OptimizedProgram,LinkOptions = {});
  LinkedProgram(LinkedProgram &&) noexcept = default;
  LinkedProgram &operator=(LinkedProgram &&) noexcept = default;
  LinkedProgram(const LinkedProgram &) = delete;
  LinkedProgram &operator=(const LinkedProgram &) = delete;
  bool valid() const noexcept { return static_cast<bool>(owner_); }
  bool owns(const LinkedFunction &f) const noexcept { return owner_ && f.owner_ == owner_; }
  const ir::Context &context() const;
  const LinkStatistics &statistics() const;
  Result<LinkedFunction> find_function(std::string_view module,std::string_view name) const;
  Result<LinkedData> find_data(std::string_view module,std::string_view name) const;
  // Raw MIR ABI; explicit hardware bindings retain their native side effects.
  Result<NativeEntry> native_entry(const LinkedFunction &) const;
  Result<std::variant<LinkedFunction,LinkedData>> find_export(std::string_view name) const;
private:
  friend class ExecutionSession;
  explicit LinkedProgram(std::shared_ptr<detail::LoadedProgram> owner) : owner_(std::move(owner)) {}
  std::shared_ptr<detail::LoadedProgram> owner_;
};
}
