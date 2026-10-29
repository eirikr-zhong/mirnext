/* Licensed under the MIT License. Private vector instructions participate in machine analyses. */
#include "mirnext/CodeGen/Detail/NativeVector.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/IR/Builder.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
TEST_CASE ("every native vector instruction has typed patterns, bounds and register effects") {
  const MachinePseudoOpcode opcodes[]{
#define MIRNEXT_NATIVE_X86(name, roles, tied, maximum, constraints, encoding) \
  MachinePseudoOpcode::name,
#define MIRNEXT_NATIVE_A64 MIRNEXT_NATIVE_X86
#include "mirnext/Target/NativeVectorOps.def"
#undef MIRNEXT_NATIVE_A64
#undef MIRNEXT_NATIVE_X86
  };
  auto target = target::TargetMachine::native ();
  REQUIRE (target.has_value ());
  for (auto opcode : opcodes) {
    auto d = mirnext::codegen::detail::native_vector_descriptor (opcode);
    REQUIRE (d);
    CAPTURE (d->name);
    MachineFunction f ("test", "native-vectors", {{}, {}}, target.value ());
    auto &block = mirnext::codegen::detail::access (f).create_block ();
    MachineRewriter rw (f);
    std::vector<MachineOperand> ops;
    std::size_t uses = 0, definitions = 0;
    for (char role : d->roles) {
      if (role == 'n') {
        ops.emplace_back (std::uint64_t (d->maximum));
        continue;
      }
      auto type
        = role == 'i' || role == 'I' ? Type::i64 () : Type::vector128 (Type::VectorElement::U8, 16);
      auto reg = mirnext::codegen::detail::access (f).create_register (type);
      REQUIRE (reg.has_value ());
      ops.emplace_back (reg.value ());
      if (role >= 'A' && role <= 'Z')
        ++definitions;
      else
        ++uses;
    }
    if (d->tied) ops[1] = ops[0];
    auto matches = [&] (std::span<const MachineOperand> operands) {
      return d->architecture == target::Architecture::AArch64
               ? aarch64_instruction_ok (opcode, operands)
               : x86_instruction_ok (opcode, operands);
    };
    CHECK (matches (ops));
    if (d->tied) {
      auto bad = ops;
      auto other = mirnext::codegen::detail::access (f).create_register (
        Type::vector128 (Type::VectorElement::U8, 16));
      REQUIRE (other.has_value ());
      bad[1] = MachineOperand (other.value ());
      CHECK_FALSE (matches (bad));
    }
    for (unsigned n = 0; n < d->roles.size (); ++n) {
      if (d->roles[n] == 'n') continue;
      auto wrong_type = d->roles[n] == 'i' || d->roles[n] == 'I'
                          ? Type::vector128 (Type::VectorElement::U8, 16)
                          : Type::i64 ();
      auto wrong = mirnext::codegen::detail::access (f).create_register (wrong_type);
      REQUIRE (wrong.has_value ());
      auto bad = ops;
      bad[n] = MachineOperand (wrong.value ());
      if (d->tied && n < 2) bad[0] = bad[1] = bad[n];
      CHECK_FALSE (matches (bad));
    }
    if (d->architecture == target.value ().architecture ()) {
      for (auto &physical : target.value ().registers ()) {
        for (unsigned n = 0; n < d->roles.size (); ++n) {
          if (d->roles[n] == 'n') continue;
          auto bad = ops;
          bad[n] = mirnext::codegen::detail::NodeAccess::make_MachineOperand (physical.reg);
          if (d->tied && n < 2) bad[0] = bad[1] = bad[n];
          bool integer = d->roles[n] == 'i' || d->roles[n] == 'I';
          bool acceptable
            = physical.bank
              == (integer ? target::RegisterBank::Integer : target::RegisterBank::Floating);
          if (integer && d->architecture == target::Architecture::AArch64
              && physical.reg.encoding () == 31)
            acceptable = false;
          CHECK (matches (bad) == acceptable);
        }
      }
    }
    auto missing = ops;
    missing.pop_back ();
    CHECK_FALSE (matches (missing));
    if (d->roles.back () == 'n') {
      auto bad = ops;
      bad.back () = MachineOperand (std::uint64_t (d->maximum + 1));
      CHECK_FALSE (matches (bad));
      bad.back () = MachineOperand (std::int64_t (-1));
      CHECK_FALSE (matches (bad));
    }
    if (d->architecture != target.value ().architecture ()) {
      CHECK_FALSE (rw.append (block, opcode, ops).has_value ());
      continue;
    }
    auto instruction = rw.append (block, opcode, ops);
    REQUIRE (instruction.has_value ());
    auto effects = machine_register_effects (f, instruction.value ());
    REQUIRE (effects.has_value ());
    CHECK (effects.value ().definitions.size () == definitions);
    CHECK (effects.value ().uses.size () == uses);
    if (d->architecture == target::Architecture::AArch64) {
      auto recipe = aarch64_encoding_recipe (opcode, ops);
      REQUIRE (recipe);
      REQUIRE (recipe->words.size () == 1);
      CHECK (recipe->words.front ().opcode != 0);
    } else
      CHECK (x86_instruction_pattern (opcode, ops).has_value ());
  }
}
TEST_CASE ("vector preparation reuses constants and creates no synthetic stack storage") {
  ir::Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("test");
  auto type = Type::vector128 (Type::VectorElement::U32, 4);
  auto f = b.create_function (module, "constant", {{type}, {type}});
  std::array<std::byte, 16> bytes{};
  for (unsigned n = 0; n < bytes.size (); ++n) bytes[n] = std::byte (n * 17 + 3);
  auto literal = b.constant_vector (type, bytes);
  auto value = b.create_add (b.argument (0), literal);
  value = b.create_sub (value, literal);
  b.create_return (b.create_xor (value, literal));
  REQUIRE (b.finish ().has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE (target.has_value ());
  auto machine = prepare_machine_ir (*f, target.value ());
  INFO ((machine.has_value () ? "" : machine.error ().message));
  REQUIRE (machine.has_value ());
  for (auto &reg : machine.value ().registers ()) CHECK_FALSE (reg.is_memory_backed ());
  unsigned constants = 0;
  for (auto &block : machine.value ().blocks ())
    for (auto &instruction : block.instructions ()) {
      CHECK (instruction.opcode () != MachineOpcode (Opcode::Addr));
      CHECK (instruction.opcode () != MachineOpcode (Opcode::Alloca));
      if (instruction.opcode () == MachineOpcode (MachinePseudoOpcode::A64VectorDupGP)
          || instruction.opcode () == MachineOpcode (MachinePseudoOpcode::X86Unpack64))
        ++constants;
    }
  CHECK (constants == 1);
}
