/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

static bool contains(const std::string &text, const char *needle) {
  return text.find(needle) != std::string::npos;
}

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) std::exit(code);
  return result.value();
}

int main() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m");
  mirnext::Prototype &prototype = module.new_prototype(
      "add1_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Import &import = module.new_import("add1");
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 17);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 18);
  mirnext::Register tmp = expect(function.create_register(mirnext::Type::u64(), "tmp"), 31);
  mirnext::Register ptr = expect(function.create_register(mirnext::Type::p(), "ptr"), 32);
  mirnext::Register fp = expect(function.create_register(mirnext::Type::f(), "fp"), 53);
  mirnext::Register dp = expect(function.create_register(mirnext::Type::d(), "dp"), 54);
  mirnext::Register ldp = expect(function.create_register(mirnext::Type::ld(), "ldp"), 55);

  mirnext::Result<mirnext::Register> duplicate
      = function.create_register(mirnext::Type::i64(), "count");
  if (duplicate || duplicate.error().code != mirnext::ErrorCode::DuplicateName) return 19;

  mirnext::Result<mirnext::Register> missing = function.argument("missing");
  if (missing || missing.error().code != mirnext::ErrorCode::UnknownName) return 20;

  mirnext::IRBuilder unset_builder(ctx);
  mirnext::Result<mirnext::Instruction *> no_insert_point
      = unset_builder.create_ret({mirnext::Operand::int64(0)});
  if (no_insert_point || no_insert_point.error().code != mirnext::ErrorCode::NoInsertPoint) {
    return 21;
  }

  mirnext::IRBuilder builder(ctx);
  builder.set_insert_point(function);
  mirnext::Label fin = expect(builder.create_label(), 22);
  mirnext::Label cont = expect(builder.create_label(), 23);

  expect(builder.create_mov(count, mirnext::Operand::int64(0)), 24);
  expect(builder.create_neg(tmp, count), 33);
  expect(builder.create_sub(tmp, tmp, mirnext::Operand::int64(1)), 34);
  expect(builder.create_mul(tmp, tmp, mirnext::Operand::int64(2)), 35);
  expect(builder.create_udiv(tmp, tmp, mirnext::Operand::int64(2)), 36);
  expect(builder.create_umod(tmp, tmp, mirnext::Operand::int64(7)), 37);
  expect(builder.create_and(tmp, tmp, mirnext::Operand::int64(15)), 38);
  expect(builder.create_or(tmp, tmp, mirnext::Operand::int64(16)), 39);
  expect(builder.create_xor(tmp, tmp, mirnext::Operand::int64(3)), 40);
  expect(builder.create_lsh(tmp, tmp, mirnext::Operand::int64(1)), 41);
  expect(builder.create_ursh(tmp, tmp, mirnext::Operand::int64(1)), 42);
  expect(builder.create_eq(tmp, tmp, mirnext::Operand::int64(0)), 43);
  expect(builder.create_ult(tmp, tmp, mirnext::Operand::int64(10)), 44);
  expect(builder.create_addo(tmp, tmp, mirnext::Operand::int64(1)), 45);
  expect(builder.create_bo(fin), 46);
  expect(builder.create_bt(fin, tmp), 47);
  expect(builder.create_bne(fin, tmp, mirnext::Operand::int64(0)), 48);
  expect(builder.create_ubgt(fin, tmp, mirnext::Operand::int64(1)), 49);
  expect(builder.create_mov(ptr, mirnext::Operand::int64(0)), 50);
  expect(builder.create_fmov(fp, mirnext::Operand::float32(1.25f)), 56);
  expect(builder.create_dmov(dp, mirnext::Operand::float64(2.5)), 57);
  expect(builder.create_ldmov(ldp, mirnext::Operand::long_double(3.75L)), 58);
  expect(builder.create_call(prototype, mirnext::Operand::ref(import), {tmp}, {count}), 51);
  expect(builder.create_call(prototype, mirnext::Operand::ref(function), {tmp}, {count}), 52);
  expect(builder.create_bge(fin, count, arg1), 25);
  expect(builder.bind(cont), 26);
  expect(builder.create_add(count, count, mirnext::Operand::int64(1)), 27);
  expect(builder.create_blt(cont, count, arg1), 28);
  expect(builder.bind(fin), 29);
  expect(builder.create_ret({count}), 30);

  if (module.name() != "m") return 1;
  if (module.prototypes().size() != 1) return 51;
  if (module.imports().size() != 1) return 52;
  if (function.name() != "loop") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 1) return 4;
  if (function.local_registers().size() != 6) return 5;
  if (function.instruction_count() != 30) return 6;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m")) return 7;
  if (!contains(text, "proto add1_p(i64 %arg) -> i64")) return 51;
  if (!contains(text, "import add1")) return 52;
  if (!contains(text, "func loop(i64 %arg1) -> i64")) return 8;
  if (!contains(text, "reg i64 %count")) return 9;
  if (!contains(text, "reg u64 %tmp")) return 31;
  if (!contains(text, "reg p %ptr")) return 32;
  if (!contains(text, "reg f %fp")) return 53;
  if (!contains(text, "reg d %dp")) return 54;
  if (!contains(text, "reg ld %ldp")) return 55;
  if (!contains(text, "mov %count 0")) return 10;
  if (!contains(text, "bge L1 %count %arg1")) return 11;
  if (!contains(text, "label L2")) return 12;
  if (!contains(text, "add %count %count 1")) return 13;
  if (!contains(text, "blt L2 %count %arg1")) return 14;
  if (!contains(text, "label L1")) return 15;
  if (!contains(text, "ret %count")) return 16;
  if (!contains(text, "neg %tmp %count")) return 33;
  if (!contains(text, "udiv %tmp %tmp 2")) return 34;
  if (!contains(text, "umod %tmp %tmp 7")) return 35;
  if (!contains(text, "and %tmp %tmp 15")) return 36;
  if (!contains(text, "or %tmp %tmp 16")) return 37;
  if (!contains(text, "xor %tmp %tmp 3")) return 38;
  if (!contains(text, "lsh %tmp %tmp 1")) return 39;
  if (!contains(text, "ursh %tmp %tmp 1")) return 40;
  if (!contains(text, "eq %tmp %tmp 0")) return 41;
  if (!contains(text, "ult %tmp %tmp 10")) return 42;
  if (!contains(text, "addo %tmp %tmp 1")) return 43;
  if (!contains(text, "bo L1")) return 44;
  if (!contains(text, "bt L1 %tmp")) return 45;
  if (!contains(text, "bne L1 %tmp 0")) return 46;
  if (!contains(text, "ubgt L1 %tmp 1")) return 47;
  if (!contains(text, "fmov %fp 1.25f")) return 56;
  if (!contains(text, "dmov %dp 2.5d")) return 57;
  if (!contains(text, "ldmov %ldp 3.75ld")) return 58;
  if (!contains(text, "call @add1_p @add1 %tmp %count")) return 48;
  if (!contains(text, "call @add1_p @loop %tmp %count")) return 49;

  return 0;
}
