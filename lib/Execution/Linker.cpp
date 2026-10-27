/* Licensed under the MIT License. Native program API on shared loaded C++ state. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
}  // namespace
std::string_view LinkedFunction::module_name () const {
  return owner_->bindings[index_].source->module ().name ();
}
std::string_view LinkedFunction::name () const { return owner_->bindings[index_].source->name (); }
const ir::Signature &LinkedFunction::signature () const {
  return *owner_->binding (index_).signature;
}
bool LinkedFunction::is_jump_return () const { return owner_->binding (index_).jump; }
std::uintptr_t LinkedFunction::address () const { return owner_->binding (index_).address; }
const codegen::CompiledFunction *LinkedFunction::artifact () const {
  return owner_->artifact (index_);
}
std::optional<BlockCompilationStatistics> LinkedFunction::block_statistics () const {
  if (auto index = owner_->binding (index_).function)
    if (auto &blocks = owner_->functions[*index]->blocks) return blocks->statistics ();
  return {};
}
std::string_view LinkedData::module_name () const {
  return owner_->bindings[index_].source->module ().name ();
}
std::string_view LinkedData::name () const { return owner_->bindings[index_].source->name (); }
std::uintptr_t LinkedData::address () const { return owner_->binding (index_).address; }
std::span<const std::byte> LinkedData::bytes () const {
  auto &binding = owner_->binding (index_);
  return {reinterpret_cast<const std::byte *> (binding.address), binding.size};
}
Result<std::span<std::byte>> LinkedData::writable_bytes () const {
  auto &binding = owner_->binding (index_);
  auto variable = owner_->bindings[index_].source->variable ();
  if (!binding.writable || (variable && !variable->writable ()))
    return invalid ("linked data is read-only");
  return std::span (reinterpret_cast<std::byte *> (binding.address), binding.size);
}
const ir::Context &LinkedProgram::context () const { return owner_->context; }
const LinkStatistics &LinkedProgram::statistics () const { return owner_->link_statistics (); }
Result<NativeEntry> LinkedProgram::native_entry (const LinkedFunction &f) const {
  if (!owns (f)) return invalid ("function belongs to another linked program");
  if (f.is_jump_return ()) return invalid ("native entry requires the ordinary calling convention");
  return NativeEntry (owner_, f.index_, f.address ());
}
Result<LinkedFunction> LinkedProgram::find_function (std::string_view module,
                                                     std::string_view name) const {
  if (!owner_) return invalid ("moved-from linked program");
  auto found = owner_->find_function (module, name);
  if (!found) return found.error ();
  return LinkedFunction (owner_, *found);
}
Result<LinkedData> LinkedProgram::find_data (std::string_view module, std::string_view name) const {
  if (!owner_) return invalid ("moved-from linked program");
  auto found = owner_->find_native (module, name);
  if (!found) return found.error ();
  if (owner_->binding (*found).kind != detail::LoadedProgram::Kind::Data)
    return invalid ("linked symbol is not data");
  return LinkedData (owner_, *found);
}
Result<std::variant<LinkedFunction, LinkedData>> LinkedProgram::find_export (
  std::string_view name) const {
  if (!owner_) return invalid ("moved-from linked program");
  auto found = owner_->exports.find (name);
  if (found == owner_->exports.end ())
    return Error{ErrorCode::UnknownName, "unknown linked export: " + std::string (name)};
  if (owner_->binding (found->second).kind == detail::LoadedProgram::Kind::Function)
    return std::variant<LinkedFunction, LinkedData> (LinkedFunction (owner_, found->second));
  if (owner_->binding (found->second).kind == detail::LoadedProgram::Kind::Data)
    return std::variant<LinkedFunction, LinkedData> (LinkedData (owner_, found->second));
  return invalid ("export has no runtime definition");
}
Result<LinkedProgram> LinkedProgram::link (transforms::OptimizedProgram program,
                                           LinkOptions options) {
  auto valid = program.verify ();
  if (!valid) return valid.error ();
  auto target = target::TargetMachine::native (program.target ());
  if (!target) return target.error ();
  auto optimized = std::make_shared<transforms::OptimizedProgram> (std::move (program));
  std::shared_ptr<const ir::Context> context (optimized, &optimized->context ());
  auto engine = detail::LoadedProgram::jit_engine (options.compilation);
  if (!engine) return engine.error ();
  auto state
    = std::make_shared<detail::LoadedProgram> (std::move (context), std::move (*target),
                                               InterpreterOptions{.link = std::move (options)},
                                               std::move (optimized), *engine);
  auto result = state->load ();
  if (!result) return result.error ();
  return LinkedProgram (std::move (state));
}
Result<LinkedProgram> LinkedProgram::create (const ir::Context &source, ProgramOptions options) {
  auto program = transforms::optimize (source, std::move (options.optimization));
  if (!program) return program.error ();
  return link (std::move (*program), std::move (options.link));
}
Result<LinkedProgram> LinkedProgram::create (const ir::Module &source, ProgramOptions options) {
  auto program = transforms::optimize (source, std::move (options.optimization));
  if (!program) return program.error ();
  return link (std::move (*program), std::move (options.link));
}
}  // namespace mirnext::execution
