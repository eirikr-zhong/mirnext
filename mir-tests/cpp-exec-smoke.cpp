/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstdint>

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) {
    FAIL("error " << code << ": " << result.error().message);
    std::abort();
  }
  return result.value();
}

static std::int64_t add7(std::int64_t value) { return value + 7; }

static mirnext::Function &create_loop(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 1);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 2);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label fin = expect(builder.create_label(), 3);
  mirnext::Label cont = expect(builder.create_label(), 4);

  expect(builder.create_mov(count, mirnext::Operand::int64(0)), 5);
  expect(builder.create_bge(fin, count, arg1), 6);
  expect(builder.bind(cont), 7);
  expect(builder.create_add(count, count, mirnext::Operand::int64(1)), 8);
  expect(builder.create_blt(cont, count, arg1), 9);
  expect(builder.bind(fin), 10);
  expect(builder.create_ret({count}), 11);
  return function;
}

static mirnext::Function &create_sieve(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_sieve");
  *module_out = &module;
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::Register iter = expect(function.create_register(mirnext::Type::i64(), "iter"), 12);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 13);
  mirnext::Register i = expect(function.create_register(mirnext::Type::i64(), "i"), 14);
  mirnext::Register k = expect(function.create_register(mirnext::Type::i64(), "k"), 15);
  mirnext::Register prime = expect(function.create_register(mirnext::Type::i64(), "prime"), 16);
  mirnext::Register flags = expect(function.create_register(mirnext::Type::i64(), "flags"), 17);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);

  mirnext::Label loop = expect(builder.create_label(), 18);
  mirnext::Label loop2 = expect(builder.create_label(), 19);
  mirnext::Label loop3 = expect(builder.create_label(), 20);
  mirnext::Label loop4 = expect(builder.create_label(), 21);
  mirnext::Label fin = expect(builder.create_label(), 22);
  mirnext::Label fin2 = expect(builder.create_label(), 23);
  mirnext::Label fin3 = expect(builder.create_label(), 24);
  mirnext::Label fin4 = expect(builder.create_label(), 25);
  mirnext::Label cont3 = expect(builder.create_label(), 26);

  expect(builder.create_alloca(flags, mirnext::Operand::int64(8190)), 27);
  expect(builder.create_mov(iter, mirnext::Operand::int64(0)), 28);
  expect(builder.bind(loop), 29);
  expect(builder.create_bge(fin, iter, mirnext::Operand::int64(100)), 30);
  expect(builder.create_mov(count, mirnext::Operand::int64(0)), 31);
  expect(builder.create_mov(i, mirnext::Operand::int64(0)), 32);
  expect(builder.bind(loop2), 33);
  expect(builder.create_bge(fin2, i, mirnext::Operand::int64(8190)), 34);
  expect(builder.create_mov(mirnext::Operand::mem(mirnext::Type::u8(), flags, i),
                            mirnext::Operand::int64(1)),
         35);
  expect(builder.create_add(i, i, mirnext::Operand::int64(1)), 36);
  expect(builder.create_jmp(loop2), 37);
  expect(builder.bind(fin2), 38);
  expect(builder.create_mov(i, mirnext::Operand::int64(1)), 39);
  expect(builder.bind(loop3), 40);
  expect(builder.create_bge(fin3, i, mirnext::Operand::int64(8190)), 41);
  expect(builder.create_beq(cont3, mirnext::Operand::mem(mirnext::Type::u8(), flags, i),
                            mirnext::Operand::int64(0)),
         42);
  expect(builder.create_add(prime, i, mirnext::Operand::int64(1)), 43);
  expect(builder.create_add(k, i, prime), 44);
  expect(builder.bind(loop4), 45);
  expect(builder.create_bge(fin4, k, mirnext::Operand::int64(8190)), 46);
  expect(builder.create_mov(mirnext::Operand::mem(mirnext::Type::u8(), flags, k),
                            mirnext::Operand::int64(0)),
         47);
  expect(builder.create_add(k, k, prime), 48);
  expect(builder.create_jmp(loop4), 49);
  expect(builder.bind(fin4), 50);
  expect(builder.create_add(count, count, mirnext::Operand::int64(1)), 51);
  expect(builder.bind(cont3), 52);
  expect(builder.create_add(i, i, mirnext::Operand::int64(1)), 53);
  expect(builder.create_jmp(loop3), 54);
  expect(builder.bind(fin3), 55);
  expect(builder.create_add(iter, iter, mirnext::Operand::int64(1)), 56);
  expect(builder.create_jmp(loop), 57);
  expect(builder.bind(fin), 58);
  expect(builder.create_ret({count}), 59);
  return function;
}

static mirnext::Function &create_integer_ops(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_integer_ops");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "integer_ops", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 72);
  mirnext::Register a = expect(function.create_register(mirnext::Type::i64(), "a"), 73);
  mirnext::Register b = expect(function.create_register(mirnext::Type::i64(), "b"), 74);
  mirnext::Register c = expect(function.create_register(mirnext::Type::i64(), "c"), 75);
  mirnext::Register cmp = expect(function.create_register(mirnext::Type::i64(), "cmp"), 76);
  mirnext::Register result = expect(function.create_register(mirnext::Type::i64(), "result"), 77);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  expect(builder.create_mov(a, arg1), 78);
  expect(builder.create_sub(a, a, mirnext::Operand::int64(5)), 79);
  expect(builder.create_mul(a, a, mirnext::Operand::int64(3)), 80);
  expect(builder.create_div(a, a, mirnext::Operand::int64(2)), 81);
  expect(builder.create_mod(b, a, mirnext::Operand::int64(6)), 82);
  expect(builder.create_or(c, b, mirnext::Operand::int64(8)), 83);
  expect(builder.create_xor(c, c, mirnext::Operand::int64(3)), 84);
  expect(builder.create_and(c, c, mirnext::Operand::int64(14)), 85);
  expect(builder.create_lsh(c, c, mirnext::Operand::int64(1)), 86);
  expect(builder.create_rsh(c, c, mirnext::Operand::int64(2)), 87);
  expect(builder.create_ursh(c, c, mirnext::Operand::int64(1)), 88);
  expect(builder.create_neg(b, c), 89);
  expect(builder.create_eq(cmp, c, mirnext::Operand::int64(2)), 90);
  expect(builder.create_add(result, b, cmp), 91);
  expect(builder.create_ret({result}), 92);
  return function;
}

static mirnext::Function &create_integer_branch(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_integer_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "integer_branch", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 93);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label signed_gt = expect(builder.create_label(), 94);
  mirnext::Label unsigned_gt = expect(builder.create_label(), 95);
  expect(builder.create_bgt(signed_gt, arg1, mirnext::Operand::int64(10)), 96);
  expect(builder.create_ubgt(unsigned_gt, arg1, mirnext::Operand::int64(10)), 97);
  expect(builder.create_ret({mirnext::Operand::int64(0)}), 98);
  expect(builder.bind(signed_gt), 99);
  expect(builder.create_ret({mirnext::Operand::int64(1)}), 100);
  expect(builder.bind(unsigned_gt), 101);
  expect(builder.create_ret({mirnext::Operand::int64(9)}), 102);
  return function;
}

static mirnext::Function &create_import_call(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_import_call");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "add7_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Import &import = module.new_import("add7");
  mirnext::Function &function = module.new_function(
      "import_call", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 115);
  mirnext::Register result = expect(function.create_register(mirnext::Type::i64(), "result"), 116);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  expect(builder.create_call(prototype, mirnext::Operand::ref(import), {result}, {arg1}), 117);
  expect(builder.create_ret({result}), 118);
  return function;
}

static mirnext::Function &create_internal_call(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_internal_call");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "add11_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Function &callee = module.new_function(
      "add11", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register callee_arg = expect(callee.argument("arg1"), 119);
  mirnext::Register callee_result
      = expect(callee.create_register(mirnext::Type::i64(), "result"), 120);
  mirnext::IRBuilder callee_builder(ctx);
  callee_builder.set_insert_point(callee);
  expect(callee_builder.create_add(callee_result, callee_arg, mirnext::Operand::int64(11)), 121);
  expect(callee_builder.create_ret({callee_result}), 122);

  mirnext::Function &caller = module.new_function(
      "internal_call", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register caller_arg = expect(caller.argument("arg1"), 123);
  mirnext::Register caller_result
      = expect(caller.create_register(mirnext::Type::i64(), "result"), 124);
  mirnext::IRBuilder caller_builder(ctx);
  caller_builder.set_insert_point(caller);
  expect(caller_builder.create_call(prototype, mirnext::Operand::ref(callee), {caller_result},
                                    {caller_arg}),
         125);
  expect(caller_builder.create_ret({caller_result}), 126);
  return caller;
}

static mirnext::Function &create_forward_call(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_forward_call");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "add17_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Function &caller = module.new_function(
      "forward_call", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Function &callee = module.new_function(
      "add17", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Register caller_arg = expect(caller.argument("arg1"), 139);
  mirnext::Register caller_result
      = expect(caller.create_register(mirnext::Type::i64(), "result"), 140);
  mirnext::IRBuilder caller_builder(ctx);
  caller_builder.set_insert_point(caller);
  expect(caller_builder.create_call(prototype, mirnext::Operand::ref(callee), {caller_result},
                                    {caller_arg}),
         141);
  expect(caller_builder.create_ret({caller_result}), 142);

  mirnext::Register callee_arg = expect(callee.argument("arg1"), 143);
  mirnext::Register callee_result
      = expect(callee.create_register(mirnext::Type::i64(), "result"), 144);
  mirnext::IRBuilder callee_builder(ctx);
  callee_builder.set_insert_point(callee);
  expect(callee_builder.create_add(callee_result, callee_arg, mirnext::Operand::int64(17)), 145);
  expect(callee_builder.create_ret({callee_result}), 146);
  return caller;
}

static mirnext::Function &create_self_recursion(mirnext::Context &ctx,
                                                mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_self_recursion");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "countdown_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Function &function = module.new_function(
      "countdown", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 147);
  mirnext::Register next = expect(function.create_register(mirnext::Type::i64(), "next"), 148);
  mirnext::Register recursive
      = expect(function.create_register(mirnext::Type::i64(), "recursive"), 149);
  mirnext::Register result = expect(function.create_register(mirnext::Type::i64(), "result"), 150);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label base = expect(builder.create_label(), 151);
  expect(builder.create_ble(base, arg1, mirnext::Operand::int64(0)), 152);
  expect(builder.create_sub(next, arg1, mirnext::Operand::int64(1)), 153);
  expect(builder.create_call(prototype, mirnext::Operand::ref(function), {recursive}, {next}), 154);
  expect(builder.create_add(result, recursive, mirnext::Operand::int64(1)), 155);
  expect(builder.create_ret({result}), 156);
  expect(builder.bind(base), 157);
  expect(builder.create_ret({mirnext::Operand::int64(0)}), 158);
  return function;
}

static mirnext::Function &create_mutual_recursion(mirnext::Context &ctx,
                                                  mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_mutual_recursion");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "mutual_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Function &first = module.new_function(
      "mutual_first", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Function &second = module.new_function(
      "mutual_second", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Register first_arg = expect(first.argument("arg1"), 159);
  mirnext::Register first_result
      = expect(first.create_register(mirnext::Type::i64(), "result"), 160);
  mirnext::IRBuilder first_builder(ctx);
  first_builder.set_insert_point(first);
  expect(first_builder.create_call(prototype, mirnext::Operand::ref(second), {first_result},
                                   {first_arg}),
         161);
  expect(first_builder.create_ret({first_result}), 162);

  mirnext::Register second_arg = expect(second.argument("arg1"), 163);
  mirnext::Register second_result
      = expect(second.create_register(mirnext::Type::i64(), "result"), 164);
  mirnext::IRBuilder second_builder(ctx);
  second_builder.set_insert_point(second);
  expect(second_builder.create_call(prototype, mirnext::Operand::ref(first), {second_result},
                                    {second_arg}),
         165);
  expect(second_builder.create_ret({second_result}), 166);
  return first;
}

static mirnext::Function &create_double_arithmetic(mirnext::Context &ctx,
                                                   mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_arithmetic");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_arithmetic", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 180);
  mirnext::Register value = expect(function.create_register(mirnext::Type::d(), "value"), 181);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  expect(builder.create_dmov(value, arg1), 182);
  expect(builder.create_dadd(value, value, mirnext::Operand::float64(2.0)), 183);
  expect(builder.create_dmul(value, value, mirnext::Operand::float64(3.0)), 184);
  expect(builder.create_ddiv(value, value, mirnext::Operand::float64(2.0)), 185);
  expect(builder.create_dsub(value, value, mirnext::Operand::float64(1.0)), 186);
  expect(builder.create_dneg(value, value), 187);
  expect(builder.create_ret({value}), 188);
  return function;
}

static mirnext::Function &create_double_branch(mirnext::Context &ctx,
                                               mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_branch", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 189);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label high = expect(builder.create_label(), 190);
  mirnext::Label low = expect(builder.create_label(), 191);
  expect(builder.create_dbgt(high, arg1, mirnext::Operand::float64(10.0)), 192);
  expect(builder.create_dble(low, arg1, mirnext::Operand::float64(5.0)), 193);
  expect(builder.create_ret({mirnext::Operand::float64(0.25)}), 194);
  expect(builder.bind(high), 195);
  expect(builder.create_ret({mirnext::Operand::float64(1.5)}), 196);
  expect(builder.bind(low), 197);
  expect(builder.create_ret({mirnext::Operand::float64(-2.5)}), 198);
  return function;
}

static mirnext::Function &create_int_double_conversion(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_int_double_conversion");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "int_double_conversion", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 199);
  mirnext::Register value = expect(function.create_register(mirnext::Type::d(), "value"), 200);
  mirnext::Register integer = expect(function.create_register(mirnext::Type::i64(), "integer"), 201);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  expect(builder.create_d2i(integer, arg1), 202);
  expect(builder.create_i2d(value, integer), 203);
  expect(builder.create_ret({value}), 206);
  return function;
}

static mirnext::Function &create_float_long_double_lowering(mirnext::Context &ctx,
                                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_float_long_double_lowering");
  *module_out = &module;
  mirnext::Function &function = module.new_function("float_long_double_lowering", {}, {});
  mirnext::Register i = expect(function.create_register(mirnext::Type::i64(), "i"), 207);
  mirnext::Register f = expect(function.create_register(mirnext::Type::f(), "f"), 208);
  mirnext::Register d = expect(function.create_register(mirnext::Type::d(), "d"), 209);
  mirnext::Register ld = expect(function.create_register(mirnext::Type::ld(), "ld"), 210);
  mirnext::Register cmp = expect(function.create_register(mirnext::Type::i64(), "cmp"), 211);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label done = expect(builder.create_label(), 212);
  expect(builder.create_mov(i, mirnext::Operand::int64(42)), 213);
  expect(builder.create_ext8(i, i), 214);
  expect(builder.create_ext16(i, i), 215);
  expect(builder.create_ext32(i, i), 216);
  expect(builder.create_uext8(i, i), 217);
  expect(builder.create_uext16(i, i), 218);
  expect(builder.create_uext32(i, i), 219);
  expect(builder.create_i2f(f, i), 220);
  expect(builder.create_ui2ld(ld, i), 221);
  expect(builder.create_fmov(f, mirnext::Operand::float32(1.25f)), 222);
  expect(builder.create_fneg(f, f), 223);
  expect(builder.create_fadd(f, f, mirnext::Operand::float32(2.0f)), 224);
  expect(builder.create_fsub(f, f, mirnext::Operand::float32(1.0f)), 225);
  expect(builder.create_fmul(f, f, mirnext::Operand::float32(3.0f)), 226);
  expect(builder.create_fdiv(f, f, mirnext::Operand::float32(2.0f)), 227);
  expect(builder.create_feq(cmp, f, mirnext::Operand::float32(0.0f)), 228);
  expect(builder.create_fne(cmp, f, mirnext::Operand::float32(0.0f)), 229);
  expect(builder.create_flt(cmp, f, mirnext::Operand::float32(0.0f)), 230);
  expect(builder.create_fle(cmp, f, mirnext::Operand::float32(0.0f)), 231);
  expect(builder.create_fgt(cmp, f, mirnext::Operand::float32(0.0f)), 232);
  expect(builder.create_fge(cmp, f, mirnext::Operand::float32(0.0f)), 233);
  expect(builder.create_fbeq(done, f, mirnext::Operand::float32(0.0f)), 234);
  expect(builder.create_fblt(done, f, mirnext::Operand::float32(0.0f)), 235);
  expect(builder.create_i2ld(ld, i), 236);
  expect(builder.create_ldmov(ld, mirnext::Operand::long_double(1.5L)), 237);
  expect(builder.create_ldneg(ld, ld), 238);
  expect(builder.create_ldadd(ld, ld, mirnext::Operand::long_double(2.0L)), 239);
  expect(builder.create_ldsub(ld, ld, mirnext::Operand::long_double(1.0L)), 240);
  expect(builder.create_ldmul(ld, ld, mirnext::Operand::long_double(3.0L)), 241);
  expect(builder.create_lddiv(ld, ld, mirnext::Operand::long_double(2.0L)), 242);
  expect(builder.create_ldeq(cmp, ld, mirnext::Operand::long_double(0.0L)), 243);
  expect(builder.create_ldne(cmp, ld, mirnext::Operand::long_double(0.0L)), 244);
  expect(builder.create_ldlt(cmp, ld, mirnext::Operand::long_double(0.0L)), 245);
  expect(builder.create_ldle(cmp, ld, mirnext::Operand::long_double(0.0L)), 246);
  expect(builder.create_ldgt(cmp, ld, mirnext::Operand::long_double(0.0L)), 247);
  expect(builder.create_ldge(cmp, ld, mirnext::Operand::long_double(0.0L)), 248);
  expect(builder.create_ldbeq(done, ld, mirnext::Operand::long_double(0.0L)), 249);
  expect(builder.create_ldbgt(done, ld, mirnext::Operand::long_double(0.0L)), 250);
  expect(builder.create_f2d(d, f), 251);
  expect(builder.create_f2ld(ld, f), 252);
  expect(builder.create_d2f(f, d), 253);
  expect(builder.create_d2ld(ld, d), 254);
  expect(builder.create_ld2f(f, ld), 255);
  expect(builder.create_ld2d(d, ld), 256);
  expect(builder.create_f2i(i, f), 257);
  expect(builder.create_ld2i(i, ld), 258);
  expect(builder.bind(done), 259);
  expect(builder.create_ret(), 260);
  return function;
}

static int check_loop_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_loop(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 60);
  const std::int64_t arg = 10000;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 61);
  return result == arg ? 0 : 62;
}

static int check_loop_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_loop(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 63);
  const std::int64_t arg = 10000;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               64);
  return result == arg ? 0 : 65;
}

static int check_sieve_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_sieve(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 66);
  std::int64_t result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function), 67);
  return result == 1027 ? 0 : 68;
}

static int check_sieve_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_sieve(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 69);
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function), 70);
  return result == 1027 ? 0 : 71;
}

static int check_integer_ops_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_integer_ops(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 103);
  const std::int64_t arg = 42;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 104);
  return result == -1 ? 0 : 105;
}

static int check_integer_ops_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_integer_ops(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 106);
  const std::int64_t arg = 42;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               107);
  return result == -1 ? 0 : 108;
}

static int check_integer_branch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_integer_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 109);
  const std::int64_t arg = -1;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 110);
  return result == 9 ? 0 : 111;
}

static int check_integer_branch_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_integer_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 112);
  const std::int64_t arg = -1;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               113);
  return result == 9 ? 0 : 114;
}

static int check_import_call_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_import_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 127);
  MIR_load_external(legacy.raw(), "add7", reinterpret_cast<void *>(add7));
  const std::int64_t arg = 35;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 128);
  return result == 42 ? 0 : 129;
}

static int check_import_call_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_import_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 130);
  MIR_load_external(legacy.raw(), "add7", reinterpret_cast<void *>(add7));
  const std::int64_t arg = 35;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               131);
  return result == 42 ? 0 : 132;
}

static int check_internal_call_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_internal_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 133);
  const std::int64_t arg = 31;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 134);
  return result == 42 ? 0 : 135;
}

static int check_internal_call_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_internal_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 136);
  const std::int64_t arg = 31;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               137);
  return result == 42 ? 0 : 138;
}

static int check_forward_call_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_forward_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 167);
  const std::int64_t arg = 25;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 168);
  return result == 42 ? 0 : 169;
}

static int check_forward_call_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_forward_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 170);
  const std::int64_t arg = 25;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               171);
  return result == 42 ? 0 : 172;
}

static int check_self_recursion_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_self_recursion(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 173);
  const std::int64_t arg = 4;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 174);
  return result == 4 ? 0 : 175;
}

static int check_self_recursion_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_self_recursion(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 176);
  const std::int64_t arg = 4;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               177);
  return result == 4 ? 0 : 178;
}

static int check_mutual_recursion_lowering() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_mutual_recursion(ctx, &module);
  mirnext::LegacyContext legacy;
  expect(mirnext::lower_to_legacy(legacy, *module, function), 179);
  return 0;
}

static int check_double_arithmetic_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_arithmetic(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 261);
  const double arg = 8.0;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         262);
  return result == doctest::Approx(-14.0) ? 0 : 263;
}

static int check_double_arithmetic_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_arithmetic(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 264);
  const double arg = 8.0;
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function, &arg, 1),
               265);
  return result == doctest::Approx(-14.0) ? 0 : 266;
}

static int check_double_branch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 267);
  const double arg = 12.0;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         268);
  return result == doctest::Approx(1.5) ? 0 : 269;
}

static int check_double_branch_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 270);
  const double arg = 4.0;
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function, &arg, 1),
               271);
  return result == doctest::Approx(-2.5) ? 0 : 272;
}

static int check_int_double_conversion_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_int_double_conversion(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 273);
  const double arg = 41.75;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         274);
  return result == doctest::Approx(41.0) ? 0 : 275;
}

static int check_int_double_conversion_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_int_double_conversion(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 276);
  const double arg = 41.75;
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function, &arg, 1),
               277);
  return result == doctest::Approx(41.0) ? 0 : 278;
}

static int check_float_long_double_lowering() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_float_long_double_lowering(ctx, &module);
  mirnext::LegacyContext legacy;
  expect(mirnext::lower_to_legacy(legacy, *module, function), 279);
  return 0;
}

TEST_CASE("legacy lowering executes loop") {
  CHECK(check_loop_interp() == 0);
  CHECK(check_loop_gen() == 0);
}

TEST_CASE("legacy lowering executes sieve") {
  CHECK(check_sieve_interp() == 0);
  CHECK(check_sieve_gen() == 0);
}

TEST_CASE("legacy lowering executes integer operations and branches") {
  CHECK(check_integer_ops_interp() == 0);
  CHECK(check_integer_ops_gen() == 0);
  CHECK(check_integer_branch_interp() == 0);
  CHECK(check_integer_branch_gen() == 0);
}

TEST_CASE("legacy lowering executes import and earlier internal calls") {
  CHECK(check_import_call_interp() == 0);
  CHECK(check_import_call_gen() == 0);
  CHECK(check_internal_call_interp() == 0);
  CHECK(check_internal_call_gen() == 0);
}

TEST_CASE("legacy lowering executes forward and recursive calls") {
  CHECK(check_forward_call_interp() == 0);
  CHECK(check_forward_call_gen() == 0);
  CHECK(check_self_recursion_interp() == 0);
  CHECK(check_self_recursion_gen() == 0);
}

TEST_CASE("legacy lowering accepts mutual recursion") {
  CHECK(check_mutual_recursion_lowering() == 0);
}

TEST_CASE("legacy lowering executes double operations") {
  CHECK(check_double_arithmetic_interp() == 0);
  CHECK(check_double_arithmetic_gen() == 0);
  CHECK(check_double_branch_interp() == 0);
  CHECK(check_double_branch_gen() == 0);
  CHECK(check_int_double_conversion_interp() == 0);
  CHECK(check_int_double_conversion_gen() == 0);
}

TEST_CASE("legacy lowering accepts float and long double operations") {
  CHECK(check_float_long_double_lowering() == 0);
}
