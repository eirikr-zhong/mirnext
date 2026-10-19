/* Licensed under the MIT License. Owning sessions without legacy runtime state. */
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution {
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidArgument, std::move(message)}; }
}
ExecutionSession::ExecutionSession(std::shared_ptr<detail::LoadedProgram> state) : state_(std::move(state)) {}
ExecutionSession::ExecutionSession(ExecutionSession &&) noexcept = default;
ExecutionSession &ExecutionSession::operator=(ExecutionSession &&) noexcept = default;
ExecutionSession::~ExecutionSession() = default;
const ir::Context &ExecutionSession::context() const { return state_->context; }
ExecutionMode ExecutionSession::mode() const { return state_->mode(); }
const LinkStatistics &ExecutionSession::statistics() const { return state_->link_statistics(); }
const InterpreterStatistics *ExecutionSession::interpreter_statistics() const {
  return state_->has_interpreted ? &state_->prepared : nullptr;
}
std::size_t ExecutionSession::host_entry_count() const {
  return state_->entry_count();
}
bool ExecutionSession::owns(const FunctionHandle &f) const noexcept { return state_ && state_ == f.owner_; }
Result<FunctionHandle> ExecutionSession::find_function(std::string_view module,std::string_view name) const {
  auto valid = status(); if (!valid) return valid.error();
  auto index = state_->find_function(module,name); if (!index) return index.error(); return FunctionHandle(state_,*index);
}
Result<LinkedData> ExecutionSession::find_data(std::string_view module,std::string_view name) const {
  auto valid = status(); if (!valid) return valid.error(); auto &p = *state_;
  auto index = p.find_native(module,name); if (!index) return index.error();
  if (p.binding(*index).kind != detail::ProgramImage::Kind::Data) return invalid("symbol is not loaded data"); return LinkedData(state_,*index);
}
Result<NativeEntry> ExecutionSession::native_entry(const FunctionHandle &f) const {
  if (!owns(f)) return invalid("function belongs to another execution session");
  return f.native_entry();
}
Result<void> ExecutionSession::status() const { return state_ ? Result<void>() : Result<void>(invalid("moved-from execution session")); }
Result<std::variant<FunctionHandle,LinkedData>> ExecutionSession::find_export(std::string_view name) const {
  auto valid = status(); if (!valid) return valid.error();
  auto &p = *state_; auto found = p.exports.find(name);
  if (found == p.exports.end()) return Error{ErrorCode::UnknownName,"unknown loaded export: " + std::string(name)};
  if (p.binding(found->second).kind == detail::ProgramImage::Kind::Function) return std::variant<FunctionHandle,LinkedData>(FunctionHandle(state_,found->second));
  if (p.binding(found->second).kind == detail::ProgramImage::Kind::Data) return std::variant<FunctionHandle,LinkedData>(LinkedData(state_,found->second));
  return invalid("export has no runtime definition");
}
Result<std::vector<ScalarValue>> ExecutionSession::call(const FunctionHandle &f,std::span<const ExecutionArgument> args) const {
  auto valid = status(); if (!valid) return valid.error();
  if (!owns(f)) return invalid("function belongs to another execution session"); return f.call(args);
}
Result<std::int64_t> ExecutionSession::call_i64(const FunctionHandle &f,std::span<const std::int64_t> args) const {
  auto valid = status(); if (!valid) return valid.error();
  if (!owns(f)) return invalid("function belongs to another execution session"); return f.call_i64(args);
}
Result<std::uint64_t> ExecutionSession::call_u64(const FunctionHandle &f,std::span<const std::uint64_t> args) const {
  auto valid = status(); if (!valid) return valid.error();
  if (!owns(f)) return invalid("function belongs to another execution session"); return f.call_u64(args);
}
Result<double> ExecutionSession::call_double(const FunctionHandle &f,std::span<const double> args) const {
  auto valid = status(); if (!valid) return valid.error();
  if (!owns(f)) return invalid("function belongs to another execution session"); return f.call_double(args);
}
Result<ExecutionSession> ExecutionSession::finish(Result<LinkedProgram> linked) {
  if (!linked) return linked.error();
  return ExecutionSession(std::move(linked->owner_));
}
Result<ExecutionSession> ExecutionSession::finish(Result<InterpretedProgram> program) {
  if (!program) return program.error();
  return ExecutionSession(std::move(program->owner_));
}
namespace {
Result<void> check_options(const ExecutionOptions &options) {
  if (options.mode != ExecutionMode::JIT && options.mode != ExecutionMode::Interpreter) return invalid("invalid execution mode");
  if (options.mode == ExecutionMode::JIT && !options.select_engine && (options.instruction_limit || options.call_depth_limit || !options.native_calls || options.native_entries))
    return invalid("interpreter limits, native-call policy and entry publication require interpreter mode");
  return {};
}
InterpreterOptions interpreter_options(ExecutionOptions options) {
  return {std::move(options.link),options.instruction_limit,options.call_depth_limit,options.native_calls,options.native_entries};
}
template<class Source> Result<transforms::OptimizedProgram> optimized(const Source &source,const ExecutionOptions &options) {
  return transforms::optimize(source,options.optimization.value_or(transforms::OptimizationOptions{}));
}
}
Result<ExecutionSession> ExecutionSession::create(const ir::Context &context,ExecutionOptions options) {
  auto valid = check_options(options); if (!valid) return valid.error();
  if (options.mode == ExecutionMode::Interpreter && !options.optimization && !options.select_engine) return finish(InterpretedProgram::create(context,interpreter_options(std::move(options))));
  auto p = optimized(context,options); if (!p) return p.error(); options.optimization.reset();
  return create(std::move(*p),std::move(options));
}
Result<ExecutionSession> ExecutionSession::create(const ir::Module &module,ExecutionOptions options) {
  auto valid = check_options(options); if (!valid) return valid.error();
  if (options.mode == ExecutionMode::Interpreter && !options.optimization && !options.select_engine) return finish(InterpretedProgram::create(module,interpreter_options(std::move(options))));
  auto p = optimized(module,options); if (!p) return p.error(); options.optimization.reset();
  return create(std::move(*p),std::move(options));
}
Result<ExecutionSession> ExecutionSession::create(transforms::OptimizedProgram program,ExecutionOptions options) {
  auto valid = check_options(options); if (!valid) return valid.error();
  if (options.optimization) return invalid("an optimized program cannot request another optimization pipeline");
  if (options.select_engine) {
    auto valid = program.verify(); if (!valid) return valid.error();
    auto target = target::TargetMachine::native(program.target()); if (!target) return target.error();
    auto engine = detail::LoadedProgram::jit_engine(options.link.compilation); if (!engine) return engine.error();
    if (options.mode == ExecutionMode::Interpreter) *engine = ExecutionEngine::Interpreter;
    auto optimized = std::make_shared<transforms::OptimizedProgram>(std::move(program));
    std::shared_ptr<const ir::Context> context(optimized,&optimized->context());
    auto select = std::move(options.select_engine);
    auto state = std::make_shared<detail::LoadedProgram>(std::move(context),std::move(*target),interpreter_options(std::move(options)),std::move(optimized),*engine);
    auto loaded = state->load(std::move(select)); if (!loaded) return loaded.error(); return ExecutionSession(std::move(state));
  }
  if (options.mode == ExecutionMode::Interpreter) return finish(InterpretedProgram::create(std::move(program),interpreter_options(std::move(options))));
  return finish(LinkedProgram::link(std::move(program),std::move(options.link)));
}
}
