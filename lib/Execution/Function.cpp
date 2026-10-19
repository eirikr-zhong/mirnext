/* Licensed under the MIT License. Checked calls owned by the loaded program. */
#include "mirnext/Execution/Function.hpp"
#include "mirnext/Execution/Invocation.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution {
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidArgument,std::move(message)}; }
Error diagnostic(const FunctionHandle &f,Error error) {
  error.message = std::string(f.module_name()) + '/' + std::string(f.name()) + " [session:host-entry]: " + error.message; return error;
}
template<class T> bool matches(ir::Type type) {
  if constexpr (std::is_same_v<T,double>) return type.kind() == ir::Type::Kind::F64;
  else return type.is_integer();
}
template<class T> Result<void> check_signature(const FunctionHandle &f,std::span<const T> args) {
  auto &s = f.signature();
  if ((args.size() && !args.data()) || args.size() > static_cast<std::size_t>(INT64_MAX)/sizeof(T)) return invalid("invalid scalar argument span");
  if (f.is_jump_return() || s.variadic || s.results.size() != 1 || !matches<T>(s.results[0]))
    return invalid("scalar helper requires one matching result and a fixed ordinary calling convention");
  if (s.parameters.size() != args.size()) return invalid("argument count does not match signature");
  for (auto type : s.parameters) if (!matches<T>(type)) return invalid("scalar argument type mismatch");
  return {};
}
}
std::string_view FunctionHandle::module_name() const { return owner_->bindings[index_].source->module().name(); }
std::string_view FunctionHandle::name() const { return owner_->bindings[index_].source->name(); }
const ir::Signature &FunctionHandle::signature() const { return *owner_->binding(index_).signature; }
bool FunctionHandle::is_jump_return() const { return owner_->binding(index_).jump; }
ExecutionEngine FunctionHandle::engine() const { return owner_->engine(index_); }
ExecutionMode FunctionHandle::mode() const { return engine() == ExecutionEngine::Interpreter ? ExecutionMode::Interpreter : ExecutionMode::JIT; }
FunctionCompilationState FunctionHandle::compilation_state() const {
  if (mode() == ExecutionMode::Interpreter) return FunctionCompilationState::Interpreted;
  auto index = owner_->binding(index_).function; if (!index) return FunctionCompilationState::Compiled;
  if (auto &blocks = owner_->functions[*index]->blocks) {
    if (blocks->compiling()) return FunctionCompilationState::Compiling;
    auto &s = blocks->statistics();
    if (s.compiled_blocks == s.blocks) return FunctionCompilationState::Compiled;
    return s.compiled_blocks ? FunctionCompilationState::PartiallyCompiled : FunctionCompilationState::Uncompiled;
  }
  switch (owner_->functions[*index]->state) {
    case detail::LoadedProgram::CodeState::Uncompiled: return FunctionCompilationState::Uncompiled;
    case detail::LoadedProgram::CodeState::Compiling: return FunctionCompilationState::Compiling;
    case detail::LoadedProgram::CodeState::Compiled: return FunctionCompilationState::Compiled;
  } return FunctionCompilationState::Uncompiled;
}
Result<void> FunctionHandle::compile() const {
  if (!owner_) return invalid("moved-from function handle");
  if (mode() == ExecutionMode::Interpreter) return invalid("explicit native compilation requires JIT mode");
  auto index = owner_->binding(index_).function; return index ? owner_->compile_native(*index) : Result<void>();
}
const codegen::CompiledFunction *FunctionHandle::artifact() const { return owner_->artifact(index_); }
std::optional<BlockCompilationStatistics> FunctionHandle::block_statistics() const {
  if (owner_) if (auto index = owner_->binding(index_).function) if (auto &blocks = owner_->functions[*index]->blocks) return blocks->statistics();
  return {};
}
Result<std::vector<ScalarValue>> FunctionHandle::call(std::span<const ExecutionArgument> args) const {
  if (!owner_) return invalid("moved-from function handle");
  if (is_jump_return()) return diagnostic(*this,invalid("host calls require the ordinary calling convention"));
  auto result = owner_->invoke(index_,args);
  if (!result && mode() == ExecutionMode::JIT) return diagnostic(*this,result.error()); return result;
}
Result<NativeEntry> FunctionHandle::native_entry() const {
  if (!owner_) return invalid("moved-from function handle");
  if (mode() == ExecutionMode::Interpreter && !owner_->options.native_entries) return invalid("native entries must be enabled before loading and data relocation");
  if (is_jump_return()) return invalid("native entry requires the ordinary calling convention");
  return NativeEntry(owner_,index_,owner_->binding(index_).address);
}
template<class T> Result<T> FunctionHandle::scalar_call(std::span<const T> args) const {
  if (!owner_) return invalid("moved-from function handle");
  auto &f = *this;
  auto checked = check_signature(f,args); if (!checked) return diagnostic(f,checked.error());
  if (mode() == ExecutionMode::Interpreter || (owner_->has_interpreted && !owner_->hardware.empty())) {
    std::vector<ExecutionArgument> values; values.reserve(args.size()); for (auto value : args) values.emplace_back(value);
    auto result = call(values); if (!result) return result.error();
    if constexpr (std::is_same_v<T,double>) return (*result)[0].as_double();
    else if constexpr (std::is_same_v<T,std::uint64_t>) return (*result)[0].as_u64();
    else return (*result)[0].as_i64();
  }
  auto entry = owner_->native_calls->scalar_entry(owner_->binding(index_).address,signature()); if (!entry) return diagnostic(f,entry.error());
  if (owner_->native_boundary) {
    auto value = owner_->native_boundary->recovery().invoke<T>(*entry,reinterpret_cast<std::uintptr_t>(args.data()));
    if (!value) return diagnostic(f,value.error()); return value;
  }
  ExecutionInvocation invocation; auto value = reinterpret_cast<T (*)(const T *)>(*entry)(args.data());
  auto result = invocation.status(); if (!result) return diagnostic(f,result.error()); return value;
}
Result<std::int64_t> FunctionHandle::call_i64(std::span<const std::int64_t> args) const { return scalar_call(args); }
Result<std::uint64_t> FunctionHandle::call_u64(std::span<const std::uint64_t> args) const { return scalar_call(args); }
Result<double> FunctionHandle::call_double(std::span<const double> args) const { return scalar_call(args); }
}
