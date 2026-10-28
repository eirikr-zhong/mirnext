/* Licensed under the MIT License. Owning loaded data handle. */
#pragma once
#include "mirnext/Support/Result.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <memory>
#include <string_view>
namespace mirnext::execution {
namespace detail {
struct ProgramImage;
}
class DataHandle {
 public:
  std::string_view module_name () const;
  std::string_view name () const;
  std::span<const std::byte> bytes () const;
  Result<std::span<std::byte>> writable_bytes () const;
  std::uintptr_t address () const;

 private:
  friend class ExecutionSession;
  DataHandle (std::shared_ptr<detail::ProgramImage> owner, std::size_t n)
    : owner_ (std::move (owner)), index_ (n) {}
  std::shared_ptr<detail::ProgramImage> owner_;
  std::size_t index_;
};
}  // namespace mirnext::execution
