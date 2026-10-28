/* Licensed under the MIT License. Typed variable fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class VariableExample {
  Initialized,
  Zero,
  Constant,
  Boolean,
  Floating,
  Pointer,
  ExpressionInitializer,
  CrossModule,
  Narrow
};
inline std::unique_ptr<ir::Program> variable_example (VariableExample mode) {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto consumer = mirnext::ir::detail::access (*(context)).create_module ("consumer");
  REQUIRE ((consumer).has_value ());
  auto module = &(consumer).value ();
  if (mode == VariableExample::CrossModule) {
    auto definition = mirnext::ir::detail::access (*(context)).create_module ("definition");
    REQUIRE ((definition).has_value ());
    module = &(definition).value ();
  }
  auto type = mode == VariableExample::Boolean    ? Type (Type::Kind::Bool)
              : mode == VariableExample::Floating ? Type (Type::Kind::F64)
              : mode == VariableExample::Pointer  ? Type::pointer ()
              : mode == VariableExample::Narrow   ? Type (Type::Kind::I8)
                                                  : Type::i64 ();
  const GlobalVariable *variable = nullptr;
  if (mode == VariableExample::ExpressionInitializer) {
    auto function = mirnext::ir::detail::access (*(module)).create_function ("expression",
                                                                             {{Type::i64 ()}, {}});
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    InstructionBuilder builder ((block).value ());
    auto value = builder.binary (Opcode::Add, 20, 22);
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
    auto created
      = mirnext::ir::detail::access (*(module)).create_variable ("value", (function).value ());
    REQUIRE ((created).has_value ());
    variable = &(created).value ();
  } else if (mode == VariableExample::Pointer) {
    auto target
      = mirnext::ir::detail::access (*(module)).create_variable ("target", Type::i64 (), 42);
    REQUIRE ((target).has_value ());
    auto created = mirnext::ir::detail::access (*(module))
                     .create_variable ("value", type, Operand (target.value ().symbol ()));
    REQUIRE ((created).has_value ());
    variable = &(created).value ();
  } else if (mode == VariableExample::Zero) {
    auto created = mirnext::ir::detail::access (*(module)).create_variable ("value", type);
    REQUIRE ((created).has_value ());
    variable = &(created).value ();
  } else {
    Operand initial = mode == VariableExample::Floating ? Operand (20.5)
                      : mode == VariableExample::Boolean
                        ? Operand (-7)
                        : Operand (mode == VariableExample::Constant ? 42
                                   : mode == VariableExample::Narrow ? -1
                                                                     : 20);
    auto created
      = mirnext::ir::detail::access (*(module)).create_variable ("value", type, initial,
                                                                 mode != VariableExample::Constant);
    REQUIRE ((created).has_value ());
    variable = &(created).value ();
  }
  const Symbol *getter = nullptr;
  if (mode == VariableExample::CrossModule) {
    REQUIRE ((mirnext::ir::detail::access ((((*(module))))).export_symbol (variable->symbol ()))
               .has_value ());
    auto function
      = mirnext::ir::detail::access (*(module)).create_function ("get_value", {{Type::i64 ()}, {}});
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    InstructionBuilder builder ((block).value ());
    auto value = builder.load (*variable);
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
    REQUIRE (
      (mirnext::ir::detail::access ((((*(module))))).export_symbol (function.value ().symbol ()))
        .has_value ());
    auto imported
      = mirnext::ir::detail::access (((consumer)).value ()).import_variable ("value", type);
    REQUIRE ((imported).has_value ());
    variable = &(imported).value ();
    auto imported_getter = mirnext::ir::detail::access (((consumer)).value ())
                             .import ("get_value", function.value ().signature ());
    REQUIRE ((imported_getter).has_value ());
    getter = &(imported_getter).value ();
  }
  auto function = mirnext::ir::detail::access (((consumer)).value ())
                    .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  const Register *answer = nullptr;
  auto initial = builder.load (*variable);
  REQUIRE ((initial).has_value ());
  if (mode == VariableExample::Boolean) {
    REQUIRE ((builder.store (*variable, 0)).has_value ());
    auto zero = builder.load (*variable);
    REQUIRE ((zero).has_value ());
    REQUIRE ((builder.store (*variable, -9)).has_value ());
    auto one = builder.load (*variable);
    REQUIRE ((one).has_value ());
    auto positive
      = builder.binary (Opcode::Mul, Operand ((initial).value ()), Operand ((one).value ()));
    REQUIRE ((positive).has_value ());
    auto negative = builder.binary (Opcode::Eq, Operand ((zero).value ()), 0);
    REQUIRE ((negative).has_value ());
    auto valid
      = builder.binary (Opcode::Mul, Operand ((positive).value ()), Operand ((negative).value ()));
    REQUIRE ((valid).has_value ());
    auto result = builder.binary (Opcode::Mul, Operand ((valid).value ()), 42);
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  } else if (mode == VariableExample::Narrow) {
    auto byte
      = mirnext::ir::detail::access (*(module)).create_variable ("byte", Type (Type::Kind::U8),
                                                                 255);
    REQUIRE ((byte).has_value ());
    auto unsigned_value = builder.load ((byte).value ());
    REQUIRE ((unsigned_value).has_value ());
    REQUIRE ((builder.store (*variable, -3)).has_value ());
    auto signed_value = builder.load (*variable);
    REQUIRE ((signed_value).has_value ());
    auto a = builder.binary (Opcode::Add, Operand ((initial).value ()),
                             Operand ((unsigned_value).value ()));
    REQUIRE ((a).has_value ());
    auto b
      = builder.binary (Opcode::Add, Operand ((a).value ()), Operand ((signed_value).value ()));
    REQUIRE ((b).has_value ());
    auto result = builder.binary (Opcode::Sub, Operand ((b).value ()), 209);
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  } else if (mode == VariableExample::Floating) {
    auto value = builder.binary (Opcode::DAdd, Operand ((initial).value ()), 21.5);
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.store (*variable, Operand ((value).value ()))).has_value ());
    auto stored = builder.load (*variable);
    REQUIRE ((stored).has_value ());
    auto result = builder.unary (Opcode::D2I, Operand ((stored).value ()));
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  } else if (mode == VariableExample::Pointer) {
    auto result = builder.load (
      mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(initial).value ()));
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  } else if (mode == VariableExample::Constant || mode == VariableExample::ExpressionInitializer)
    answer = &(initial).value ();
  else if (mode == VariableExample::CrossModule) {
    REQUIRE ((builder.store (*variable, 42)).has_value ());
    auto call = builder.call (*getter);
    REQUIRE ((call).has_value ());
    auto value = call.value ().value ();
    REQUIRE ((value).has_value ());
    answer = &(value).value ();
  } else {
    auto value = builder.binary (Opcode::Add, Operand ((initial).value ()),
                                 mode == VariableExample::Zero ? 42 : 22);
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.store (*variable, Operand ((value).value ()))).has_value ());
    auto result = builder.load (*variable);
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  }
  REQUIRE ((builder.ret (Operand (*answer))).has_value ());
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
}  // namespace mirnext::test
