/* Licensed under the MIT License. Width-aware integer folding with defined arithmetic. */
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::transforms {
std::optional<std::uint64_t> fold_integer (ir::Opcode, std::optional<std::uint64_t>,
                                           std::optional<std::uint64_t> = {});
}
