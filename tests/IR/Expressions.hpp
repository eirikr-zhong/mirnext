/* Licensed under the MIT License. Expression DSL fixtures. */
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
inline std::unique_ptr<ir::Context> expression_example (ExpressionExample mode) {
  using namespace ir;
  auto context = std::make_unique<Context> ();
  auto module = context->create_module ("expressions");
  REQUIRE (module);
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  const auto i64 = Type::i64 ();
  const auto u64 = Type (Type::Kind::U64);
  const auto boolean = Type (Type::Kind::Bool);
  if (mode == ExpressionExample::Arithmetic)
    REQUIRE (builder.ret ((builder.literal (20) + 1) * 2));
  else if (mode == ExpressionExample::Unsigned) {
    auto value = builder.literal (u64, UINT64_MAX);
    auto high = builder.literal (u64, UINT64_C (1) << 63);
    auto checked
      = ((value / high) == 1) & ((value >> 63) == 1) & ((value % high) == (UINT64_MAX >> 1));
    REQUIRE (builder.ret (checked.convert (i64) * 42));
  } else if (mode == ExpressionExample::Narrow) {
    auto signed_value = builder.literal (Type (Type::Kind::I8), 255).convert (i64);
    auto unsigned_value
      = builder.literal (Type (Type::Kind::U8), 255).convert (Type (Type::Kind::F64)).convert (i64);
    REQUIRE (builder.ret (signed_value + unsigned_value - 212));
  } else if (mode == ExpressionExample::Floating) {
    auto value = -(-(builder.literal (20.5f) + 21.5f));
    REQUIRE (builder.ret (value.convert (Type (Type::Kind::F64))
                            .convert (Type (Type::Kind::LongDouble))
                            .convert (i64)));
  } else if (mode == ExpressionExample::NaN) {
    auto zero = builder.literal (0.0);
    auto nan = zero / zero;
    auto checked = (nan != nan) & !(nan == nan) & !(nan < nan) & nan.convert (boolean);
    REQUIRE (builder.ret (checked.convert (i64) * 42));
  } else if (mode == ExpressionExample::Branch) {
    auto failure = (*function)->create_block ();
    REQUIRE (failure);
    auto success = (*function)->create_block ();
    REQUIRE (success);
    auto target = (*function)->create_label (**success);
    REQUIRE (target);
    REQUIRE (builder.branch (Comparison::Less, builder.literal (Type (Type::Kind::U32), 1),
                             builder.literal (Type (Type::Kind::U32), UINT32_MAX), **target));
    REQUIRE (builder.set_insert_point (**failure));
    REQUIRE (builder.ret (builder.literal (0)));
    REQUIRE (builder.set_insert_point (**success));
    REQUIRE (builder.ret (builder.literal (42)));
  } else if (mode == ExpressionExample::Conversion) {
    auto small = builder.literal (511).convert (Type (Type::Kind::I8));
    auto widened = small.convert (Type (Type::Kind::U16));
    auto truncated = builder.literal (42.75).convert (Type (Type::Kind::I16));
    auto checked = (small == -1) & (widened == 65535) & (truncated == 42);
    REQUIRE (builder.ret (checked.convert (i64) * 42));
  } else if (mode == ExpressionExample::Memory) {
    auto global = (*module)->create_variable ("value", i64, 40);
    REQUIRE (global);
    REQUIRE (builder.store (**global, builder.expression (**global) + 2));
    auto address = builder.address_of (**global);
    REQUIRE (address);
    auto local = (*function)->create_register (i64);
    REQUIRE (local);
    REQUIRE (builder.assign (**local, builder.expression (Memory{i64, *address})));
    REQUIRE (builder.ret (builder.expression (**local)));
  } else if (mode == ExpressionExample::Call) {
    auto callee = (*module)->create_function ("identity", {{i64}, {i64}});
    REQUIRE (callee);
    auto callee_block = (*callee)->create_block ();
    REQUIRE (callee_block);
    IRBuilder target (**callee_block);
    REQUIRE (target.ret (target.expression (*(*callee)->arguments ()[0])));
    auto call = builder.call (**callee, {builder.literal (20) + 22});
    REQUIRE (call);
    auto result = call->value ();
    REQUIRE (result);
    REQUIRE (builder.ret (builder.expression (**result)));
  } else if (mode == ExpressionExample::Boolean) {
    auto variable = (*module)->create_variable ("flag", boolean, 0);
    REQUIRE (variable);
    auto address = builder.address_of (**variable);
    REQUIRE (address);
    REQUIRE (builder.store (Memory{boolean, *address}, 42));
    auto value = builder.expression (Memory{boolean, *address});
    REQUIRE (builder.ret (value.convert (i64) * 42));
  } else {
    auto initializer = (*module)->create_function ("initialize", {{i64}, {}});
    REQUIRE (initializer);
    auto initializer_block = (*initializer)->create_block ();
    REQUIRE (initializer_block);
    IRBuilder target (**initializer_block);
    REQUIRE (target.ret (target.literal (6) * 7));
    auto global = (*module)->create_variable ("value", **initializer);
    REQUIRE (global);
    REQUIRE (builder.ret (builder.expression (**global)));
  }
  REQUIRE (context->verify ());
  return context;
}
}  // namespace mirnext::test
