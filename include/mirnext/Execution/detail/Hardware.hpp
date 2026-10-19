/* Licensed under the MIT License. Typed storage at interpreted/native boundaries. */
#pragma once
#include "mirnext/Execution/Value.hpp"
namespace mirnext::execution::detail {
struct alignas (long double) HardwareSlot {
  ScalarValue::Storage bytes{};
};
}  // namespace mirnext::execution::detail
