/* Licensed under the MIT License. Shared native coverage fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"

namespace mirnext::test {
inline std::unique_ptr<ir::Program> public_instruction_example (
  const ir::InstructionDescriptor &descriptor) {
  using namespace ir;
  auto integer = [] (std::int64_t value) { return Operand (value); };
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("coverage");
  REQUIRE ((module).has_value ());
  Signature signature;
  if (descriptor.opcode != Opcode::JRet) signature.results.push_back (Type::i64 ());
  signature.variadic = descriptor.opcode == Opcode::VaStart;
  if (signature.variadic) signature.parameters.push_back (Type::i64 ());
  auto function
    = mirnext::ir::detail::access (((module)).value ()).create_function ("entry", signature);
  REQUIRE ((function).has_value ());
  auto entry = mirnext::ir::detail::access (((function)).value ()).create_block ("entry");
  REQUIRE ((entry).has_value ());
  auto exit = mirnext::ir::detail::access (((function)).value ()).create_block ("exit");
  REQUIRE ((exit).has_value ());
  auto label
    = mirnext::ir::detail::access (((function)).value ()).create_label ((exit).value (), "exit");
  REQUIRE ((label).has_value ());
  InstructionBuilder builder ((entry).value ());
  std::vector<Operand> operands;
  if (descriptor.opcode == Opcode::Bo || descriptor.opcode == Opcode::Bno
      || descriptor.opcode == Opcode::UBo || descriptor.opcode == Opcode::UBno) {
    auto reg = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
    REQUIRE ((reg).has_value ());
    REQUIRE ((builder.emit (Opcode::Addo, {Operand ((reg).value ()), integer (1), integer (1)}))
               .has_value ());
  }
  if (descriptor.opcode == Opcode::Label) {
    auto anchor = mirnext::ir::detail::access (((function)).value ())
                    .create_label ((entry).value (), "entry");
    REQUIRE ((anchor).has_value ());
    REQUIRE ((builder.ret (integer (42))).has_value ());
  } else if (descriptor.count == OperandCount::Call) {
    Signature call_signature;
    if (descriptor.opcode != Opcode::JCall) {
      call_signature.results.push_back (Type::i64 ());
      call_signature.parameters.push_back (Type::i64 ());
    }
    auto prototype = mirnext::ir::detail::access (((module)).value ())
                       .prototype ("call_signature", call_signature);
    REQUIRE ((prototype).has_value ());
    auto imported
      = mirnext::ir::detail::access (((module)).value ()).import ("callee", call_signature);
    REQUIRE ((imported).has_value ());
    operands.emplace_back ((prototype).value ());
    operands.emplace_back ((imported).value ());
    if (descriptor.opcode != Opcode::JCall) {
      auto reg = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
      REQUIRE ((reg).has_value ());
      operands.emplace_back ((reg).value ());
      operands.push_back (integer (42));
    }
  } else if (descriptor.count == OperandCount::Return)
    operands.push_back (integer (42));
  else if (descriptor.count == OperandCount::Switch) {
    operands.push_back (integer (0));
    operands.emplace_back ((label).value ());
  } else
    for (std::size_t n = 0; n < descriptor.roles.size (); ++n) {
      auto role = descriptor.roles[n];
      if (role == 'l')
        operands.emplace_back ((label).value ());
      else if (role == 'I' || role == 'F' || role == 'D' || role == 'E' || role == 'r'
               || (role == 'a' && descriptor.opcode == Opcode::PrSet)) {
        auto type = role == 'F'   ? Type (Type::Kind::F32)
                    : role == 'D' ? Type (Type::Kind::F64)
                    : role == 'E' ? Type (Type::Kind::LongDouble)
                                  : Type::i64 ();
        auto reg = mirnext::ir::detail::access (((function)).value ()).create_register (type);
        REQUIRE ((reg).has_value ());
        operands.emplace_back ((reg).value ());
      } else if (role == 'f')
        operands.emplace_back (1.0f);
      else if (role == 'd')
        operands.emplace_back (1.0);
      else if (role == 'e')
        operands.emplace_back (1.0L);
      else if (descriptor.opcode == Opcode::VaArg && n == 2)
        operands.emplace_back (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ()));
      else if ((descriptor.opcode == Opcode::PrBeq || descriptor.opcode == Opcode::PrBne)
               && n == 1) {
        auto reg
          = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
        REQUIRE ((reg).has_value ());
        operands.emplace_back ((reg).value ());
      } else
        operands.push_back (integer (descriptor.opcode == Opcode::VaBlockArg && n == 3 ? 0 : 16));
    }
  if (descriptor.opcode != Opcode::Label) {
    auto instruction = builder.emit (descriptor.opcode, operands);
    INFO (((instruction).has_value () ? "" : instruction.error ().message));
    REQUIRE ((instruction).has_value ());
    CHECK (instruction.value ().opcode () == descriptor.opcode);
    if (descriptor.flow == ControlFlow::Next) REQUIRE ((builder.ret (integer (42))).has_value ());
  }
  InstructionBuilder exit_builder ((exit).value ());
  if (descriptor.opcode == Opcode::JRet)
    REQUIRE ((exit_builder.emit (Opcode::JRet, {integer (0)})).has_value ());
  else if (signature.results.empty ())
    REQUIRE ((exit_builder.ret ()).has_value ());
  else
    REQUIRE ((exit_builder.ret (integer (42))).has_value ());
  auto valid = context->verify ();
  INFO (((valid).has_value () ? "" : valid.error ().message));
  REQUIRE ((valid).has_value ());
  return context;
}
}  // namespace mirnext::test
