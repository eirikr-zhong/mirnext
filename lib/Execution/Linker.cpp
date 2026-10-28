/* Licensed under the MIT License. Native program API on shared loaded C++ state. */
#include "mirnext/Execution/detail/LoadedProgram.hpp"
namespace mirnext::execution {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
}  // namespace
std::string_view DataHandle::module_name () const {
  return owner_ ? owner_->bindings[index_].source->module ().name () : std::string_view{};
}
std::string_view DataHandle::name () const {
  return owner_ ? owner_->bindings[index_].source->name () : std::string_view{};
}
std::uintptr_t DataHandle::address () const {
  return owner_ ? owner_->binding (index_).address : 0;
}
std::span<const std::byte> DataHandle::bytes () const {
  if (!owner_) return {};
  auto &binding = owner_->binding (index_);
  return {reinterpret_cast<const std::byte *> (binding.address), binding.size};
}
Result<std::span<std::byte>> DataHandle::writable_bytes () const {
  if (!owner_) return invalid ("moved-from data handle");
  auto &binding = owner_->binding (index_);
  auto variable = owner_->bindings[index_].source->variable ().get ();
  if (!binding.writable || (variable && !variable->writable ()))
    return invalid ("linked data is read-only");
  return std::span (reinterpret_cast<std::byte *> (binding.address), binding.size);
}
}  // namespace mirnext::execution
