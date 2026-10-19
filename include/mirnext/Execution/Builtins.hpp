/* Licensed under the MIT License. Typed runtime helper services and host ABI. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/Execution/Invocation.hpp"
namespace mirnext::execution {
std::size_t variadic_state_size (target::ABI);
Result<std::uintptr_t> advance_variadic_scalar (target::ABI, std::span<std::byte>, ir::Type,
                                                unsigned long_double_size = sizeof (long double));
Result<void> advance_variadic_block (target::ABI, std::span<std::byte> state,
                                     std::span<std::byte> destination, std::size_t size,
                                     ir::Type::BlockClass = ir::Type::BlockClass::Default);
Result<void> initialize_variadic_state (target::ABI, std::span<std::byte>,
                                        std::span<const std::byte>);
Result<std::uintptr_t> builtin_address (codegen::Builtin);
}  // namespace mirnext::execution
