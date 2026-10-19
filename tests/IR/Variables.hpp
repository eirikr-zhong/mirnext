/* Licensed under the MIT License. Native variable fixtures shared with the independent oracle. */
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class VariableExample { Initialized, Zero, Constant, Boolean, Floating, Pointer, Expression, CrossModule, Narrow };
inline std::unique_ptr<ir::Context> variable_example(VariableExample mode) {
  using namespace ir;
  auto context = std::make_unique<Context>(); auto consumer = context->create_module("consumer"); REQUIRE(consumer);
  auto module = *consumer;
  if (mode == VariableExample::CrossModule) { auto definition = context->create_module("definition"); REQUIRE(definition); module = *definition; }
  auto type = mode == VariableExample::Boolean ? Type(Type::Kind::Bool) : mode == VariableExample::Floating ? Type(Type::Kind::F64)
    : mode == VariableExample::Pointer ? Type::pointer() : mode == VariableExample::Narrow ? Type(Type::Kind::I8) : Type::i64();
  const GlobalVariable *variable = nullptr;
  if (mode == VariableExample::Expression) {
    auto function = module->create_function("expression", {{Type::i64()}, {}}); REQUIRE(function);
    auto block = (*function)->create_block(); REQUIRE(block); IRBuilder builder(**block);
    auto value = builder.binary(Opcode::Add, 20, 22); REQUIRE(value); REQUIRE(builder.ret(Operand(**value)));
    auto created = module->create_variable("value", **function); REQUIRE(created); variable = *created;
  } else if (mode == VariableExample::Pointer) {
    auto target = module->create_variable("target", Type::i64(), 42); REQUIRE(target);
    auto created = module->create_variable("value", type, Operand((*target)->symbol())); REQUIRE(created); variable = *created;
  } else if (mode == VariableExample::Zero) {
    auto created = module->create_variable("value", type); REQUIRE(created); variable = *created;
  } else {
    Operand initial = mode == VariableExample::Floating ? Operand(20.5) : mode == VariableExample::Boolean ? Operand(-7)
      : Operand(mode == VariableExample::Constant ? 42 : mode == VariableExample::Narrow ? -1 : 20);
    auto created = module->create_variable("value", type, initial, mode != VariableExample::Constant); REQUIRE(created); variable = *created;
  }
  const Symbol *getter = nullptr;
  if (mode == VariableExample::CrossModule) {
    REQUIRE(module->export_symbol(variable->symbol()));
    auto function = module->create_function("get_value", {{Type::i64()}, {}}); REQUIRE(function);
    auto block = (*function)->create_block(); REQUIRE(block); IRBuilder builder(**block);
    auto value = builder.load(*variable); REQUIRE(value); REQUIRE(builder.ret(Operand(**value)));
    REQUIRE(module->export_symbol((*function)->symbol()));
    auto imported = (*consumer)->import_variable("value", type); REQUIRE(imported); variable = *imported;
    auto imported_getter = (*consumer)->import("get_value", (*function)->signature()); REQUIRE(imported_getter); getter = *imported_getter;
  }
  auto function = (*consumer)->create_function("entry", {{Type::i64()}, {}}); REQUIRE(function);
  auto block = (*function)->create_block(); REQUIRE(block); IRBuilder builder(**block);
  const Register *answer = nullptr;
  auto initial = builder.load(*variable); REQUIRE(initial);
  if (mode == VariableExample::Boolean) {
    REQUIRE(builder.store(*variable, 0)); auto zero = builder.load(*variable); REQUIRE(zero);
    REQUIRE(builder.store(*variable, -9)); auto one = builder.load(*variable); REQUIRE(one);
    auto positive = builder.binary(Opcode::Mul, Operand(**initial), Operand(**one)); REQUIRE(positive);
    auto negative = builder.binary(Opcode::Eq, Operand(**zero), 0); REQUIRE(negative);
    auto valid = builder.binary(Opcode::Mul, Operand(**positive), Operand(**negative)); REQUIRE(valid);
    auto result = builder.binary(Opcode::Mul, Operand(**valid), 42); REQUIRE(result); answer = *result;
  } else if (mode == VariableExample::Narrow) {
    auto byte = module->create_variable("byte", Type(Type::Kind::U8), 255); REQUIRE(byte);
    auto unsigned_value = builder.load(**byte); REQUIRE(unsigned_value); REQUIRE(builder.store(*variable, -3));
    auto signed_value = builder.load(*variable); REQUIRE(signed_value);
    auto a = builder.binary(Opcode::Add, Operand(**initial), Operand(**unsigned_value)); REQUIRE(a);
    auto b = builder.binary(Opcode::Add, Operand(**a), Operand(**signed_value)); REQUIRE(b);
    auto result = builder.binary(Opcode::Sub, Operand(**b), 209); REQUIRE(result); answer = *result;
  } else if (mode == VariableExample::Floating) {
    auto value = builder.binary(Opcode::DAdd, Operand(**initial), 21.5); REQUIRE(value); REQUIRE(builder.store(*variable, Operand(**value)));
    auto stored = builder.load(*variable); REQUIRE(stored); auto result = builder.unary(Opcode::D2I, Operand(**stored)); REQUIRE(result); answer = *result;
  } else if (mode == VariableExample::Pointer) {
    auto result = builder.load(Memory{Type::i64(), *initial}); REQUIRE(result); answer = *result;
  } else if (mode == VariableExample::Constant || mode == VariableExample::Expression) answer = *initial;
  else if (mode == VariableExample::CrossModule) {
    REQUIRE(builder.store(*variable, 42)); auto call = builder.call(*getter); REQUIRE(call); auto value = call->value(); REQUIRE(value); answer = *value;
  } else {
    auto value = builder.binary(Opcode::Add, Operand(**initial), mode == VariableExample::Zero ? 42 : 22); REQUIRE(value);
    REQUIRE(builder.store(*variable, Operand(**value))); auto result = builder.load(*variable); REQUIRE(result); answer = *result;
  }
  REQUIRE(builder.ret(Operand(*answer))); REQUIRE(context->verify()); return context;
}
}
