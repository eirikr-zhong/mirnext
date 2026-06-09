/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdlib>
#include <sstream>
#include <string>

static bool contains(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) std::exit(code);
  return result.value();
}

int main() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m_sieve");
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::Register iter = expect(function.create_register(mirnext::Type::i64(), "iter"), 15);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 16);
  mirnext::Register i = expect(function.create_register(mirnext::Type::i64(), "i"), 17);
  mirnext::Register k = expect(function.create_register(mirnext::Type::i64(), "k"), 18);
  mirnext::Register prime = expect(function.create_register(mirnext::Type::i64(), "prime"), 19);
  mirnext::Register flags = expect(function.create_register(mirnext::Type::i64(), "flags"), 20);

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);

  mirnext::Label loop = expect(builder.create_label(), 21);
  mirnext::Label loop2 = expect(builder.create_label(), 22);
  mirnext::Label loop3 = expect(builder.create_label(), 23);
  mirnext::Label loop4 = expect(builder.create_label(), 24);
  mirnext::Label fin = expect(builder.create_label(), 25);
  mirnext::Label fin2 = expect(builder.create_label(), 26);
  mirnext::Label fin3 = expect(builder.create_label(), 27);
  mirnext::Label fin4 = expect(builder.create_label(), 28);
  mirnext::Label cont3 = expect(builder.create_label(), 29);

  expect(builder.create_alloca(flags, mirnext::Operand::int64(819000)), 30);
  expect(builder.create_mov(iter, mirnext::Operand::int64(0)), 31);
  expect(builder.bind(loop), 32);
  expect(builder.create_bge(fin, iter, mirnext::Operand::int64(100)), 33);
  expect(builder.create_mov(count, mirnext::Operand::int64(0)), 34);
  expect(builder.create_mov(i, mirnext::Operand::int64(0)), 35);
  expect(builder.bind(loop2), 36);
  expect(builder.create_bge(fin2, i, mirnext::Operand::int64(819000)), 37);
  expect(builder.create_mov(mirnext::Operand::mem(mirnext::Type::u8(), flags, i),
                            mirnext::Operand::int64(1)),
         38);
  expect(builder.create_add(i, i, mirnext::Operand::int64(1)), 39);
  expect(builder.create_jmp(loop2), 40);
  expect(builder.bind(fin2), 41);
  expect(builder.create_mov(i, mirnext::Operand::int64(1)), 42);
  expect(builder.bind(loop3), 43);
  expect(builder.create_bge(fin3, i, mirnext::Operand::int64(819000)), 44);
  expect(builder.create_beq(cont3, mirnext::Operand::mem(mirnext::Type::u8(), flags, i),
                            mirnext::Operand::int64(0)),
         45);
  expect(builder.create_add(prime, i, mirnext::Operand::int64(1)), 46);
  expect(builder.create_add(k, i, prime), 47);
  expect(builder.bind(loop4), 48);
  expect(builder.create_bge(fin4, k, mirnext::Operand::int64(819000)), 49);
  expect(builder.create_mov(mirnext::Operand::mem(mirnext::Type::u8(), flags, k),
                            mirnext::Operand::int64(0)),
         50);
  expect(builder.create_add(k, k, prime), 51);
  expect(builder.create_jmp(loop4), 52);
  expect(builder.bind(fin4), 53);
  expect(builder.create_add(count, count, mirnext::Operand::int64(1)), 54);
  expect(builder.bind(cont3), 55);
  expect(builder.create_add(i, i, mirnext::Operand::int64(1)), 56);
  expect(builder.create_jmp(loop3), 57);
  expect(builder.bind(fin3), 58);
  expect(builder.create_add(iter, iter, mirnext::Operand::int64(1)), 59);
  expect(builder.create_jmp(loop), 60);
  expect(builder.bind(fin), 61);
  expect(builder.create_ret({count}), 62);

  if (module.name() != "m_sieve") return 1;
  if (function.name() != "sieve") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 0) return 4;
  if (function.local_registers().size() != 6) return 5;
  if (function.instruction_count() != 33) return 6;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m_sieve")) return 7;
  if (!contains(text, "func sieve() -> i64")) return 8;
  if (!contains(text, "alloca %flags 819000")) return 9;
  if (!contains(text, "u8:(%flags, %i, 1)")) return 10;
  if (!contains(text, "u8:(%flags, %k, 1)")) return 11;
  if (!contains(text, "beq")) return 12;
  if (!contains(text, "jmp")) return 13;
  if (!contains(text, "ret %count")) return 14;

  return 0;
}
