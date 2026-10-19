/* Licensed under the MIT License. Shared checked execution boundary rules. */
#pragma once
#include "mirnext/Execution/Value.hpp"
namespace mirnext::execution::detail {
Result<ScalarValue> normalize_scalar (ir::Type, const ScalarValue &);
Result<void> check_argument_count (const ir::Signature &, std::size_t);
Result<ExecutionArgument> normalize_argument (ir::Type, const ExecutionArgument &,
                                              bool unnamed = false);
struct NormalizedArguments {
  NormalizedArguments () = default;
  NormalizedArguments (NormalizedArguments &&) noexcept = default;
  NormalizedArguments &operator= (NormalizedArguments &&) noexcept = default;
  NormalizedArguments (const NormalizedArguments &) = delete;
  NormalizedArguments &operator= (const NormalizedArguments &) = delete;
  // Callback block views borrow these independent by-value copies. Moving the
  // vectors retains their storage; RBLK continues to borrow caller storage.
  std::vector<std::vector<std::byte>> blocks;
  std::vector<ExecutionArgument> values;
};
Result<NormalizedArguments> normalize_arguments (const ir::Signature &,
                                                 std::span<const ExecutionArgument>,
                                                 bool copy_blocks = false);
}  // namespace mirnext::execution::detail
