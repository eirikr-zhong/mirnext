/* Licensed under the MIT License. Owning execution sessions. */
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidArgument, std::move (message)}; }
}  // namespace
ExecutionSession::ExecutionSession (std::shared_ptr<detail::LoadedProgram> state)
  : state_ (std::move (state)) {}
ExecutionSession::ExecutionSession (ExecutionSession &&) noexcept = default;
ExecutionSession &ExecutionSession::operator= (ExecutionSession &&) noexcept = default;
ExecutionSession::~ExecutionSession () = default;
const ir::Program &ExecutionSession::program () const { return state_->context; }
ExecutionMode ExecutionSession::mode () const { return state_->mode (); }
const LinkStatistics &ExecutionSession::statistics () const { return state_->link_statistics (); }
OptionalRef<const InterpreterStatistics> ExecutionSession::interpreter_statistics () const {
  if (!state_->has_interpreted) return {};
  return state_->prepared;
}
std::size_t ExecutionSession::host_entry_count () const { return state_->entry_count (); }
bool ExecutionSession::owns (const FunctionHandle &f) const noexcept {
  return state_ && state_ == f.owner_;
}
Result<FunctionHandle> ExecutionSession::find_function (std::string_view module,
                                                        std::string_view name) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  auto index = state_->find_function (module, name);
  if (!(index).has_value ()) return index.error ();
  return FunctionHandle (state_, (index).value ());
}
Result<DataHandle> ExecutionSession::find_data (std::string_view module,
                                                std::string_view name) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  auto &p = *state_;
  auto index = p.find_native (module, name);
  if (!(index).has_value ()) return index.error ();
  if (p.binding ((index).value ()).kind != detail::ProgramImage::Kind::Data)
    return invalid ("symbol is not loaded data");
  return DataHandle (state_, (index).value ());
}
Result<NativeEntry> ExecutionSession::native_entry (const FunctionHandle &f) const {
  if (!owns (f)) return invalid ("function belongs to another execution session");
  return f.native_entry ();
}
Result<void> ExecutionSession::status () const {
  return state_ ? Result<void> () : Result<void> (invalid ("moved-from execution session"));
}
Result<std::variant<FunctionHandle, DataHandle>> ExecutionSession::find_export (
  std::string_view name) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  auto &p = *state_;
  auto found = p.exports.find (name);
  if (found == p.exports.end ())
    return Error{ErrorCode::UnknownName, "unknown loaded export: " + std::string (name)};
  if (p.binding (found->second).kind == detail::ProgramImage::Kind::Function)
    return std::variant<FunctionHandle, DataHandle> (FunctionHandle (state_, found->second));
  if (p.binding (found->second).kind == detail::ProgramImage::Kind::Data)
    return std::variant<FunctionHandle, DataHandle> (DataHandle (state_, found->second));
  return invalid ("export has no runtime definition");
}
Result<std::vector<ScalarValue>> ExecutionSession::call (
  const FunctionHandle &f, std::span<const ExecutionArgument> args) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  if (!owns (f)) return invalid ("function belongs to another execution session");
  return f.call (args);
}
Result<std::int64_t> ExecutionSession::call_i64 (const FunctionHandle &f,
                                                 std::span<const std::int64_t> args) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  if (!owns (f)) return invalid ("function belongs to another execution session");
  return f.call_i64 (args);
}
Result<std::uint64_t> ExecutionSession::call_u64 (const FunctionHandle &f,
                                                  std::span<const std::uint64_t> args) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  if (!owns (f)) return invalid ("function belongs to another execution session");
  return f.call_u64 (args);
}
Result<double> ExecutionSession::call_double (const FunctionHandle &f,
                                              std::span<const double> args) const {
  auto valid = status ();
  if (!(valid).has_value ()) return valid.error ();
  if (!owns (f)) return invalid ("function belongs to another execution session");
  return f.call_double (args);
}
#define MIRNEXT_NAMED_CALL(method, type)                                                 \
  Result<type> ExecutionSession::method (std::string_view module, std::string_view name, \
                                         std::span<const type> args) const {             \
    auto function = find_function (module, name);                                        \
    if (!function.has_value ()) return function.error ();                                \
    return function.value ().method (args);                                              \
  }
MIRNEXT_NAMED_CALL (call_i64, std::int64_t)
MIRNEXT_NAMED_CALL (call_u64, std::uint64_t)
MIRNEXT_NAMED_CALL (call_double, double)
#undef MIRNEXT_NAMED_CALL

namespace {
Result<void> check_options (const ExecutionOptions &options) {
  if (options.mode != ExecutionMode::JIT && options.mode != ExecutionMode::Interpreter)
    return invalid ("invalid execution mode");
  if (options.mode == ExecutionMode::JIT && !options.select_engine
      && (options.instruction_limit || options.call_depth_limit || !options.native_calls
          || options.native_entries))
    return invalid (
      "interpreter limits, native-call policy and entry publication require interpreter mode");
  return {};
}
}  // namespace
Result<ExecutionSession> ExecutionSession::create (const ir::Program &source,
                                                   ExecutionOptions options) {
  auto copied = ir::clone (source);
  if (!(copied).has_value ()) return copied.error ();
  return create (std::move ((copied).value ()), std::move (options));
}
Result<ExecutionSession> ExecutionSession::create (const ir::Module &source,
                                                   ExecutionOptions options) {
  auto copied = ir::clone (source);
  if (!(copied).has_value ()) return copied.error ();
  return create (std::move ((copied).value ()), std::move (options));
}
Result<ExecutionSession> ExecutionSession::create (ir::Program &&source, ExecutionOptions options) {
  auto valid = check_options (options);
  if (!(valid).has_value ()) return valid.error ();
  if (options.mode == ExecutionMode::Interpreter && !options.optimization
      && !options.select_engine) {
    auto materialized = ir::materialize_stack_scopes (source);
    if (!(materialized).has_value ()) return materialized.error ();
    auto checked = source.verify ();
    if (!(checked).has_value ()) return checked.error ();
    auto target = target::TargetMachine::native ();
    if (!(target).has_value ()) return target.error ();
    auto state
      = std::make_shared<detail::LoadedProgram> (std::make_shared<ir::Program> (std::move (source)),
                                                 std::move ((target).value ()),
                                                 std::move (options));
    auto loaded = state->load ();
    if (!(loaded).has_value ()) return loaded.error ();
    return ExecutionSession (std::move (state));
  }
  auto optimization = options.optimization.value_or (transforms::OptimizationOptions{});
  auto p = transforms::optimize (std::move (source), std::move (optimization));
  if (!(p).has_value ()) return p.error ();
  options.optimization.reset ();
  return create (std::move ((p).value ()), std::move (options));
}
Result<ExecutionSession> ExecutionSession::create (transforms::OptimizedProgram program,
                                                   ExecutionOptions options) {
  auto valid = check_options (options);
  if (!(valid).has_value ()) return valid.error ();
  if (options.optimization)
    return invalid ("an optimized program cannot request another optimization pipeline");
  valid = program.verify ();
  if (!(valid).has_value ()) return valid.error ();
  auto target = target::TargetMachine::native (program.target ());
  if (!(target).has_value ()) return target.error ();
  auto engine = detail::LoadedProgram::jit_engine (options.link.compilation);
  if (!(engine).has_value ()) return engine.error ();
  if (options.mode == ExecutionMode::Interpreter) (engine).value () = ExecutionEngine::Interpreter;
  auto optimized = std::make_shared<transforms::OptimizedProgram> (std::move (program));
  std::shared_ptr<const ir::Program> context (optimized, &optimized->program ());
  auto select = std::move (options.select_engine);
  options.select_engine = {};
  auto state
    = std::make_shared<detail::LoadedProgram> (std::move (context), std::move ((target).value ()),
                                               std::move (options), std::move (optimized),
                                               (engine).value ());
  auto loaded = state->load (std::move (select));
  if (!(loaded).has_value ()) return loaded.error ();
  return ExecutionSession (std::move (state));
}
}  // namespace mirnext::execution
