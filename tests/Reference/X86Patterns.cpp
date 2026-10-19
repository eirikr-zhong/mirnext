/* Licensed under the MIT License. Test-only original x86 declaration oracle. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include "mirnext/CodeGen/Detail/X86Encoding.hpp"
#include "COracle.h"
#include <string>

namespace original_x86 {
enum class Code {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) name,
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
  Unspec, Use, Phi, Invalid, Bound
};
struct pattern { Code code; const char *pattern, *replacement; int max_insn_size = 0; };
// These isolated declaration includes never enter the product dependency graph.
#include "X86PatternAliases.hpp"
#include "mir-machine-patterns-x86_64.def"
}
TEST_CASE("owning x86 declaration order and encodings equal the original expanded table") {
  using namespace mirnext; using namespace mirnext::codegen;
  auto actual = x86_instruction_patterns(); REQUIRE(actual.size() == std::size(original_x86::patterns) + 3);
  std::size_t private_moves = 0;
  for (std::size_t n = 0; n < std::size(original_x86::patterns); ++n) {
    CAPTURE(n); auto &expected = original_x86::patterns[n];
    std::string constraints(expected.pattern), encoding(expected.replacement);
    if (expected.code == original_x86::Code::Unspec) {
      ++private_moves; REQUIRE(constraints.starts_with("c0 ")); constraints.erase(0, 3);
      // The private aligned move no longer carries the old UNSPEC selector.
      for (std::size_t at = 0; at + 1 < encoding.size(); ++at) {
        if (encoding[at] == 'r' || encoding[at] == 'R' || encoding[at] == 'm') {
          if (encoding[at + 1] == '1') encoding[at + 1] = '0';
          else if (encoding[at + 1] == '2') encoding[at + 1] = '1';
        }
      }
      CHECK(actual[n].opcode == MachineOpcode(MachinePseudoOpcode::X86Mov128));
    } else CHECK(actual[n].opcode == MachineOpcode(static_cast<ir::Opcode>(expected.code)));
    CHECK(actual[n].index == n); CHECK(actual[n].constraints == constraints); CHECK(actual[n].encoding == encoding);
    auto recipe = x86_encoding_recipe(n); REQUIRE(recipe);
    if (target::TargetMachine::native()->architecture() == target::Architecture::X86_64)
      CHECK(recipe->maximum_size == _MIR_opt_test_x86_encoding_max(n));
  }
  CHECK(private_moves == 3);
  for (auto &p : actual.last(3)) { CHECK(p.opcode == MachineOpcode(MachinePseudoOpcode::X86Mov128Unaligned)); CHECK(p.encoding.starts_with("F3 ")); }
}
