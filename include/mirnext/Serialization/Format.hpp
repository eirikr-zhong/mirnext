/* Licensed under the MIT License. MIR formats construct native, independently owned IR. */
#pragma once
#include "mirnext/IR/IR.hpp"

namespace mirnext::serialization {
struct ParseOptions {
  std::string_view source_name = "<memory>";
  std::size_t max_tokens = 4 * 1024 * 1024;
  std::size_t max_instructions = 1024 * 1024;
};
Result<ir::Program> parse_text (std::string_view, ParseOptions = {});
// Portable MIR text normalizes names and metadata; dump remains a separate diagnostic format.
// Noncanonical NaN payloads have no lossless textual spelling and return a diagnostic.
Result<std::string> write_text (const ir::Program &);
enum class BinaryEncoding { Compressed, Raw };
struct BinaryOptions {
  BinaryEncoding encoding = BinaryEncoding::Compressed;
  std::string_view source_name = "<binary>";
  std::size_t max_bytes = 256 * 1024 * 1024;
  std::size_t max_strings = 4 * 1024 * 1024;
  std::size_t max_instructions = 1024 * 1024;
};
Result<std::vector<std::byte>> write_binary (const ir::Program &,
                                             BinaryEncoding = BinaryEncoding::Compressed);
Result<ir::Program> read_binary (std::span<const std::byte>, BinaryOptions = {});
}  // namespace mirnext::serialization
