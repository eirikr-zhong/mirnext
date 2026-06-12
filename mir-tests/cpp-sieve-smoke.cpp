/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static bool contains(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) std::exit(code);
  return *result;
}

template <class T> static T expect(T value, int) {
  return value;
}

#define expect_ok(expr, code) \
  do {                        \
    (void)(code);             \
    expr;                     \
  } while (false)

int main() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m_sieve");
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::Label &loop = function.label();
  mirnext::Label &loop2 = function.label();
  mirnext::Label &loop3 = function.label();
  mirnext::Label &loop4 = function.label();
  mirnext::Label &fin = function.label();
  mirnext::Label &fin2 = function.label();
  mirnext::Label &fin3 = function.label();
  mirnext::Label &fin4 = function.label();
  mirnext::Label &cont3 = function.label();

  mirnext::Value flags = expect(function.alloca(819000, "flags"), 21);
  mirnext::Var iter = expect(function.var(mirnext::Type::i64(), "iter"), 22);
  mirnext::Var count = expect(function.var(mirnext::Type::i64(), "count"), 23);
  mirnext::Var i = expect(function.var(mirnext::Type::i64(), "i"), 24);
  mirnext::Var k = expect(function.var(mirnext::Type::i64(), "k"), 25);
  mirnext::Var prime = expect(function.var(mirnext::Type::i64(), "prime"), 26);
  expect_ok(iter.assign(expect(function.i64(0), 27)), 28);
  expect_ok(function.jmp(loop), 29);

  mirnext::Value loop_iter = expect(loop.load(iter), 30);
  expect_ok(loop.if_(expect(loop_iter >= expect(loop.i64(100), 31), 32), fin), 33);
  expect_ok(loop.store(count, expect(loop.i64(0), 34)), 35);
  expect_ok(loop.store(i, expect(loop.i64(0), 36)), 37);
  expect_ok(loop.jmp(loop2), 38);

  mirnext::Value loop2_i = expect(loop2.load(i), 39);
  expect_ok(loop2.if_(expect(loop2_i >= expect(loop2.i64(819000), 40), 41), fin2), 42);
  expect_ok(loop2.store(expect(loop2.mem(mirnext::Type::u8(), flags, loop2_i), 43),
                        expect(loop2.u8(1), 44)),
            45);
  expect_ok(loop2.store(i, expect(loop2_i + expect(loop2.i64(1), 46), 47)), 48);
  expect_ok(loop2.jmp(loop2), 49);

  expect_ok(fin2.store(i, expect(fin2.i64(1), 50)), 51);
  expect_ok(fin2.jmp(loop3), 52);

  mirnext::Value loop3_i = expect(loop3.load(i), 53);
  expect_ok(loop3.if_(expect(loop3_i >= expect(loop3.i64(819000), 54), 55), fin3), 56);
  mirnext::Value flag = expect(loop3.load(expect(loop3.mem(mirnext::Type::u8(), flags, loop3_i), 57)),
                               58);
  expect_ok(loop3.if_(expect(flag == expect(loop3.u8(0), 59), 60), cont3), 61);
  mirnext::Value next_prime = expect(loop3_i + expect(loop3.i64(1), 62), 63);
  expect_ok(loop3.store(prime, next_prime), 64);
  expect_ok(loop3.store(k, expect(loop3_i + next_prime, 65)), 66);
  expect_ok(loop3.jmp(loop4), 67);

  mirnext::Value loop4_k = expect(loop4.load(k), 68);
  expect_ok(loop4.if_(expect(loop4_k >= expect(loop4.i64(819000), 69), 70), fin4), 71);
  expect_ok(loop4.store(expect(loop4.mem(mirnext::Type::u8(), flags, loop4_k), 72),
                        expect(loop4.u8(0), 73)),
            74);
  expect_ok(loop4.store(k, expect(loop4_k + expect(loop4.load(prime), 75), 76)), 77);
  expect_ok(loop4.jmp(loop4), 78);

  mirnext::Value fin4_count = expect(fin4.load(count), 79);
  expect_ok(fin4.store(count, expect(fin4_count + expect(fin4.i64(1), 80), 81)), 82);
  expect_ok(fin4.jmp(cont3), 83);

  mirnext::Value cont3_i = expect(cont3.load(i), 84);
  expect_ok(cont3.store(i, expect(cont3_i + expect(cont3.i64(1), 85), 86)), 87);
  expect_ok(cont3.jmp(loop3), 88);

  mirnext::Value fin3_iter = expect(fin3.load(iter), 89);
  expect_ok(fin3.store(iter, expect(fin3_iter + expect(fin3.i64(1), 90), 91)), 92);
  expect_ok(fin3.jmp(loop), 93);

  expect_ok(fin.ret(expect(fin.load(count), 94)), 95);
  expect_ok(function.end(), 96);
  if (module.error()) return 97;

  if (module.name() != "m_sieve") return 1;
  if (function.name() != "sieve") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 0) return 4;
  if (function.local_registers().size() != 19) return 5;
  if (function.instruction_count() == 0) return 6;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m_sieve")) return 7;
  if (!contains(text, "func sieve() -> i64")) return 8;
  if (!contains(text, "alloca %flags 819000")) return 9;
  if (!contains(text, "u8:(%flags, %i, 1)")) return 10;
  if (!contains(text, "u8:(%flags, %k, 1)")) return 11;
  if (!contains(text, "eq %")) return 12;
  if (!contains(text, "bt L")) return 15;
  if (!contains(text, "jmp")) return 13;
  if (!contains(text, "ret %count")) return 14;

  return 0;
}
