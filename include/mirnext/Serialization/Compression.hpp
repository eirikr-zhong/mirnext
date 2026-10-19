/* Licensed under the MIT License. Checked MIR compression without runtime objects. */
#pragma once
#include "mirnext/Support/Result.hpp"
#include <cstddef>
#include <span>
#include <vector>
namespace mirnext::serialization {
Result<std::vector<std::byte>> compress (std::span<const std::byte>);
Result<std::vector<std::byte>> decompress (std::span<const std::byte>, std::size_t max_bytes);
}  // namespace mirnext::serialization
