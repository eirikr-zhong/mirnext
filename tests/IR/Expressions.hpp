/* Licensed under the MIT License. mirnext::ir::detail::ExpressionState DSL fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class ExpressionExample {
  Arithmetic,
  Unsigned,
  Narrow,
  Floating,
  NaN,
  Branch,
  Conversion,
  Memory,
  Call,
  Boolean,
  Initializer
};
inline std::unique_ptr<ir::Program> expression_example (ExpressionExample mode) {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("expressions");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  const auto i64 = Type::i64 ();
  const auto u64 = Type (Type::Kind::U64);
  const auto boolean = Type (Type::Kind::Bool);
  if (mode == ExpressionExample::Arithmetic)
    REQUIRE ((builder.ret ((builder.literal (20) + 1) * 2)).has_value ());
  else if (mode == ExpressionExample::Unsigned) {
    auto value = builder.literal (u64, UINT64_MAX);
    auto high = builder.literal (u64, UINT64_C (1) << 63);
    auto checked
      = ((value / high) == 1) & ((value >> 63) == 1) & ((value % high) == (UINT64_MAX >> 1));
    REQUIRE ((builder.ret (checked.convert (i64) * 42)).has_value ());
  } else if (mode == ExpressionExample::Narrow) {
    auto signed_value = builder.literal (Type (Type::Kind::I8), 255).convert (i64);
    auto unsigned_value
      = builder.literal (Type (Type::Kind::U8), 255).convert (Type (Type::Kind::F64)).convert (i64);
    REQUIRE ((builder.ret (signed_value + unsigned_value - 212)).has_value ());
  } else if (mode == ExpressionExample::Floating) {
    auto value = -(-(builder.literal (20.5f) + 21.5f));
    REQUIRE ((builder.ret (value.convert (Type (Type::Kind::F64))
                             .convert (Type (Type::Kind::LongDouble))
                             .convert (i64)))
               .has_value ());
  } else if (mode == ExpressionExample::NaN) {
    auto zero = builder.literal (0.0);
    auto nan = zero / zero;
    auto checked = (nan != nan) & !(nan == nan) & !(nan < nan) & nan.convert (boolean);
    REQUIRE ((builder.ret (checked.convert (i64) * 42)).has_value ());
  } else if (mode == ExpressionExample::Branch) {
    auto failure = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((failure).has_value ());
    auto success = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((success).has_value ());
    auto target
      = mirnext::ir::detail::access (((function)).value ()).create_label ((success).value ());
    REQUIRE ((target).has_value ());
    REQUIRE (
      (builder.branch (Comparison::Less, builder.literal (Type (Type::Kind::U32), 1),
                       builder.literal (Type (Type::Kind::U32), UINT32_MAX), (target).value ()))
        .has_value ());
    REQUIRE ((builder.set_insert_point ((failure).value ())).has_value ());
    REQUIRE ((builder.ret (builder.literal (0))).has_value ());
    REQUIRE ((builder.set_insert_point ((success).value ())).has_value ());
    REQUIRE ((builder.ret (builder.literal (42))).has_value ());
  } else if (mode == ExpressionExample::Conversion) {
    auto small = builder.literal (511).convert (Type (Type::Kind::I8));
    auto widened = small.convert (Type (Type::Kind::U16));
    auto truncated = builder.literal (42.75).convert (Type (Type::Kind::I16));
    auto checked = (small == -1) & (widened == 65535) & (truncated == 42);
    REQUIRE ((builder.ret (checked.convert (i64) * 42)).has_value ());
  } else if (mode == ExpressionExample::Memory) {
    auto global
      = mirnext::ir::detail::access (((module)).value ()).create_variable ("value", i64, 40);
    REQUIRE ((global).has_value ());
    REQUIRE (
      (builder.store ((global).value (), builder.expression ((global).value ()) + 2)).has_value ());
    auto address = builder.address_of ((global).value ());
    REQUIRE ((address).has_value ());
    auto local = mirnext::ir::detail::access (((function)).value ()).create_register (i64);
    REQUIRE ((local).has_value ());
    REQUIRE (
      (builder.assign ((local).value (),
                       builder.expression (
                         mirnext::ir::detail::NodeAccess::make_Memory (i64, &(address).value ()))))
        .has_value ());
    REQUIRE ((builder.ret (builder.expression ((local).value ()))).has_value ());
  } else if (mode == ExpressionExample::Call) {
    auto callee = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("identity", {{i64}, {i64}});
    REQUIRE ((callee).has_value ());
    auto callee_block = mirnext::ir::detail::access (((callee)).value ()).create_block ();
    REQUIRE ((callee_block).has_value ());
    InstructionBuilder target ((callee_block).value ());
    REQUIRE ((target.ret (target.expression (
                *mirnext::ir::detail::ReferenceAccess::pointers (callee.value ().arguments ())[0])))
               .has_value ());
    auto call = builder.call ((callee).value (), {builder.literal (20) + 22});
    REQUIRE ((call).has_value ());
    auto result = call.value ().value ();
    REQUIRE ((result).has_value ());
    REQUIRE ((builder.ret (builder.expression ((result).value ()))).has_value ());
  } else if (mode == ExpressionExample::Boolean) {
    auto variable
      = mirnext::ir::detail::access (((module)).value ()).create_variable ("flag", boolean, 0);
    REQUIRE ((variable).has_value ());
    auto address = builder.address_of ((variable).value ());
    REQUIRE ((address).has_value ());
    REQUIRE (
      (builder.store (mirnext::ir::detail::NodeAccess::make_Memory (boolean, &(address).value ()),
                      42))
        .has_value ());
    auto value = builder.expression (
      mirnext::ir::detail::NodeAccess::make_Memory (boolean, &(address).value ()));
    REQUIRE ((builder.ret (value.convert (i64) * 42)).has_value ());
  } else {
    auto initializer = mirnext::ir::detail::access (((module)).value ())
                         .create_function ("initialize", {{i64}, {}});
    REQUIRE ((initializer).has_value ());
    auto initializer_block = mirnext::ir::detail::access (((initializer)).value ()).create_block ();
    REQUIRE ((initializer_block).has_value ());
    InstructionBuilder target ((initializer_block).value ());
    REQUIRE ((target.ret (target.literal (6) * 7)).has_value ());
    auto global = mirnext::ir::detail::access (((module)).value ())
                    .create_variable ("value", (initializer).value ());
    REQUIRE ((global).has_value ());
    REQUIRE ((builder.ret (builder.expression ((global).value ()))).has_value ());
  }
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
}  // namespace mirnext::test
