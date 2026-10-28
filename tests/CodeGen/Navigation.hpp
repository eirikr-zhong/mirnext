/* Licensed under the MIT License. Checked optional machine navigation for tests. */
#pragma once
#include <doctest/doctest.h>
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::test {
inline OptionalRef<codegen::MachineInstruction> neighbor (
  Result<OptionalRef<codegen::MachineInstruction>> result) {
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return (result).value ();
}
}  // namespace mirnext::test
