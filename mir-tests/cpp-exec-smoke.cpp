/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#include <cstdlib>
#include <cstdint>

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) std::exit(code);
  return result.value();
}

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

int main() {
  if (int code = check_loop_interp()) return code;
  if (int code = check_loop_gen()) return code;
  if (int code = check_sieve_interp()) return code;
  if (int code = check_sieve_gen()) return code;
  return 0;
}
