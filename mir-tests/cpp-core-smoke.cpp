/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdlib>
#include <limits>
#include <sstream>
#include <string>
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

static int check_result_api() {
  mirnext::Result<int> ok = 7;
  if (!ok || *ok != 7 || ok.value_ptr() == nullptr || ok.error_ptr() != nullptr) return 10;
  mirnext::Result<int> bad
      = mirnext::Error{mirnext::ErrorCode::InvalidArgument, "expected failure"};
  if (bad || bad.value_ptr() != nullptr || bad.error_ptr() == nullptr) return 11;
  mirnext::Result<void> void_ok;
  if (!void_ok || void_ok.has_error()) return 12;
  mirnext::Result<void> void_bad
      = mirnext::Error{mirnext::ErrorCode::InvalidOperand, "expected void failure"};
  if (void_bad || !void_bad.has_error() || void_bad.error_ptr() == nullptr) return 13;
  return 0;
}

static int check_block_tree_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("blocks");
  mirnext::Function &function = module.new_function(
      "sum_to_n", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});

  if (module.kind() != mirnext::Block::Kind::Module) return 20;
  if (function.kind() != mirnext::Block::Kind::Function) return 21;
  if (function.parent() != &module || &function.module() != &module) return 22;

  mirnext::Value n = expect(function.arg("n"), 39);
  mirnext::Var i = expect(function.var(mirnext::Type::i64(), "i"), 40);
  mirnext::Var sum = expect(function.var(mirnext::Type::i64(), "sum"), 41);
  mirnext::Label &loop = function.label("loop");
  mirnext::Label &done = function.label("done");
  if (loop.parent() != &function || done.parent() != &function) return 23;
  if (loop.id() >= done.id()) return 24;
  if (&n.block() != &function || &i.block() != &function) return 25;

  function[i] = 1;
  function[sum] = 0;
  expect_ok(function.jmp(loop), 44);

  mirnext::Value limit_d = expect(n.convert(mirnext::Type::Kind::D), 45);
  mirnext::Value limit = expect(limit_d.convert(mirnext::Type::Kind::I64), 46);
  loop.if_(loop[i] > limit, done);
  loop[sum] = loop[sum] + loop[i];
  loop[i] = loop[i] + 1;
  expect_ok(loop.jmp(loop), 56);
  done.ret(done[sum]);
  expect_ok(function.end(), 58);
  if (module.error()) return 59;

  if (function.local_registers().size() != 7) return 29;
  const auto &instructions = function.instructions();
  if (instructions.size() < 12) return 30;
  if (instructions[0]->opcode() != mirnext::Opcode::Mov) return 31;
  bool saw_loop_label = false;
  bool saw_ret = false;
  for (const auto &instruction : instructions) {
    if (instruction->opcode() == mirnext::Opcode::Label && instruction->label_id() == loop.id()) {
      saw_loop_label = true;
    }
    if (instruction->opcode() == mirnext::Opcode::Ret) saw_ret = true;
  }
  if (!saw_loop_label) return 32;
  if (!saw_ret) return 33;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "func sum_to_n(i64 %n) -> i64")) return 34;
  if (!contains(text, "i2d %")) return 35;
  if (!contains(text, "d2i %")) return 36;
  if (!contains(text, "gt %")) return 37;
  if (!contains(text, "bt L")) return 42;
  if (!contains(text, "ret %sum")) return 38;
  return 0;
}

static int check_global_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("globals");
  mirnext::Var global = expect(module.var(mirnext::Type::i64(), "g"), 54);
  mirnext::Value initial = expect(module.i64(42), 55);
  expect_ok(global.init(initial), 56);
  mirnext::Value ro = expect(module.value(mirnext::Type::d(), "pi"), 57);
  if (!global.is_valid() || !ro.is_valid()) return 50;
  if (&global.block() != &module || &ro.block() != &module) return 51;

  mirnext::Function &function = module.new_function("bad_global_load", {}, {});
  mirnext::Value loaded = function.load(global);
  if (!loaded.is_valid() || !loaded.is_poison()) return 52;
  if (!module.error() || module.error()->code != mirnext::ErrorCode::InvalidOperand) return 53;

  mirnext::Function &dsl_function = module.new_function("bad_global_dsl_load", {}, {});
  dsl_function[global] = 1;
  dsl_function.end();
  if (!module.error() || module.error()->code != mirnext::ErrorCode::InvalidOperand) return 58;
  return 0;
}

static int check_errors() {
  mirnext::Context ctx;
  mirnext::Module &cross_module = ctx.new_module("cross_error");
  mirnext::Function &left = cross_module.new_function(
      "left", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  mirnext::Function &right = cross_module.new_function(
      "right", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});

  mirnext::Value left_arg = expect(left.arg("n"), 78);
  mirnext::Value right_arg = expect(right.arg("n"), 79);
  const std::size_t cross_instruction_count = left.instruction_count();
  const std::size_t cross_local_count = left.local_registers().size();
  mirnext::Value cross = left_arg + right_arg;
  if (!cross.is_valid() || !cross.is_poison()) return 70;
  if (!cross_module.error() || cross_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 71;
  }
  left.var(mirnext::Type::i64(), "after_cross");
  left.ret(left_arg + left.i64(1));
  if (left.instruction_count() != cross_instruction_count
      || left.local_registers().size() != cross_local_count) {
    return 72;
  }
  mirnext::Result<std::vector<std::byte>> cross_error_bytes = cross_module.encode_binary();
  if (cross_error_bytes
      || cross_error_bytes.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 84;
  }

  mirnext::Module &missing_module = ctx.new_module("missing_error");
  mirnext::Function &missing_fn = missing_module.new_function(
      "missing", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  mirnext::Value missing = missing_fn.arg("missing");
  if (!missing.is_valid() || !missing.is_poison()) return 73;
  if (!missing_module.error() || missing_module.error()->code != mirnext::ErrorCode::UnknownName) {
    return 74;
  }

  mirnext::Module &mixed_module = ctx.new_module("mixed_error");
  mirnext::Function &mixed_fn = mixed_module.new_function(
      "mixed", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  mirnext::Value mixed_arg = mixed_fn.arg("n");
  mirnext::Value mixed = mixed_arg + mixed_fn.u64(1);
  if (!mixed.is_valid() || !mixed.is_poison()) return 75;
  if (!mixed_module.error() || mixed_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 76;
  }

  mirnext::Module &mod_module = ctx.new_module("mod_error");
  mirnext::Function &mod_fn = mod_module.new_function("mod", {}, {});
  mirnext::Value bad_mod = mod_fn.f32(1.0f) % mod_fn.f32(2.0f);
  if (!bad_mod.is_valid() || !bad_mod.is_poison()) return 77;
  if (!mod_module.error() || mod_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 78;
  }

  mirnext::Module &duplicate_module = ctx.new_module("duplicate_error");
  mirnext::Function &duplicate_fn = duplicate_module.new_function(
      "duplicate", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  const std::size_t duplicate_local_count = duplicate_fn.local_registers().size();
  mirnext::Var duplicate = duplicate_fn.var(mirnext::Type::i64(), "n");
  if (!duplicate.is_valid()) return 79;
  if (!duplicate_module.error()
      || duplicate_module.error()->code != mirnext::ErrorCode::DuplicateName) {
    return 80;
  }
  if (duplicate_fn.local_registers().size() != duplicate_local_count) return 81;

  mirnext::Module &assign_module = ctx.new_module("assign_error");
  mirnext::Function &assign_left = assign_module.new_function("left", {}, {});
  mirnext::Function &assign_right = assign_module.new_function("right", {}, {});
  mirnext::Var right_local = expect(assign_right.var(mirnext::Type::i64(), "local"), 82);
  assign_left[right_local] = 1;
  assign_left.end();
  if (!assign_module.error() || assign_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 83;
  }

  mirnext::Module &condition_module = ctx.new_module("condition_error");
  mirnext::Function &condition_fn = condition_module.new_function("condition", {}, {});
  mirnext::Label &condition_target = condition_fn.label();
  const std::size_t condition_instruction_count = condition_fn.instruction_count();
  condition_fn.if_(condition_fn.i64(1), condition_target);
  if (!condition_module.error()
      || condition_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 85;
  }
  if (condition_fn.instruction_count() != condition_instruction_count) return 86;

  mirnext::Module &bool_mem_module = ctx.new_module("bool_mem_error");
  mirnext::Function &bool_mem_fn = bool_mem_module.new_function("bool_mem", {}, {});
  mirnext::Value storage = expect(bool_mem_fn.alloca(8, "storage"), 87);
  mirnext::Memory bool_mem = bool_mem_fn.mem(mirnext::Type::b(), storage);
  if (!bool_mem.is_valid()) return 88;
  if (!bool_mem_module.error()
      || bool_mem_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 89;
  }

  return 0;
}

static int check_bool_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("bools");
  mirnext::Function &function = module.new_function(
      "bool_ops", {mirnext::Type::b()}, {{mirnext::Type::i64(), "a"},
                                         {mirnext::Type::i64(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 190);
  mirnext::Value b = expect(function.arg("b"), 191);
  mirnext::Value less = expect(a < b, 192);
  mirnext::Value equal = expect(a == b, 193);
  if (less.type() != mirnext::Type::b() || equal.type() != mirnext::Type::b()) return 194;
  mirnext::Value literal = expect(function.b(true), 195);
  if (literal.type() != mirnext::Type::b()) return 196;
  mirnext::Label &target = function.label("target");
  expect_ok(function.if_(literal, target), 197);
  expect_ok(function.ret(less), 198);
  expect_ok(target.ret(expect(target.b(false), 199)), 200);
  expect_ok(function.end(), 201);
  if (module.error()) return 202;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "func bool_ops(i64 %a, i64 %b) -> b")) return 203;
  if (!contains(text, "reg b %")) return 204;
  if (!contains(text, "lt %")) return 205;
  if (!contains(text, "eq %")) return 206;
  if (!contains(text, "bt L")) return 207;
  if (!contains(text, "ret %")) return 208;
  return 0;
}

static int check_expression_ops() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("expr_ops");
  mirnext::Function &function = module.new_function(
      "ops", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"},
                                      {mirnext::Type::u64(), "u"},
                                      {mirnext::Type::d(), "d"}});
  mirnext::Value n = expect(function.arg("n"), 120);
  mirnext::Value u = expect(function.arg("u"), 121);
  mirnext::Value d = expect(function.arg("d"), 122);

  mirnext::Value bits = expect(((n & expect(function.i64(15), 123))
                                | expect(function.i64(32), 124))
                                   ^ expect(function.i64(7), 125),
                               126);
  mirnext::Value shifted = expect((bits << expect(function.i64(1), 127))
                                      >> expect(function.i64(2), 128),
                                  129);
  mirnext::Value unsigned_shift = expect(u >> expect(function.u64(3), 130), 131);
  mirnext::Value negated = expect(-shifted, 132);
  mirnext::Value dneg = expect(-d, 133);
  (void)unsigned_shift;
  expect_ok(function.if_(expect(dneg < expect(function.f64(0.0), 134), 135), function.label()), 136);
  expect_ok(function.ret(negated), 139);
  expect_ok(function.end(), 140);
  if (module.error()) return 141;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "and %")) return 142;
  if (!contains(text, "or %")) return 143;
  if (!contains(text, "xor %")) return 144;
  if (!contains(text, "lsh %")) return 145;
  if (!contains(text, "rsh %")) return 146;
  if (!contains(text, "ursh %")) return 147;
  if (!contains(text, "neg %")) return 148;
  if (!contains(text, "dneg %")) return 149;

  mirnext::Context error_ctx;
  mirnext::Module &float_module = error_ctx.new_module("float_bitwise_error");
  mirnext::Function &float_fn = float_module.new_function("f", {}, {});
  mirnext::Value bad_float = float_fn.f64(1.0) & float_fn.f64(2.0);
  if (!bad_float.is_valid() || !bad_float.is_poison()) return 150;
  if (!float_module.error()
      || float_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 151;
  }

  mirnext::Module &ptr_module = error_ctx.new_module("ptr_shift_error");
  mirnext::Function &ptr_fn = ptr_module.new_function("p", {}, {});
  mirnext::Value bad_ptr = ptr_fn.alloca(8, "buf") << ptr_fn.i64(1);
  if (!bad_ptr.is_valid() || !bad_ptr.is_poison()) return 152;
  if (!ptr_module.error()
      || ptr_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 153;
  }

  mirnext::Module &mixed_module = error_ctx.new_module("mixed_bitwise_error");
  mirnext::Function &mixed_fn = mixed_module.new_function("m", {}, {});
  mirnext::Value bad_mixed = mixed_fn.i64(1) & mixed_fn.u64(1);
  if (!bad_mixed.is_valid() || !bad_mixed.is_poison()) return 154;
  if (!mixed_module.error()
      || mixed_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 155;
  }

  return 0;
}

static int check_overflow_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("checked");
  mirnext::Function &function = module.new_function(
      "checked_add", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                              {mirnext::Type::i64(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 260);
  mirnext::Value b = expect(function.arg("b"), 261);
  mirnext::Value a_copy = a;
  mirnext::Label &overflow = function.label("overflow");
  if (a.checks_overflow() || a.has_overflow_result()) return 262;
  a.overflow(true);
  if (!a.checks_overflow() || a_copy.checks_overflow()) return 263;
  mirnext::Value sum = expect(a + b, 264);
  if (sum.type() != mirnext::Type::i64() || !sum.checks_overflow()
      || !sum.has_overflow_result()) {
    return 265;
  }
  expect_ok(function.if_overflow(sum, overflow), 266);
  expect_ok(function.ret(sum), 267);
  expect_ok(overflow.ret(expect(overflow.i64(-1), 268)), 269);
  expect_ok(function.end(), 270);
  if (module.error()) return 271;

  mirnext::Function &no_function = module.new_function(
      "checked_sub_no", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                                 {mirnext::Type::i64(), "b"}});
  mirnext::Value no_a = expect(no_function.arg("a"), 272).overflow(true);
  mirnext::Value no_b = expect(no_function.arg("b"), 273);
  mirnext::Label &ok = no_function.label("ok");
  mirnext::Value diff = expect(no_a - no_b, 274);
  expect_ok(no_function.if_no_overflow(diff, ok), 275);
  expect_ok(no_function.ret(expect(no_function.i64(-1), 276)), 277);
  expect_ok(ok.ret(diff), 278);
  expect_ok(no_function.end(), 279);
  if (module.error()) return 280;

  mirnext::Function &chain_function = module.new_function(
      "checked_chain", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                                {mirnext::Type::i64(), "b"},
                                                {mirnext::Type::i64(), "c"}});
  mirnext::Value chain_a = expect(chain_function.arg("a"), 281);
  mirnext::Value chain_b = expect(chain_function.arg("b"), 282);
  mirnext::Value chain_c = expect(chain_function.arg("c"), 283);
  chain_a.overflow(true);
  mirnext::Value chain_sum = expect(chain_a + chain_b, 284);
  mirnext::Value product = expect(chain_sum * chain_c, 285);
  if (!product.checks_overflow() || !product.has_overflow_result()) return 286;
  expect_ok(chain_function.ret(product), 287);
  expect_ok(chain_function.end(), 288);
  if (module.error()) return 289;

  mirnext::Function &small_function = module.new_function(
      "checked_i32", {mirnext::Type::i32()}, {{mirnext::Type::i32(), "a"},
                                             {mirnext::Type::i32(), "b"}});
  mirnext::Value small_a = expect(small_function.arg("a"), 290).overflow(true);
  mirnext::Value small_b = expect(small_function.arg("b"), 291);
  expect_ok(small_function.ret(expect(small_a + small_b, 292)), 293);
  expect_ok(small_function.end(), 294);
  if (module.error()) return 295;

  mirnext::Function &plain_function = module.new_function(
      "overflow_disabled", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                                   {mirnext::Type::i64(), "b"}});
  mirnext::Value plain_a = expect(plain_function.arg("a"), 296);
  mirnext::Value plain_b = expect(plain_function.arg("b"), 297);
  plain_a.overflow(true).overflow(false);
  mirnext::Value plain = expect(plain_a + plain_b, 298);
  if (plain.checks_overflow() || plain.has_overflow_result()) return 299;
  expect_ok(plain_function.ret(plain), 300);
  expect_ok(plain_function.end(), 301);
  if (module.error()) return 302;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "func checked_add(i64 %a, i64 %b) -> i64")) return 303;
  if (!contains(text, "addo %")) return 304;
  if (!contains(text, "subo %")) return 305;
  if (!contains(text, "mulo %")) return 306;
  if (!contains(text, "addos %")) return 307;
  if (!contains(text, "bo L")) return 308;
  if (!contains(text, "bno L")) return 309;
  if (!contains(text, "func overflow_disabled")) return 310;

  auto expect_invalid = [](mirnext::Module &m, int code) {
    return m.error() && m.error()->code == mirnext::ErrorCode::InvalidOperand ? 0 : code;
  };

  mirnext::Module &div_module = ctx.new_module("overflow_div_error");
  mirnext::Function &div_fn = div_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Value div_a = expect(div_fn.arg("a"), 311).overflow(true);
  mirnext::Value div_bad = div_a / expect(div_fn.arg("b"), 312);
  if (!div_bad.is_valid() || !div_bad.is_poison()) return 313;
  if (int code = expect_invalid(div_module, 314)) return code;

  mirnext::Module &bit_module = ctx.new_module("overflow_bit_error");
  mirnext::Function &bit_fn = bit_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Value bit_a = expect(bit_fn.arg("a"), 315).overflow(true);
  mirnext::Value bit_bad = bit_a & expect(bit_fn.arg("b"), 316);
  if (!bit_bad.is_valid() || !bit_bad.is_poison()) return 317;
  if (int code = expect_invalid(bit_module, 318)) return code;

  mirnext::Module &cmp_module = ctx.new_module("overflow_cmp_error");
  mirnext::Function &cmp_fn = cmp_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Value cmp_a = expect(cmp_fn.arg("a"), 319).overflow(true);
  mirnext::Value cmp_bad = cmp_a < expect(cmp_fn.arg("b"), 320);
  if (!cmp_bad.is_valid() || !cmp_bad.is_poison()) return 321;
  if (int code = expect_invalid(cmp_module, 322)) return code;

  mirnext::Module &unsigned_module = ctx.new_module("overflow_unsigned_error");
  mirnext::Function &unsigned_fn = unsigned_module.new_function(
      "f", {}, {{mirnext::Type::u64(), "a"}, {mirnext::Type::u64(), "b"}});
  mirnext::Value unsigned_a = expect(unsigned_fn.arg("a"), 323).overflow(true);
  mirnext::Value unsigned_bad = unsigned_a + expect(unsigned_fn.arg("b"), 324);
  if (!unsigned_bad.is_valid() || !unsigned_bad.is_poison()) return 325;
  if (int code = expect_invalid(unsigned_module, 326)) return code;

  mirnext::Module &ptr_module = ctx.new_module("overflow_ptr_error");
  mirnext::Function &ptr_fn = ptr_module.new_function("f", {}, {});
  mirnext::Value ptr = expect(ptr_fn.alloca(8, "p"), 327).overflow(true);
  mirnext::Value ptr_bad = ptr + ptr;
  if (!ptr_bad.is_valid() || !ptr_bad.is_poison()) return 328;
  if (int code = expect_invalid(ptr_module, 329)) return code;

  mirnext::Module &bool_module = ctx.new_module("overflow_bool_error");
  mirnext::Function &bool_fn = bool_module.new_function("f", {}, {});
  mirnext::Value bool_value = expect(bool_fn.b(true), 330).overflow(true);
  mirnext::Value bool_bad = bool_value + expect(bool_fn.b(false), 331);
  if (!bool_bad.is_valid() || !bool_bad.is_poison()) return 332;
  if (int code = expect_invalid(bool_module, 333)) return code;

  mirnext::Module &float_module = ctx.new_module("overflow_float_error");
  mirnext::Function &float_fn = float_module.new_function("f", {}, {});
  mirnext::Value float_value = expect(float_fn.f64(1.0), 334).overflow(true);
  mirnext::Value float_bad = float_value + expect(float_fn.f64(2.0), 335);
  if (!float_bad.is_valid() || !float_bad.is_poison()) return 336;
  if (int code = expect_invalid(float_module, 337)) return code;

  mirnext::Module &plain_branch_module = ctx.new_module("overflow_plain_branch_error");
  mirnext::Function &plain_branch_fn = plain_branch_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}});
  mirnext::Label &plain_target = plain_branch_fn.label();
  const std::size_t plain_count = plain_branch_fn.instruction_count();
  expect_ok(plain_branch_fn.if_overflow(expect(plain_branch_fn.arg("a"), 338), plain_target),
            339);
  if (int code = expect_invalid(plain_branch_module, 340)) return code;
  if (plain_branch_fn.instruction_count() != plain_count) return 341;

  mirnext::Module &stale_module = ctx.new_module("overflow_stale_branch_error");
  mirnext::Function &stale_fn = stale_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Value stale_a = expect(stale_fn.arg("a"), 342).overflow(true);
  mirnext::Value stale_sum = expect(stale_a + expect(stale_fn.arg("b"), 343), 344);
  expect_ok(stale_fn.convert(stale_sum, mirnext::Type::d()), 345);
  mirnext::Label &stale_target = stale_fn.label();
  const std::size_t stale_count = stale_fn.instruction_count();
  expect_ok(stale_fn.if_overflow(stale_sum, stale_target), 346);
  if (int code = expect_invalid(stale_module, 347)) return code;
  if (stale_fn.instruction_count() != stale_count) return 348;

  mirnext::Module &consumed_module = ctx.new_module("overflow_consumed_branch_error");
  mirnext::Function &consumed_fn = consumed_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Value consumed_a = expect(consumed_fn.arg("a"), 349).overflow(true);
  mirnext::Value consumed_sum = expect(consumed_a + expect(consumed_fn.arg("b"), 350), 351);
  mirnext::Label &consumed_target = consumed_fn.label();
  expect_ok(consumed_fn.if_overflow(consumed_sum, consumed_target), 352);
  const std::size_t consumed_count = consumed_fn.instruction_count();
  expect_ok(consumed_fn.if_overflow(consumed_sum, consumed_target), 353);
  if (int code = expect_invalid(consumed_module, 354)) return code;
  if (consumed_fn.instruction_count() != consumed_count) return 355;

  mirnext::Module &cross_block_module = ctx.new_module("overflow_cross_block_error");
  mirnext::Function &cross_block_fn = cross_block_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Label &cross_label = cross_block_fn.label();
  mirnext::Value cross_a = expect(cross_block_fn.arg("a"), 356).overflow(true);
  mirnext::Value cross_sum = expect(cross_a + expect(cross_block_fn.arg("b"), 357), 358);
  expect_ok(cross_label.if_overflow(cross_sum, cross_label), 359);
  if (int code = expect_invalid(cross_block_module, 360)) return code;

  mirnext::Module &cross_function_module = ctx.new_module("overflow_cross_function_error");
  mirnext::Function &left = cross_function_module.new_function(
      "left", {}, {{mirnext::Type::i64(), "a"}, {mirnext::Type::i64(), "b"}});
  mirnext::Function &right = cross_function_module.new_function("right", {}, {});
  mirnext::Value left_a = expect(left.arg("a"), 361).overflow(true);
  mirnext::Value left_sum = expect(left_a + expect(left.arg("b"), 362), 363);
  mirnext::Label &right_target = right.label();
  expect_ok(right.if_overflow(left_sum, right_target), 364);
  if (int code = expect_invalid(cross_function_module, 365)) return code;

  return 0;
}

static int check_switch_and_data_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("switch_data");
  mirnext::Data &buf = module.bss("buf", 64);
  mirnext::Data &nums = module.data("nums", mirnext::Type::i64(),
                                    std::vector<std::int64_t>{1, 2, 3});
  mirnext::Data &msg = module.string_data("msg", "hello");
  mirnext::Function &callee = module.new_function("callee", {}, {});
  callee.ret();
  callee.end();
  mirnext::Data &callee_ref = module.ref_data("callee_ref", callee);
  mirnext::Value data_ref = expect(module.ref(nums), 160);
  if (!buf.is_valid() || !nums.is_valid() || !msg.is_valid() || !callee_ref.is_valid()) return 161;
  if (!data_ref.is_valid() || data_ref.operand().reference_kind()
                                  != mirnext::Operand::ReferenceKind::Data) {
    return 162;
  }

  mirnext::Function &function = module.new_function(
      "switchy", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  mirnext::Value n = expect(function.arg("n"), 163);
  mirnext::Label &a = function.label("a");
  mirnext::Label &b = function.label("b");
  mirnext::Label &c = function.label("c");
  function.switch_(n, {&a, &b});
  a.ret(a.i64(10));
  b.ret(b.i64(20));
  c.ret(c.i64(30));
  function.end();
  if (module.error()) return 164;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "buf: bss 64")) return 165;
  if (!contains(text, "nums: data i64 1 2 3")) return 166;
  if (!contains(text, "msg: string \"hello\"")) return 167;
  if (!contains(text, "callee_ref: ref @callee 0")) return 168;
  if (!contains(text, "switch %n L")) return 169;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 170;

  mirnext::Module &addr_module = ctx.new_module("addr_data");
  mirnext::Data &addr_nums = addr_module.data("nums", mirnext::Type::i64(),
                                              std::vector<std::int64_t>{10, 20});
  mirnext::Function &addr_function = addr_module.new_function("addr_user", {}, {});
  mirnext::Value nums_ref = expect(addr_module.ref(addr_nums), 180);
  mirnext::Value nums_ptr = expect(addr_function.addr(nums_ref, "nums_ptr"), 181);
  if (!nums_ptr.is_valid() || nums_ptr.is_poison() || nums_ptr.type() != mirnext::Type::p()) {
    return 182;
  }
  addr_function.ret();
  addr_function.end();
  if (addr_module.error()) return 183;
  std::ostringstream addr_out;
  ctx.dump(addr_out);
  if (!contains(addr_out.str(), "addr %nums_ptr @nums")) return 184;

  mirnext::Module &bad_addr_module = ctx.new_module("bad_addr");
  mirnext::Function &bad_addr_function = bad_addr_module.new_function("bad", {}, {});
  mirnext::Value bad_addr = bad_addr_function.addr(bad_addr_function.i64(1));
  if (!bad_addr.is_valid() || !bad_addr.is_poison()) return 185;
  if (!bad_addr_module.error()
      || bad_addr_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 186;
  }

  mirnext::Module &addr_left = ctx.new_module("addr_left");
  mirnext::Data &left_data = addr_left.data("left_data", mirnext::Type::i64(),
                                            std::vector<std::int64_t>{1});
  mirnext::Module &addr_right = ctx.new_module("addr_right");
  mirnext::Function &right_function = addr_right.new_function("right", {}, {});
  mirnext::Value cross_ref = expect(addr_right.ref(left_data), 187);
  mirnext::Value cross_addr = right_function.addr(cross_ref);
  if (!cross_addr.is_valid() || !cross_addr.is_poison()) return 188;
  if (!addr_right.error() || addr_right.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 189;
  }

  mirnext::Module &case_module = ctx.new_module("case_switch");
  mirnext::Function &case_function = case_module.new_function(
      "case_switch", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});
  mirnext::Value case_n = expect(case_function.arg("n"), 171);
  mirnext::Label &ten = case_function.label("ten");
  mirnext::Label &twenty = case_function.label("twenty");
  mirnext::Label &other = case_function.label("other");
  case_function.switch_(case_n, {{10, &ten}, {20, &twenty}}, other);
  ten.ret(ten.i64(10));
  twenty.ret(twenty.i64(20));
  other.ret(other.i64(-1));
  case_function.end();
  if (case_module.error()) return 172;

  mirnext::Module &duplicate_module = ctx.new_module("duplicate_switch");
  mirnext::Function &duplicate_function = duplicate_module.new_function(
      "duplicate_switch", {}, {{mirnext::Type::i64(), "n"}});
  mirnext::Label &duplicate_target = duplicate_function.label();
  duplicate_function.switch_(expect(duplicate_function.arg("n"), 173),
                             {{1, &duplicate_target}, {1, &duplicate_target}},
                             duplicate_target);
  if (!duplicate_module.error()
      || duplicate_module.error()->code != mirnext::ErrorCode::DuplicateName) {
    return 174;
  }

  mirnext::Module &float_switch_module = ctx.new_module("float_switch");
  mirnext::Function &float_switch = float_switch_module.new_function("float_switch", {}, {});
  mirnext::Label &float_target = float_switch.label();
  float_switch.switch_(float_switch.f64(1.0), {&float_target});
  if (!float_switch_module.error()
      || float_switch_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 175;
  }

  mirnext::Module &cross_switch_module = ctx.new_module("cross_switch");
  mirnext::Function &left = cross_switch_module.new_function(
      "left", {}, {{mirnext::Type::i64(), "n"}});
  mirnext::Function &right = cross_switch_module.new_function("right", {}, {});
  mirnext::Label &right_label = right.label();
  left.switch_(expect(left.arg("n"), 176), {&right_label});
  if (!cross_switch_module.error()
      || cross_switch_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 177;
  }

  mirnext::Module &bad_data_type = ctx.new_module("bad_data_type");
  bad_data_type.data("bad", mirnext::Type::f(), std::vector<std::int64_t>{1});
  if (!bad_data_type.error()
      || bad_data_type.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 178;
  }

  mirnext::Module &bad_data_range = ctx.new_module("bad_data_range");
  bad_data_range.data("bad", mirnext::Type::i8(), std::vector<std::int64_t>{128});
  if (!bad_data_range.error()
      || bad_data_range.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 179;
  }

  return 0;
}

static int check_binary_encode_errors() {
  mirnext::Context ctx;
  mirnext::Module &bad = ctx.new_module("binary_bad");
  mirnext::Function &nop = bad.new_function(
      "nop", {}, {{mirnext::Type::i64(), "arg"}});
  (void)nop.arg("missing");
  mirnext::Result<std::vector<std::byte>> bad_bytes = bad.encode_binary();
  if (bad_bytes || bad_bytes.error().code != mirnext::ErrorCode::UnknownName) return 90;

  mirnext::Module &left = ctx.new_module("binary_left");
  mirnext::Module &right = ctx.new_module("binary_right");
  mirnext::Prototype &prototype = left.new_prototype(
      "p", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &foreign = right.new_function(
      "foreign", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &caller = left.new_function(
      "caller", {}, {{mirnext::Type::i64(), "arg"}});
  expect_ok(caller.call_void(prototype, foreign, {expect(caller.arg("arg"), 91)}), 92);
  expect_ok(caller.ret(), 93);
  expect_ok(caller.end(), 94);
  mirnext::Result<std::vector<std::byte>> cross_bytes = left.encode_binary();
  if (cross_bytes || cross_bytes.error().code != mirnext::ErrorCode::InvalidOperand) return 92;
  return 0;
}

int main() {
  if (int code = check_result_api()) return code;
  if (int code = check_block_tree_api()) return code;
  if (int code = check_global_api()) return code;
  if (int code = check_errors()) return code;
  if (int code = check_bool_api()) return code;
  if (int code = check_expression_ops()) return code;
  if (int code = check_overflow_api()) return code;
  if (int code = check_switch_and_data_api()) return code;
  if (int code = check_binary_encode_errors()) return code;

  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m");
  mirnext::Prototype &prototype = module.new_prototype(
      "add1_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Import &import = module.new_import("add1");
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Value arg = expect(function.arg("arg1"), 101);
  mirnext::Var count = expect(function.var(mirnext::Type::i64(), "count"), 102);
  mirnext::Label &fin = function.label("fin");
  mirnext::Label &cont = function.label("cont");
  expect_ok(count.assign(expect(function.i64(0), 103)), 104);
  mirnext::Value result = expect(arg + expect(function.i64(1), 105), 106);
  mirnext::Value multiplied = expect(result * expect(function.i64(3), 107), 108);
  mirnext::Value loop_cond = expect(multiplied >= expect(function.i64(42), 109), 110);
  expect_ok(function.if_(loop_cond, fin), 111);
  mirnext::Value next = expect(count.value() + expect(cont.i64(1), 112), 113);
  mirnext::Value cont_cond = expect(next < expect(cont.i64(42), 114), 115);
  expect_ok(cont.if_(cont_cond, cont), 116);
  mirnext::Value call_result = expect(function.call(prototype, import, {count.value()}), 117);
  expect_ok(fin.ret(multiplied), 118);
  expect_ok(function.end(), 119);

  if (module.name() != "m") return 1;
  if (module.prototypes().size() != 1 || module.imports().size() != 1) return 2;
  if (function.name() != "loop" || function.arguments().size() != 1) return 3;
  if (!call_result.is_valid() || call_result.operand().kind() != mirnext::Operand::Kind::Register) {
    return 4;
  }

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m")) return 5;
  if (!contains(text, "proto add1_p(i64 %arg) -> i64")) return 6;
  if (!contains(text, "import add1")) return 7;
  if (!contains(text, "func loop(i64 %arg1) -> i64")) return 8;
  if (!contains(text, "add %")) return 9;
  if (!contains(text, "mul %")) return 14;
  if (!contains(text, "ge %")) return 15;
  if (!contains(text, "label L2")) return 16;
  if (!contains(text, "lt %")) return 17;
  if (!contains(text, "bt L")) return 20;
  if (!contains(text, "call @add1_p @add1 %")) return 18;
  if (!contains(text, "ret %")) return 19;

  return 0;
}
