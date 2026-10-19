/* Licensed under the MIT License. Shared native coverage fixtures. */
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"

namespace mirnext::test {
inline std::unique_ptr<ir::Context> public_instruction_example(const ir::InstructionDescriptor &descriptor) {
  using namespace ir;
  auto integer = [](std::int64_t value) { return Operand(value); };
  auto context = std::make_unique<Context>(); auto module = context->create_module("coverage"); REQUIRE(module);
  Signature signature;
  if (descriptor.opcode != Opcode::JRet) signature.results.push_back(Type::i64());
  signature.variadic = descriptor.opcode == Opcode::VaStart;
  if (signature.variadic) signature.parameters.push_back(Type::i64());
  auto function = (*module)->create_function("entry", signature); REQUIRE(function);
  auto entry = (*function)->create_block("entry"); REQUIRE(entry);
  auto exit = (*function)->create_block("exit"); REQUIRE(exit);
  auto label = (*function)->create_label(**exit, "exit"); REQUIRE(label);
  IRBuilder builder(**entry); std::vector<Operand> operands;
  if (descriptor.opcode == Opcode::Bo || descriptor.opcode == Opcode::Bno
      || descriptor.opcode == Opcode::UBo || descriptor.opcode == Opcode::UBno) {
    auto reg = (*function)->create_register(Type::i64()); REQUIRE(reg);
    REQUIRE(builder.emit(Opcode::Addo, {Operand(**reg), integer(1), integer(1)}));
  }
  if (descriptor.opcode == Opcode::Label) {
    auto anchor = (*function)->create_label(**entry, "entry"); REQUIRE(anchor);
    REQUIRE(builder.ret(integer(42)));
  } else if (descriptor.count == OperandCount::Call) {
    Signature call_signature;
    if (descriptor.opcode != Opcode::JCall) { call_signature.results.push_back(Type::i64()); call_signature.parameters.push_back(Type::i64()); }
    auto prototype = (*module)->prototype("call_signature", call_signature); REQUIRE(prototype);
    auto imported = (*module)->import("callee", call_signature); REQUIRE(imported);
    operands.emplace_back(**prototype); operands.emplace_back(**imported);
    if (descriptor.opcode != Opcode::JCall) {
      auto reg = (*function)->create_register(Type::i64()); REQUIRE(reg); operands.emplace_back(**reg); operands.push_back(integer(42));
    }
  } else if (descriptor.count == OperandCount::Return) operands.push_back(integer(42));
  else if (descriptor.count == OperandCount::Switch) { operands.push_back(integer(0)); operands.emplace_back(**label); }
  else for (std::size_t n = 0; n < descriptor.roles.size(); ++n) {
    auto role = descriptor.roles[n];
    if (role == 'l') operands.emplace_back(**label);
    else if (role == 'I' || role == 'F' || role == 'D' || role == 'E' || role == 'r'
        || (role == 'a' && descriptor.opcode == Opcode::PrSet)) {
      auto type = role == 'F' ? Type(Type::Kind::F32) : role == 'D' ? Type(Type::Kind::F64)
        : role == 'E' ? Type(Type::Kind::LongDouble) : Type::i64();
      auto reg = (*function)->create_register(type); REQUIRE(reg); operands.emplace_back(**reg);
    } else if (role == 'f') operands.emplace_back(1.0f);
    else if (role == 'd') operands.emplace_back(1.0);
    else if (role == 'e') operands.emplace_back(1.0L);
    else if (descriptor.opcode == Opcode::VaArg && n == 2) operands.emplace_back(Memory{Type::i64()});
    else if ((descriptor.opcode == Opcode::PrBeq || descriptor.opcode == Opcode::PrBne) && n == 1) {
      auto reg = (*function)->create_register(Type::i64()); REQUIRE(reg); operands.emplace_back(**reg);
    } else operands.push_back(integer(descriptor.opcode == Opcode::VaBlockArg && n == 3 ? 0 : 16));
  }
  if (descriptor.opcode != Opcode::Label) {
    auto instruction = builder.emit(descriptor.opcode, operands);
    INFO((instruction ? "" : instruction.error().message)); REQUIRE(instruction);
    CHECK((*instruction)->opcode() == descriptor.opcode);
    if (descriptor.flow == ControlFlow::Next) REQUIRE(builder.ret(integer(42)));
  }
  IRBuilder exit_builder(**exit);
  if (descriptor.opcode == Opcode::JRet) REQUIRE(exit_builder.emit(Opcode::JRet, {integer(0)}));
  else if (signature.results.empty()) REQUIRE(exit_builder.ret()); else REQUIRE(exit_builder.ret(integer(42)));
  auto valid = context->verify(); INFO((valid ? "" : valid.error().message)); REQUIRE(valid);
  return context;
}
} // namespace mirnext::test
