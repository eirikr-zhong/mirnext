/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"
#include "mir-legacy.hpp"

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

template <class T>
concept HasPublicNop = requires(T &block) { block.nop(); };

template <class T>
concept HasPublicBts = requires(T &block, mirnext::Value cond, mirnext::Label &target) {
  block.bts(cond, target);
};

template <class T>
concept HasPublicBfs = requires(T &block, mirnext::Value cond, mirnext::Label &target) {
  block.bfs(cond, target);
};

template <class T>
concept HasPublicBeq = requires(T &block, mirnext::Value lhs, mirnext::Value rhs,
                                mirnext::Label &target) { block.beq(lhs, rhs, target); };

template <class T>
concept HasPublicBne = requires(T &block, mirnext::Value lhs, mirnext::Value rhs,
                                mirnext::Label &target) { block.bne(lhs, rhs, target); };

template <class T>
concept HasPublicBlt = requires(T &block, mirnext::Value lhs, mirnext::Value rhs,
                                mirnext::Label &target) { block.blt(lhs, rhs, target); };

template <class T>
concept HasPublicUble = requires(T &block, mirnext::Value lhs, mirnext::Value rhs,
                                 mirnext::Label &target) { block.uble(lhs, rhs, target); };

template <class T>
concept HasPublicDbge = requires(T &block, mirnext::Value lhs, mirnext::Value rhs,
                                 mirnext::Label &target) { block.dbge(lhs, rhs, target); };

template <class T>
concept HasPublicAppendOperation = requires(T &block, mirnext::Instruction instruction) {
  block.append_operation(instruction);
};

template <class T>
concept HasPublicInlineCall = requires(T &block, const mirnext::Function &callee,
                                       std::vector<mirnext::Value> args) {
  block.inline_call(callee, args);
};

template <class T>
concept HasCallableInlined = requires(T &callable, std::vector<mirnext::Value> args) {
  callable.inlined(args);
};

// Public branch DSL stays at value-producing comparisons plus bool branches.
static_assert(!HasPublicNop<mirnext::Block>);
static_assert(!HasPublicBts<mirnext::Block>);
static_assert(!HasPublicBfs<mirnext::Block>);
static_assert(!HasPublicBeq<mirnext::Block>);
static_assert(!HasPublicBne<mirnext::Block>);
static_assert(!HasPublicBlt<mirnext::Block>);
static_assert(!HasPublicUble<mirnext::Block>);
static_assert(!HasPublicDbge<mirnext::Block>);
static_assert(!HasPublicAppendOperation<mirnext::Block>);
static_assert(!HasPublicInlineCall<mirnext::Block>);
static_assert(!HasCallableInlined<mirnext::CallableRef>);

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

  mirnext::Module &false_condition_module = ctx.new_module("false_condition_error");
  mirnext::Function &false_condition_fn = false_condition_module.new_function(
      "false_condition", {}, {});
  mirnext::Label &false_condition_target = false_condition_fn.label();
  const std::size_t false_condition_instruction_count = false_condition_fn.instruction_count();
  false_condition_fn.if_not(false_condition_fn.i64(1), false_condition_target);
  if (!false_condition_module.error()
      || false_condition_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 122;
  }
  if (false_condition_fn.instruction_count() != false_condition_instruction_count) return 123;

  mirnext::Module &false_target_module = ctx.new_module("false_target_error");
  mirnext::Function &false_left = false_target_module.new_function("false_left", {}, {});
  mirnext::Function &false_right = false_target_module.new_function("false_right", {}, {});
  mirnext::Label &foreign_false_target = false_right.label();
  const std::size_t false_target_instruction_count = false_left.instruction_count();
  false_left.if_not(false_left.b(true), foreign_false_target);
  if (!false_target_module.error()
      || false_target_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 124;
  }
  if (false_left.instruction_count() != false_target_instruction_count) return 125;

  mirnext::Module &bool_mem_module = ctx.new_module("bool_mem_error");
  mirnext::Function &bool_mem_fn = bool_mem_module.new_function("bool_mem", {}, {});
  mirnext::Value storage = expect(bool_mem_fn.value(mirnext::Type::p(), "storage"), 87);
  mirnext::Memory bool_mem = bool_mem_fn.mem(mirnext::Type::b(), storage, 8);
  if (!bool_mem.is_valid()) return 88;
  if (!bool_mem_module.error()
      || bool_mem_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 89;
  }

  mirnext::Module &non_register_mem_module = ctx.new_module("non_register_mem_error");
  mirnext::Function &non_register_mem_fn = non_register_mem_module.new_function(
      "non_register_mem", {}, {});
  mirnext::Memory non_register_mem = non_register_mem_fn.mem(
      mirnext::Type::i64(), non_register_mem_fn.i64(1), 8);
  if (!non_register_mem.is_valid()
      || non_register_mem.operand().kind() != mirnext::Operand::Kind::Poison) {
    return 120;
  }
  if (!non_register_mem_module.error()
      || non_register_mem_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 121;
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
  mirnext::Label &false_target = function.label("false_target");
  expect_ok(function.if_not(equal, false_target), 198);
  expect_ok(function.ret(less), 199);
  expect_ok(target.ret(expect(target.b(false), 200)), 201);
  expect_ok(false_target.ret(expect(false_target.b(true), 209)), 210);
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
  if (!contains(text, "bf L")) return 211;
  if (!contains(text, "ret %")) return 208;
  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 212;
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
  mirnext::Value small_negated = expect(-expect(function.i32(7), 137), 138);
  mirnext::Value dneg = expect(-d, 133);
  (void)unsigned_shift;
  (void)small_negated;
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
  if (!contains(text, "negs %")) return 156;

  mirnext::Function &small_signed_function = module.new_function(
      "small_signed_ops", {mirnext::Type::i32()}, {{mirnext::Type::i32(), "a"},
                                                   {mirnext::Type::i32(), "b"}});
  mirnext::Value signed_a = expect(small_signed_function.arg("a"), 157);
  mirnext::Value signed_b = expect(small_signed_function.arg("b"), 158);
  mirnext::Value signed_arithmetic
      = expect(((signed_a + signed_b) - expect(small_signed_function.i32(1), 159))
                   * expect(small_signed_function.i32(3), 160),
               161);
  signed_arithmetic = expect((signed_arithmetic / expect(small_signed_function.i32(2), 162))
                                 % expect(small_signed_function.i32(5), 163),
                             164);
  mirnext::Value signed_bits = expect(((signed_a & expect(small_signed_function.i32(15), 165))
                                       | expect(small_signed_function.i32(32), 166))
                                          ^ signed_b,
                                      167);
  mirnext::Value signed_shifted = expect((signed_bits << expect(small_signed_function.i32(1), 168))
                                             >> expect(small_signed_function.i32(2), 169),
                                         170);
  mirnext::Label &signed_less = small_signed_function.label("less");
  expect_ok(small_signed_function.if_(signed_a < signed_b, signed_less), 171);
  expect_ok(small_signed_function.if_(signed_a <= signed_b, signed_less), 172);
  expect_ok(small_signed_function.if_(signed_a > signed_b, signed_less), 173);
  expect_ok(small_signed_function.if_(signed_a >= signed_b, signed_less), 174);
  expect_ok(small_signed_function.if_(signed_a == signed_b, signed_less), 175);
  expect_ok(small_signed_function.if_(signed_a != signed_b, signed_less), 176);
  expect_ok(small_signed_function.ret(expect(signed_arithmetic + signed_shifted, 177)), 178);
  expect_ok(signed_less.ret(expect(signed_less.i32(0), 179)), 180);
  expect_ok(small_signed_function.end(), 181);
  if (module.error()) return 182;

  mirnext::Function &small_unsigned_function = module.new_function(
      "small_unsigned_ops", {mirnext::Type::u32()}, {{mirnext::Type::u32(), "a"},
                                                     {mirnext::Type::u32(), "b"}});
  mirnext::Value unsigned_a = expect(small_unsigned_function.arg("a"), 183);
  mirnext::Value unsigned_b = expect(small_unsigned_function.arg("b"), 184);
  mirnext::Value unsigned_arithmetic
      = expect(((unsigned_a + unsigned_b) - expect(small_unsigned_function.u32(1), 185))
                   * expect(small_unsigned_function.u32(3), 186),
               187);
  unsigned_arithmetic
      = expect((unsigned_arithmetic / expect(small_unsigned_function.u32(2), 188))
                   % expect(small_unsigned_function.u32(5), 189),
               190);
  mirnext::Value unsigned_shifted = expect(unsigned_a >> expect(small_unsigned_function.u32(1), 191),
                                           192);
  mirnext::Label &unsigned_less = small_unsigned_function.label("less");
  expect_ok(small_unsigned_function.if_(unsigned_a < unsigned_b, unsigned_less), 193);
  expect_ok(small_unsigned_function.if_(unsigned_a <= unsigned_b, unsigned_less), 194);
  expect_ok(small_unsigned_function.if_(unsigned_a > unsigned_b, unsigned_less), 195);
  expect_ok(small_unsigned_function.if_(unsigned_a >= unsigned_b, unsigned_less), 196);
  expect_ok(small_unsigned_function.ret(expect(unsigned_arithmetic + unsigned_shifted, 197)),
            198);
  expect_ok(unsigned_less.ret(expect(unsigned_less.u32(0), 199)), 200);
  expect_ok(small_unsigned_function.end(), 201);
  if (module.error()) return 202;

  std::ostringstream small_out;
  ctx.dump(small_out);
  const std::string small_text = small_out.str();
  if (!contains(small_text, "func small_signed_ops(i32 %a, i32 %b) -> i32")) return 213;
  if (!contains(small_text, "adds %")) return 214;
  if (!contains(small_text, "subs %")) return 215;
  if (!contains(small_text, "muls %")) return 216;
  if (!contains(small_text, "divs %")) return 217;
  if (!contains(small_text, "mods %")) return 218;
  if (!contains(small_text, "ands %")) return 219;
  if (!contains(small_text, "ors %")) return 220;
  if (!contains(small_text, "xors %")) return 221;
  if (!contains(small_text, "lshs %")) return 222;
  if (!contains(small_text, "rshs %")) return 223;
  if (!contains(small_text, "eqs %")) return 224;
  if (!contains(small_text, "nes %")) return 225;
  if (!contains(small_text, "lts %")) return 226;
  if (!contains(small_text, "les %")) return 227;
  if (!contains(small_text, "gts %")) return 228;
  if (!contains(small_text, "ges %")) return 229;
  if (!contains(small_text, "func small_unsigned_ops(u32 %a, u32 %b) -> u32")) return 230;
  if (!contains(small_text, "udivs %")) return 231;
  if (!contains(small_text, "umods %")) return 232;
  if (!contains(small_text, "urshs %")) return 233;
  if (!contains(small_text, "ults %")) return 234;
  if (!contains(small_text, "ules %")) return 235;
  if (!contains(small_text, "ugts %")) return 236;
  if (!contains(small_text, "uges %")) return 237;

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
  mirnext::Value ptr = expect(ptr_fn.value(mirnext::Type::p(), "ptr"), 152);
  mirnext::Value bad_ptr = ptr << ptr_fn.i64(1);
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

  mirnext::Function &unsigned_mul_function = module.new_function(
      "checked_unsigned_mul", {mirnext::Type::u64()}, {{mirnext::Type::u64(), "a"},
                                                       {mirnext::Type::u64(), "b"}});
  mirnext::Value unsigned_mul_a = expect(unsigned_mul_function.arg("a"), 370).overflow(true);
  mirnext::Value unsigned_mul_b = expect(unsigned_mul_function.arg("b"), 371);
  mirnext::Label &unsigned_overflow = unsigned_mul_function.label("unsigned_overflow");
  mirnext::Value unsigned_product = expect(unsigned_mul_a * unsigned_mul_b, 372);
  if (!unsigned_product.checks_overflow() || !unsigned_product.has_overflow_result()) return 373;
  expect_ok(unsigned_mul_function.if_overflow(unsigned_product, unsigned_overflow), 374);
  expect_ok(unsigned_mul_function.ret(unsigned_product), 375);
  expect_ok(unsigned_overflow.ret(expect(unsigned_overflow.u64(0), 376)), 377);
  expect_ok(unsigned_mul_function.end(), 378);
  if (module.error()) return 379;

  mirnext::Function &unsigned_small_mul_function = module.new_function(
      "checked_unsigned_i32_mul", {mirnext::Type::u32()}, {{mirnext::Type::u32(), "a"},
                                                           {mirnext::Type::u32(), "b"}});
  mirnext::Value unsigned_small_a
      = expect(unsigned_small_mul_function.arg("a"), 380).overflow(true);
  mirnext::Value unsigned_small_b = expect(unsigned_small_mul_function.arg("b"), 381);
  mirnext::Label &unsigned_no_overflow
      = unsigned_small_mul_function.label("unsigned_no_overflow");
  mirnext::Value unsigned_small_product = expect(unsigned_small_a * unsigned_small_b, 382);
  if (!unsigned_small_product.checks_overflow()
      || !unsigned_small_product.has_overflow_result()) {
    return 383;
  }
  expect_ok(unsigned_small_mul_function.if_no_overflow(unsigned_small_product,
                                                       unsigned_no_overflow),
            384);
  expect_ok(unsigned_small_mul_function.ret(expect(unsigned_small_mul_function.u32(0), 385)),
            386);
  expect_ok(unsigned_no_overflow.ret(unsigned_small_product), 387);
  expect_ok(unsigned_small_mul_function.end(), 388);
  if (module.error()) return 389;

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

  mirnext::Function &checked_neg_function = module.new_function(
      "checked_neg", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value checked_neg_x = expect(checked_neg_function.arg("x"), 394).overflow(true);
  mirnext::Label &checked_neg_overflow = checked_neg_function.label("overflow");
  mirnext::Value negated = expect(-checked_neg_x, 395);
  if (negated.type() != mirnext::Type::i64() || !negated.checks_overflow()
      || !negated.has_overflow_result()) {
    return 396;
  }
  expect_ok(checked_neg_function.if_overflow(negated, checked_neg_overflow), 397);
  expect_ok(checked_neg_function.ret(negated), 398);
  expect_ok(checked_neg_overflow.ret(expect(checked_neg_overflow.i64(0), 399)), 400);
  expect_ok(checked_neg_function.end(), 401);
  if (module.error()) return 402;

  mirnext::Function &checked_neg_no_function = module.new_function(
      "checked_neg_no", {mirnext::Type::i32()}, {{mirnext::Type::i32(), "x"}});
  mirnext::Value checked_neg_no_x
      = expect(checked_neg_no_function.arg("x"), 403).overflow(true);
  mirnext::Label &checked_neg_no_ok = checked_neg_no_function.label("ok");
  mirnext::Value small_negated = expect(-checked_neg_no_x, 404);
  if (small_negated.type() != mirnext::Type::i32() || !small_negated.checks_overflow()
      || !small_negated.has_overflow_result()) {
    return 405;
  }
  expect_ok(checked_neg_no_function.if_no_overflow(small_negated, checked_neg_no_ok), 406);
  expect_ok(checked_neg_no_function.ret(expect(checked_neg_no_function.i32(0), 407)), 408);
  expect_ok(checked_neg_no_ok.ret(small_negated), 409);
  expect_ok(checked_neg_no_function.end(), 410);
  if (module.error()) return 411;

  mirnext::Function &checked_neg_expr_function = module.new_function(
      "checked_neg_expr", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value checked_neg_expr_x
      = expect(checked_neg_expr_function.arg("x"), 431).overflow(true);
  expect_ok(checked_neg_expr_function.ret(-checked_neg_expr_function.expr(checked_neg_expr_x)),
            432);
  expect_ok(checked_neg_expr_function.end(), 433);
  if (module.error()) return 434;

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
  if (!contains(text, "umulo %")) return 390;
  if (!contains(text, "umulos %")) return 391;
  if (!contains(text, "ubo L")) return 392;
  if (!contains(text, "ubno L")) return 393;
  if (!contains(text, "func checked_neg(i64 %x) -> i64")) return 412;
  if (!contains(text, "subo %")) return 413;
  if (!contains(text, "subos %")) return 414;
  if (!contains(text, "func checked_neg_expr(i64 %x) -> i64")) return 435;
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
  mirnext::Value ptr = expect(ptr_fn.value(mirnext::Type::p(), "p"), 327).overflow(true);
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

  mirnext::Module &neg_bool_module = ctx.new_module("overflow_neg_bool_error");
  mirnext::Function &neg_bool_fn = neg_bool_module.new_function("f", {}, {});
  mirnext::Value neg_bool_value = expect(neg_bool_fn.b(true), 415).overflow(true);
  mirnext::Value neg_bool_bad = -neg_bool_value;
  if (!neg_bool_bad.is_valid() || !neg_bool_bad.is_poison()) return 416;
  if (int code = expect_invalid(neg_bool_module, 417)) return code;

  mirnext::Module &neg_ptr_module = ctx.new_module("overflow_neg_ptr_error");
  mirnext::Function &neg_ptr_fn = neg_ptr_module.new_function("f", {}, {});
  mirnext::Value neg_ptr_value
      = expect(neg_ptr_fn.value(mirnext::Type::p(), "p"), 418).overflow(true);
  mirnext::Value neg_ptr_bad = -neg_ptr_value;
  if (!neg_ptr_bad.is_valid() || !neg_ptr_bad.is_poison()) return 419;
  if (int code = expect_invalid(neg_ptr_module, 420)) return code;

  mirnext::Module &neg_float_module = ctx.new_module("overflow_neg_float_error");
  mirnext::Function &neg_float_fn = neg_float_module.new_function("f", {}, {});
  mirnext::Value neg_float_value = expect(neg_float_fn.f64(1.0), 421).overflow(true);
  mirnext::Value neg_float_bad = -neg_float_value;
  if (!neg_float_bad.is_valid() || !neg_float_bad.is_poison()) return 422;
  if (int code = expect_invalid(neg_float_module, 423)) return code;

  mirnext::Module &neg_unsigned_module = ctx.new_module("overflow_neg_unsigned_error");
  mirnext::Function &neg_unsigned_fn = neg_unsigned_module.new_function(
      "f", {}, {{mirnext::Type::u64(), "u"}});
  mirnext::Value neg_unsigned_value = expect(neg_unsigned_fn.arg("u"), 424).overflow(true);
  mirnext::Value neg_unsigned_bad = -neg_unsigned_value;
  if (!neg_unsigned_bad.is_valid() || !neg_unsigned_bad.is_poison()) return 425;
  if (int code = expect_invalid(neg_unsigned_module, 426)) return code;

  mirnext::Module &plain_branch_module = ctx.new_module("overflow_plain_branch_error");
  mirnext::Function &plain_branch_fn = plain_branch_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}});
  mirnext::Label &plain_target = plain_branch_fn.label();
  const std::size_t plain_count = plain_branch_fn.instruction_count();
  expect_ok(plain_branch_fn.if_overflow(expect(plain_branch_fn.arg("a"), 338), plain_target),
            339);
  if (int code = expect_invalid(plain_branch_module, 340)) return code;
  if (plain_branch_fn.instruction_count() != plain_count) return 341;

  mirnext::Module &plain_neg_branch_module
      = ctx.new_module("overflow_plain_neg_branch_error");
  mirnext::Function &plain_neg_branch_fn = plain_neg_branch_module.new_function(
      "f", {}, {{mirnext::Type::i64(), "a"}});
  mirnext::Label &plain_neg_target = plain_neg_branch_fn.label();
  mirnext::Value plain_neg = -expect(plain_neg_branch_fn.arg("a"), 427);
  const std::size_t plain_neg_count = plain_neg_branch_fn.instruction_count();
  expect_ok(plain_neg_branch_fn.if_overflow(plain_neg, plain_neg_target), 428);
  if (int code = expect_invalid(plain_neg_branch_module, 429)) return code;
  if (plain_neg_branch_fn.instruction_count() != plain_neg_count) return 430;

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
  mirnext::Function &foreign = right.new_function(
      "foreign", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &caller = left.new_function(
      "caller", {}, {{mirnext::Type::i64(), "arg"}});
  caller[foreign]({expect(caller.arg("arg"), 91)});
  expect_ok(caller.ret(), 93);
  expect_ok(caller.end(), 94);
  mirnext::Result<std::vector<std::byte>> cross_bytes = left.encode_binary();
  if (cross_bytes || cross_bytes.error().code != mirnext::ErrorCode::InvalidOperand) return 92;
  return 0;
}

static int check_memory_displacement_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("memory_displacement");
  mirnext::Function &function = module.new_function("memory_displacement",
                                                    {mirnext::Type::i64()}, {});
  mirnext::Memory ptr = expect(function.alloca(16, "buf"), 430);
  function[ptr.cast_ptr(mirnext::Type::i64())[1]] = expect(function.i64(42), 432);
  mirnext::Value loaded = expect(function[ptr.cast_ptr(mirnext::Type::i64())[1]], 435);
  expect_ok(function.ret(loaded), 436);
  expect_ok(function.end(), 437);
  if (module.error()) return 438;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "mov i64:(%buf, %r0, 1, 8) 42")) return 439;
  if (!contains(text, "mov %.t1 i64:(%buf, %r0, 1, 8)")) return 440;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 441;
  return 0;
}

static int check_memory_alias_metadata_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("memory_alias_metadata");
  mirnext::Function &function = module.new_function(
      "memory_alias_metadata", {mirnext::Type::i64()},
      {{mirnext::Type::p(), "base"}, {mirnext::Type::i64(), "index"}});

  mirnext::Value base = expect(function.arg("base"), 442);
  mirnext::Value index = expect(function.arg("index"), 443);
  mirnext::Memory alias_only = function.mem(mirnext::Type::i64(), base,
                                            {.alias = "items"});
  if (alias_only.operand().memory_alias() != "items") return 444;
  if (!alias_only.operand().memory_nonalias().empty()) return 445;
  if (!alias_only.operand().has_memory_alias_metadata()) return 446;

  std::string scratch = "scratch";
  mirnext::Memory nonalias_only = function.mem(mirnext::Type::i64(), base, 16,
                                               {.nonalias = scratch.substr(0, 7)});
  if (!nonalias_only.operand().memory_alias().empty()) return 447;
  if (nonalias_only.operand().memory_nonalias() != "scratch") return 448;

  mirnext::Memory both = function.mem(mirnext::Type::i64(), base, index, 8, 24,
                                      {.alias = "items", .nonalias = "scratch"});
  if (both.operand().memory_alias() != "items") return 449;
  if (both.operand().memory_nonalias() != "scratch") return 476;
  mirnext::Memory cast = both.cast_ptr(mirnext::Type::u8());
  if (cast.operand().memory_alias() != "items"
      || cast.operand().memory_nonalias() != "scratch") {
    return 477;
  }
  mirnext::Memory offset = alias_only[2];
  if (offset.operand().memory_alias() != "items"
      || offset.operand().memory_displacement() != 16) {
    return 478;
  }
  mirnext::Memory indexed = alias_only[index];
  if (indexed.operand().memory_alias() != "items"
      || indexed.operand().memory_index_register_id() != index.operand().register_id()) {
    return 479;
  }

  function.store(alias_only, function.i64(1));
  function.store(nonalias_only, function.i64(2));
  function[cast] = function.u8(3);
  function.ret(function.load(both));
  function.end();
  if (module.error()) return 493;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "i64:(%base, %r0, 1):items")) return 494;
  if (!contains(text, "i64:(%base, %r0, 1, 16)::scratch")) return 495;
  if (!contains(text, "i64:(%base, %index, 8, 24):items:scratch")) return 496;
  if (!contains(text, "u8:(%base, %index, 8, 24):items:scratch")) return 497;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 498;

  mirnext::Module &plain_module = ctx.new_module("plain_memory_metadata_regression");
  mirnext::Function &plain = plain_module.new_function(
      "plain", {}, {{mirnext::Type::p(), "base"}});
  mirnext::Memory plain_mem = plain.mem(mirnext::Type::i64(), plain.arg("base"));
  if (plain_mem.operand().has_memory_alias_metadata()) return 499;
  plain.store(plain_mem, plain.i64(0));
  plain.ret();
  plain.end();
  std::ostringstream plain_out;
  ctx.dump(plain_out);
  if (!contains(plain_out.str(), "i64:(%base, %r0, 1) 0")) return 501;

  mirnext::Module &bad_module = ctx.new_module("bad_memory_alias_metadata");
  mirnext::Function &bad = bad_module.new_function(
      "bad", {}, {{mirnext::Type::p(), "base"}});
  (void)bad.mem(mirnext::Type::i64(), bad.arg("base"),
                mirnext::MemoryOptions{std::string_view("a\0b", 3), {}});
  if (!bad_module.error()
      || bad_module.error()->code != mirnext::ErrorCode::InvalidArgument) {
    return 502;
  }

  return 0;
}

static int check_memory_pointer_dsl_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("memory_pointer_dsl");
  mirnext::Function &function = module.new_function(
      "memory_pointer_dsl", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "index"}});

  mirnext::Memory bytes = expect(function.alloca(32, "buf"), 450);
  if (bytes.type() != mirnext::Type::u8()) return 451;
  if (bytes.operand().memory_displacement() != 0) return 452;

  mirnext::Memory words = bytes.cast_ptr(mirnext::Type::i64());
  if (words.type() != mirnext::Type::i64()) return 453;
  if (bytes.type() != mirnext::Type::u8()) return 454;

  mirnext::Memory zero = words[0];
  mirnext::Memory next = words[1];
  mirnext::Memory prev = words[-1];
  if (zero.operand().memory_displacement() != 0) return 455;
  if (next.operand().memory_displacement() != 8) return 456;
  if (prev.operand().memory_displacement() != -8) return 457;

  function[zero] = expect(function.i64(7), 458);
  function[next] = expect(function.i64(11), 459);
  mirnext::Value index = expect(function.arg("index"), 460);
  mirnext::Memory indexed = words[index];
  if (indexed.operand().memory_index_register_id() != index.operand().register_id()) return 461;
  if (indexed.operand().memory_scale() != 8) return 462;
  mirnext::Value loaded = function[next];
  expect_ok(function.ret(loaded), 463);
  expect_ok(function.end(), 464);
  if (module.error()) return 465;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "alloca %buf 32")) return 466;
  if (!contains(text, "mov i64:(%buf, %r0, 1) 7")) return 467;
  if (!contains(text, "mov i64:(%buf, %r0, 1, 8) 11")) return 468;

  mirnext::Module &typed_module = ctx.new_module("typed_alloca_pointer_dsl");
  mirnext::Function &typed_fn = typed_module.new_function("typed", {mirnext::Type::i64()}, {});
  mirnext::Memory typed = expect(typed_fn.alloca(mirnext::Type::i64(), 2, "items"), 469);
  if (typed.type() != mirnext::Type::i64()) return 470;
  typed_fn[typed[1]] = expect(typed_fn.i64(3), 471);
  typed_fn.ret(typed_fn[typed[1]]);
  typed_fn.end();
  if (typed_module.error()) return 472;

  mirnext::Module &addr_module = ctx.new_module("value_as_mem_pointer_dsl");
  mirnext::Function &addr_fn = addr_module.new_function(
      "addr", {}, {{mirnext::Type::p(), "ptr"}, {mirnext::Type::i64(), "value"}});
  mirnext::Value raw = expect(addr_fn.arg("ptr"), 473);
  mirnext::Value value = expect(addr_fn.arg("value"), 474);
  addr_fn[raw.as_mem(mirnext::Type::i64())] = value;
  addr_fn.ret();
  addr_fn.end();
  if (addr_module.error()) return 475;

  return 0;
}

static int check_memory_pointer_dsl_errors() {
  mirnext::Context ctx;

  mirnext::Module &byte_store_module = ctx.new_module("byte_store_error");
  mirnext::Function &byte_store_fn = byte_store_module.new_function("byte_store", {}, {});
  mirnext::Memory byte_mem = expect(byte_store_fn.alloca(8, "buf"), 480);
  byte_store_fn[byte_mem] = expect(byte_store_fn.i64(1), 481);
  if (!byte_store_module.error()
      || byte_store_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 482;
  }

  mirnext::Module &as_mem_module = ctx.new_module("as_mem_error");
  mirnext::Function &as_mem_fn = as_mem_module.new_function("as_mem", {}, {});
  mirnext::Memory bad_addr = as_mem_fn.i64(1).as_mem(mirnext::Type::i64());
  if (!bad_addr.is_valid()
      || bad_addr.operand().kind() != mirnext::Operand::Kind::Poison) {
    return 483;
  }
  if (!as_mem_module.error()
      || as_mem_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 484;
  }

  mirnext::Module &scale_module = ctx.new_module("memory_scale_error");
  mirnext::Function &scale_fn = scale_module.new_function(
      "scale", {}, {{mirnext::Type::i64(), "index"}});
  mirnext::Memory ld_mem = expect(scale_fn.alloca(mirnext::Type::ld(), 2, "ld"), 485);
  mirnext::Value index = expect(scale_fn.arg("index"), 486);
  mirnext::Memory bad_indexed = ld_mem[index];
  if (!bad_indexed.is_valid()
      || bad_indexed.operand().kind() != mirnext::Operand::Kind::Poison) {
    return 487;
  }
  if (!scale_module.error()
      || scale_module.error()->code != mirnext::ErrorCode::InvalidArgument) {
    return 488;
  }

  return 0;
}

static int check_call_result_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("call_result_api");
  mirnext::Function &callee = module.new_function(
      "pair",
      {mirnext::Type::i64(), mirnext::Type::i64()},
      {{mirnext::Type::i64(), "arg"}});
  mirnext::Value callee_arg = expect(callee.arg("arg"), 401);
  expect_ok(callee.ret({callee_arg, expect(callee_arg + expect(callee.i64(1), 402), 403)}), 404);
  expect_ok(callee.end(), 405);

  mirnext::Function &caller = module.new_function(
      "call_pair",
      {mirnext::Type::i64(), mirnext::Type::i64()},
      {{mirnext::Type::i64(), "arg"}});
  mirnext::Value arg = expect(caller.arg("arg"), 406);
  mirnext::CallResult result = caller[callee]({arg});
  if (result.empty()) return 407;
  if (result.size() != 2) return 408;
  if (result.values().size() != 2) return 409;

  std::size_t first = 0;
  std::size_t second = 1;
  mirnext::Value a = result[first];
  mirnext::Value b = result[second];
  if (!a.is_valid() || !b.is_valid()) return 410;
  if (a.type() != mirnext::Type::i64() || b.type() != mirnext::Type::i64()) return 411;
  if (result.values()[first].type() != mirnext::Type::i64()
      || result.values()[second].type() != mirnext::Type::i64()) {
    return 412;
  }
  expect_ok(caller.ret({result[first], result[second]}), 413);
  expect_ok(caller.end(), 414);

  mirnext::Function &take_caller = module.new_function(
      "take_pair",
      {mirnext::Type::i64(), mirnext::Type::i64()},
      {{mirnext::Type::i64(), "arg"}});
  mirnext::Value take_arg = expect(take_caller.arg("arg"), 415);
  mirnext::CallResult take_result = take_caller[callee]({take_arg});
  std::vector<mirnext::Value> taken = take_result.take_values();
  if (taken.size() != 2) return 417;
  if (taken[first].type() != mirnext::Type::i64()
      || taken[second].type() != mirnext::Type::i64()) {
    return 418;
  }
  expect_ok(take_caller.ret(taken), 419);
  expect_ok(take_caller.end(), 420);

  if (module.error()) return 421;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "func pair(i64 %arg) -> i64, i64")) return 423;
  if (!contains(text, "func call_pair(i64 %arg) -> i64, i64")) return 424;
  if (!contains(text, "call @pair @pair %.t0 %.t1 %arg")) return 425;
  if (!contains(text, "ret %.t0 %.t1")) return 426;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 427;

  mirnext::Module &jcall_module = ctx.new_module("jcall_api");
  mirnext::Function &signature = jcall_module.new_function(
      "i64_to_i64", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}},
      {.linkage = mirnext::FunctionLinkage::Signature});
  mirnext::Function &target = jcall_module.new_function(
      "add2", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Value target_arg = expect(target.arg("arg"), 428);
  target.ret(target_arg + target.i64(2));
  target.end();
  mirnext::Function &jcaller = jcall_module.new_function(
      "call_ptr", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Value target_ref = expect(jcall_module.ref(target), 429);
  mirnext::Value target_ptr = jcaller.addr(target_ref);
  mirnext::Value indirect = expect(jcaller[signature](target_ptr, {jcaller.arg("arg")}).value(), 430);
  jcaller.ret(indirect);
  jcaller.end();
  if (jcall_module.error()) return 431;

  std::ostringstream jout;
  ctx.dump(jout);
  const std::string jtext = jout.str();
  if (!contains(jtext, "proto i64_to_i64(i64 %arg) -> i64")) return 432;
  if (!contains(jtext, "jcall @i64_to_i64 %.t0 %.t1 %arg")) return 433;
  mirnext::Result<std::vector<std::byte>> jbytes = jcall_module.encode_binary();
  if (!jbytes) return 434;
  return 0;
}

static int check_function_inline_tag_api() {
  mirnext::Context ctx;
  std::string module_key = "module.enabled";
  std::string module_string_key = "module.name";
  mirnext::Module &module = ctx.new_module(
      "inline_api",
      {.attrs = {{std::string_view(module_key), true},
                 {"module.count", std::int64_t(7)},
                 {std::string_view(module_string_key), std::string("owned")}}});
  module_key.assign("changed");
  module_string_key.assign("changed");
  if (!module.bool_attr("module.enabled")) return 435;
  const mirnext::AttrValue *module_count = module.attr("module.count");
  if (module_count == nullptr || module_count->kind() != mirnext::AttrValue::Kind::Int64
      || module_count->int64_value() == nullptr || *module_count->int64_value() != 7) {
    return 436;
  }
  const mirnext::AttrValue *module_name = module.attr("module.name");
  if (module_name == nullptr || module_name->string_value() == nullptr
      || *module_name->string_value() != "owned") {
    return 437;
  }
  module.set_attr("module.u", std::uint64_t(9));
  const mirnext::AttrValue *module_u = module.attr("module.u");
  if (module_u == nullptr || module_u->uint64_value() == nullptr
      || *module_u->uint64_value() != 9) {
    return 438;
  }
  if (&module.set_attr("module.flag", true) != &module || !module.bool_attr("module.flag")) {
    return 439;
  }
  if (&module.remove_attr("module.flag") != &module || module.has_attr("module.flag")) {
    return 440;
  }

  std::string inline_key = std::string(mirnext::attr::Inline);
  mirnext::Function &inline_callee = module.new_function(
      "add1", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}},
      {.attrs = {{std::string_view(inline_key), true}}});
  inline_key.assign("changed");
  if (!inline_callee.bool_attr(mirnext::attr::Inline)) return 441;
  if (!inline_callee.has_attr(mirnext::attr::Inline)) return 442;
  mirnext::Value inline_arg = expect(inline_callee.arg("x"), 443);
  inline_callee.ret(inline_arg + inline_callee.i64(1));
  inline_callee.end();

  mirnext::Function &caller = module.new_function(
      "use_inline_option", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value caller_arg = expect(caller.arg("x"), 444);
  mirnext::CallResult inline_result = caller[inline_callee]({caller_arg});
  if (inline_result.size() != 1 || inline_result.value().type() != mirnext::Type::i64()) {
    return 445;
  }
  caller.ret(inline_result.value());
  caller.end();

  mirnext::Function &setter_callee = module.new_function(
      "setter_add", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  if (setter_callee.bool_attr(mirnext::attr::Inline)) return 446;
  if (&setter_callee.set_attr(mirnext::attr::Inline, true) != &setter_callee
      || !setter_callee.bool_attr(mirnext::attr::Inline)) {
    return 447;
  }
  setter_callee.set_attr("notes", "callee metadata");
  const mirnext::AttrValue *notes = setter_callee.attr("notes");
  if (notes == nullptr || notes->string_value() == nullptr
      || *notes->string_value() != "callee metadata") {
    return 448;
  }
  setter_callee.set_attr(mirnext::attr::Inline, std::int64_t(1));
  if (setter_callee.bool_attr(mirnext::attr::Inline)) return 449;
  setter_callee.set_attr(mirnext::attr::Inline, true);
  mirnext::Value setter_arg = expect(setter_callee.arg("x"), 450);
  setter_callee.ret(setter_arg + setter_callee.i64(2));
  setter_callee.end();

  mirnext::Function &setter_caller = module.new_function(
      "use_inline_setter", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value setter_caller_arg = expect(setter_caller.arg("x"), 451);
  mirnext::Value setter_value = setter_caller[setter_callee]({setter_caller_arg}).value();
  setter_caller.ret(setter_value);
  setter_caller.end();

  if (&setter_callee.set_attr(mirnext::attr::Inline, false) != &setter_callee
      || setter_callee.bool_attr(mirnext::attr::Inline)) {
    return 452;
  }
  mirnext::Function &plain_caller = module.new_function(
      "use_plain_after_disable", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value plain_arg = expect(plain_caller.arg("x"), 453);
  mirnext::Value plain_value = plain_caller[setter_callee]({plain_arg}).value();
  plain_caller.ret(plain_value);
  plain_caller.end();

  mirnext::Function &multi = module.new_function(
      "inline_pair",
      {mirnext::Type::i64(), mirnext::Type::i64()},
      {{mirnext::Type::i64(), "x"}},
      {.attrs = {{mirnext::attr::Inline, true}}});
  mirnext::Value multi_arg = expect(multi.arg("x"), 454);
  multi.ret({multi_arg, multi_arg + multi.i64(3)});
  multi.end();

  mirnext::Function &multi_caller = module.new_function(
      "use_inline_pair",
      {mirnext::Type::i64(), mirnext::Type::i64()},
      {{mirnext::Type::i64(), "x"}});
  mirnext::CallResult pair = multi_caller[multi]({multi_caller.arg("x")});
  if (pair.size() != 2 || pair[0].type() != mirnext::Type::i64()
      || pair[1].type() != mirnext::Type::i64()) {
    return 455;
  }
  multi_caller.ret({pair[0], pair[1]});
  multi_caller.end();

  mirnext::Function &vararg = module.new_function(
      "inline_vararg", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "first"}},
      {.vararg = true, .attrs = {{mirnext::attr::Inline, true}}});
  vararg.ret(vararg.arg("first"));
  vararg.end();
  mirnext::Function &vararg_caller = module.new_function("use_inline_vararg",
                                                         {mirnext::Type::i64()}, {});
  mirnext::Value vararg_value
      = vararg_caller[vararg]({vararg_caller.i64(1), vararg_caller.i64(2)}).value();
  vararg_caller.ret(vararg_value);
  vararg_caller.end();

  mirnext::Function &signature = module.new_function(
      "inline_sig", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}},
      {.linkage = mirnext::FunctionLinkage::Signature,
       .attrs = {{mirnext::attr::Inline, true}}});
  if (!signature.bool_attr(mirnext::attr::Inline)) return 456;
  mirnext::Function &indirect_caller = module.new_function(
      "indirect_inline_sig", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value callee_ref = expect(module.ref(inline_callee), 457);
  mirnext::Value callee_ptr = indirect_caller.addr(callee_ref);
  mirnext::Value indirect_value
      = indirect_caller[signature](callee_ptr, {indirect_caller.arg("x")}).value();
  indirect_caller.ret(indirect_value);
  indirect_caller.end();

  if (module.error()) return 458;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "inline @add1 @add1")) return 459;
  if (!contains(text, "inline @setter_add @setter_add")) return 460;
  if (!contains(text, "call @setter_add @setter_add")) return 461;
  if (!contains(text, "inline @inline_pair @inline_pair")) return 462;
  if (!contains(text, "inline @inline_vararg @inline_vararg")) return 463;
  if (!contains(text, "jcall @inline_sig")) return 464;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 465;

  mirnext::Context error_ctx;
  mirnext::Module &bad_module = error_ctx.new_module("bad_inline_signature_call");
  mirnext::Function &bad_sig = bad_module.new_function(
      "bad_sig", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}},
      {.linkage = mirnext::FunctionLinkage::Signature,
       .attrs = {{mirnext::attr::Inline, true}}});
  mirnext::Function &bad_caller = bad_module.new_function(
      "bad_caller", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Value bad = bad_caller[bad_sig]({bad_caller.arg("x")}).value();
  if (!bad.is_valid() || !bad.is_poison()) return 466;
  if (!bad_module.error()
      || bad_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 467;
  }

  return 0;
}

static int check_vararg_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m_vararg");
  mirnext::Function &signature = module.new_function(
      "sum_i64_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "count"}},
      {.linkage = mirnext::FunctionLinkage::Signature, .vararg = true});
  mirnext::Function &function = module.new_vararg_function(
      "sum_i64", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "count"}});

  if (!signature.is_vararg()) return 480;
  if (!function.is_vararg()) return 481;

  mirnext::Var ap = function.var(mirnext::Type::p(), "ap");
  mirnext::Var block_dst = function.var(mirnext::Type::p(), "block_dst");
  mirnext::Var total = function.var(mirnext::Type::i64(), "total");
  mirnext::Var i = function.var(mirnext::Type::i64(), "i");
  mirnext::Label &loop = function.label("loop");
  mirnext::Label &done = function.label("done");

  function[total] = 0;
  function[i] = 0;
  function.va_start(ap.value());
  function.va_block_arg(block_dst.value(), ap.value(), 16);
  function.jmp(loop);

  loop.if_(loop[i] >= function.arg("count"), done);
  mirnext::Value next = loop.va_arg(mirnext::Type::i64(), ap.value());
  loop[total] = loop[total] + next;
  loop[i] = loop[i] + 1;
  loop.jmp(loop);

  done.va_end(ap.value());
  done.ret(done[total]);
  function.end();

  if (module.error()) return 482;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "proto sum_i64_p(i64 %count, ...) -> i64")) return 483;
  if (!contains(text, "func sum_i64(i64 %count, ...) -> i64")) return 484;
  if (!contains(text, "va_start %ap")) return 485;
  if (!contains(text, "va_arg %")) return 486;
  if (!contains(text, "va_block_arg %")) return 487;
  if (!contains(text, "va_end %ap")) return 488;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 489;

  mirnext::Module &bad_start_module = ctx.new_module("bad_start");
  mirnext::Function &bad_start = bad_start_module.new_function("bad", {}, {});
  mirnext::Var bad_ap = bad_start.var(mirnext::Type::p(), "ap");
  bad_start.va_start(bad_ap.value());
  bad_start.end();
  if (!bad_start_module.error()) return 490;

  mirnext::Module &bad_type_module = ctx.new_module("bad_type");
  mirnext::Function &bad_type = bad_type_module.new_vararg_function("bad_type", {}, {});
  mirnext::Var bad_type_ap = bad_type.var(mirnext::Type::p(), "ap");
  (void)bad_type.va_arg(mirnext::Type::b(), bad_type_ap.value());
  bad_type.end();
  if (!bad_type_module.error()) return 491;

  mirnext::Module &bad_ap_module = ctx.new_module("bad_ap");
  mirnext::Function &bad_ap_fn = bad_ap_module.new_vararg_function("bad_ap", {}, {});
  mirnext::Var not_ap = bad_ap_fn.var(mirnext::Type::i64(), "not_ap");
  bad_ap_fn.va_start(not_ap.value());
  bad_ap_fn.end();
  if (!bad_ap_module.error()) return 492;

  mirnext::Module &call_module = ctx.new_module("vararg_call");
  mirnext::Function &printf_like_import = call_module.new_function(
      "printf_like", {mirnext::Type::i64()}, {{mirnext::Type::p(), "fmt"}},
      {.linkage = mirnext::FunctionLinkage::Import, .vararg = true});
  mirnext::Function &caller = call_module.new_function("caller", {mirnext::Type::i64()}, {});
  mirnext::Data &fmt_data = call_module.string_data("fmt", "%d");
  mirnext::Value fmt_ref = expect(call_module.ref(fmt_data), 493);
  mirnext::Value fmt = caller.addr(fmt_ref);
  mirnext::Value result = caller[printf_like_import]({fmt, caller.i64(10), caller.i64(20)}).value();
  caller.ret(result);
  caller.end();
  if (call_module.error()) return 494;
  mirnext::Result<std::vector<std::byte>> call_bytes = call_module.encode_binary();
  if (!call_bytes) return 495;

  return 0;
}

static int check_indirect_control_flow_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("indirect_control");
  mirnext::Function &function = module.new_function(
      "jump_by_label_addr", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "x"}});
  mirnext::Label &target = function.label("target");

  mirnext::Value target_addr = expect(function.label_addr(target), 500);
  if (target_addr.type() != mirnext::Type::p()
      || target_addr.operand().kind() != mirnext::Operand::Kind::Register) {
    return 501;
  }
  function.jmp(target_addr);
  function.ret(function.i64(0));
  target.ret(target.i64(42));
  function.end();
  if (module.error()) return 502;

  mirnext::Function &named = module.new_function("named_label_addr", {mirnext::Type::i64()}, {});
  mirnext::Label &done = named.label("done");
  mirnext::Value done_addr = expect(named.label_addr(done, "done_addr"), 503);
  named.jmp(done_addr);
  done.ret(done.i64(7));
  named.end();
  if (module.error()) return 504;

  mirnext::Function &return_to = module.new_function("return_to_label_addr", {}, {});
  mirnext::Label &exit = return_to.label("exit");
  mirnext::Value exit_addr = expect(return_to.label_addr(exit), 505);
  return_to.ret_to(exit_addr);
  exit.ret();
  return_to.end();
  if (module.error()) return 506;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "reg p %")) return 507;
  if (!contains(text, "laddr %")) return 508;
  if (!contains(text, "jmpi %")) return 509;
  if (!contains(text, "ret 42")) return 510;
  if (!contains(text, "reg p %done_addr")) return 511;
  if (!contains(text, "laddr %done_addr")) return 512;
  if (!contains(text, "jret %")) return 513;

  mirnext::Context error_ctx;
  mirnext::Module &target_error_module = error_ctx.new_module("target_error");
  mirnext::Function &left = target_error_module.new_function("left", {}, {});
  mirnext::Function &right = target_error_module.new_function("right", {}, {});
  mirnext::Label &right_label = right.label();
  const std::size_t left_local_count = left.local_registers().size();
  mirnext::Value bad_addr = left.label_addr(right_label);
  if (!bad_addr.is_valid() || !bad_addr.is_poison()) return 514;
  if (!target_error_module.error()
      || target_error_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 515;
  }
  if (left.local_registers().size() != left_local_count) return 516;

  mirnext::Module &jmp_error_module = error_ctx.new_module("jmpi_error");
  mirnext::Function &bad_jmp = jmp_error_module.new_function("bad_jmpi", {}, {});
  const std::size_t bad_jmp_instruction_count = bad_jmp.instruction_count();
  bad_jmp.jmp(bad_jmp.i64(1));
  if (!jmp_error_module.error()
      || jmp_error_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 517;
  }
  if (bad_jmp.instruction_count() != bad_jmp_instruction_count) return 518;

  mirnext::Module &ret_error_module = error_ctx.new_module("jret_error");
  mirnext::Function &bad_ret = ret_error_module.new_function("bad_jret", {}, {});
  mirnext::Value literal_pointer = bad_ret.value(mirnext::Type::p(), "literal_pointer");
  (void)literal_pointer;
  const std::size_t bad_ret_instruction_count = bad_ret.instruction_count();
  bad_ret.ret_to(bad_ret.i64(1));
  if (!ret_error_module.error()
      || ret_error_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 519;
  }
  if (bad_ret.instruction_count() != bad_ret_instruction_count) return 520;

  mirnext::Module &cross_value_module = error_ctx.new_module("cross_value_error");
  mirnext::Function &cross_left = cross_value_module.new_function("cross_left", {}, {});
  mirnext::Function &cross_right = cross_value_module.new_function("cross_right", {}, {});
  mirnext::Value foreign_pointer = cross_right.value(mirnext::Type::p(), "foreign_pointer");
  cross_left.ret_to(foreign_pointer);
  if (!cross_value_module.error()
      || cross_value_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 521;
  }

  mirnext::Module &ref_error_module = error_ctx.new_module("ref_error");
  mirnext::Function &ref_fn = ref_error_module.new_function("ref_fn", {}, {});
  mirnext::Data &data = ref_error_module.string_data("data", "x");
  mirnext::Value ref = expect(ref_error_module.ref(data), 522);
  ref_fn.ret_to(ref);
  if (!ref_error_module.error()
      || ref_error_module.error()->code != mirnext::ErrorCode::InvalidOperand) {
    return 523;
  }

  return 0;
}

static int check_label_stack_scope_api() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("label_stack_scope");
  mirnext::Function &function = module.new_function("example", {}, {});
  mirnext::Label &work = function.label("work", {.stack_scope = true});
  mirnext::Label &done = function.label("done");

  if (!work.is_stack_scoped()) return 524;
  if (done.is_stack_scoped()) return 525;

  function.jmp(work);
  mirnext::Memory tmp = work.alloca(mirnext::Type::i64(), "tmp");
  work.store(tmp, work.i64(42));
  work.jmp(done);
  done.ret();
  function.end();
  if (module.error()) return 526;

  const auto &instructions = function.instructions();
  bool saw_bstart = false;
  bool saw_bend_before_jmp = false;
  bool saw_plain_done_label = false;
  for (std::size_t i = 0; i + 1 < instructions.size(); ++i) {
    if (instructions[i]->opcode() == mirnext::Opcode::BStart) saw_bstart = true;
    if (instructions[i]->opcode() == mirnext::Opcode::BEnd
        && instructions[i + 1]->opcode() == mirnext::Opcode::Jmp) {
      saw_bend_before_jmp = true;
    }
    if (instructions[i]->opcode() == mirnext::Opcode::Label
        && instructions[i]->label_id() == done.id()
        && instructions[i + 1]->opcode() != mirnext::Opcode::BStart) {
      saw_plain_done_label = true;
    }
  }
  if (!saw_bstart) return 527;
  if (!saw_bend_before_jmp) return 528;
  if (!saw_plain_done_label) return 529;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "reg p %.scope0")) return 530;
  if (!contains(text, "bstart %.scope0")) return 531;
  if (!contains(text, "alloca %tmp 8")) return 532;
  if (!contains(text, "bend %.scope0")) return 533;
  if (!contains(text, "jmp L")) return 534;

  mirnext::Result<std::vector<std::byte>> bytes = module.encode_binary();
  if (!bytes) return 535;
  mirnext::LegacyContext legacy;
  mirnext::Result<mirnext::LegacyLoweredFunction> lowered
      = mirnext::lower_to_legacy(legacy, module, function);
  if (!lowered || lowered->function == nullptr) return 536;

  mirnext::Module &ret_module = ctx.new_module("label_stack_ret");
  mirnext::Function &ret_fn = ret_module.new_function("ret_scoped", {}, {});
  mirnext::Label &ret_label = ret_fn.label("ret_label", {.stack_scope = true});
  ret_fn.jmp(ret_label);
  ret_label.ret();
  ret_fn.end();
  if (ret_module.error()) return 537;
  const auto &ret_instructions = ret_fn.instructions();
  bool saw_bend_before_ret = false;
  for (std::size_t i = 0; i + 1 < ret_instructions.size(); ++i) {
    if (ret_instructions[i]->opcode() == mirnext::Opcode::BEnd
        && ret_instructions[i + 1]->opcode() == mirnext::Opcode::Ret) {
      saw_bend_before_ret = true;
    }
  }
  if (!saw_bend_before_ret) return 538;

  mirnext::Module &branch_module = ctx.new_module("label_stack_branch");
  mirnext::Function &branch_fn = branch_module.new_function(
      "branch_scoped", {}, {{mirnext::Type::i64(), "x"}});
  mirnext::Label &branch_work = branch_fn.label("branch_work", {.stack_scope = true});
  mirnext::Label &branch_done = branch_fn.label("branch_done");
  mirnext::Value branch_x = expect(branch_fn.arg("x"), 546);
  branch_fn.jmp(branch_work);
  branch_work.if_(branch_x == branch_work.i64(0), branch_done);
  branch_work.jmp(branch_work);
  branch_done.ret();
  branch_fn.end();
  if (branch_module.error()) return 539;
  const auto &branch_instructions = branch_fn.instructions();
  bool saw_bend_before_bt = false;
  bool saw_bend_before_self_loop = false;
  for (std::size_t i = 0; i + 1 < branch_instructions.size(); ++i) {
    if (branch_instructions[i]->opcode() == mirnext::Opcode::BEnd
        && branch_instructions[i + 1]->opcode() == mirnext::Opcode::Bt) {
      saw_bend_before_bt = true;
    }
    if (branch_instructions[i]->opcode() == mirnext::Opcode::BEnd
        && branch_instructions[i + 1]->opcode() == mirnext::Opcode::Jmp
        && branch_instructions[i + 1]->operands()[0].label_id() == branch_work.id()) {
      saw_bend_before_self_loop = true;
    }
  }
  if (!saw_bend_before_bt) return 540;
  if (!saw_bend_before_self_loop) return 541;

  mirnext::Module &switch_module = ctx.new_module("label_stack_switch");
  mirnext::Function &switch_fn = switch_module.new_function(
      "switch_scoped", {}, {{mirnext::Type::i64(), "x"}});
  mirnext::Label &switch_work = switch_fn.label("switch_work", {.stack_scope = true});
  mirnext::Label &case0 = switch_fn.label("case0");
  mirnext::Label &case1 = switch_fn.label("case1");
  mirnext::Value switch_x = expect(switch_fn.arg("x"), 547);
  switch_fn.jmp(switch_work);
  switch_work.switch_(switch_x, {&case0, &case1});
  case0.ret();
  case1.ret();
  switch_fn.end();
  if (switch_module.error()) return 542;
  const auto &switch_instructions = switch_fn.instructions();
  bool saw_bend_before_switch = false;
  for (std::size_t i = 0; i + 1 < switch_instructions.size(); ++i) {
    if (switch_instructions[i]->opcode() == mirnext::Opcode::BEnd
        && switch_instructions[i + 1]->opcode() == mirnext::Opcode::Switch) {
      saw_bend_before_switch = true;
    }
  }
  if (!saw_bend_before_switch) return 543;

  mirnext::Module &fallthrough_module = ctx.new_module("label_stack_fallthrough");
  mirnext::Function &fallthrough_fn = fallthrough_module.new_function("fallthrough_scoped", {}, {});
  mirnext::Label &fallthrough_work = fallthrough_fn.label("fallthrough_work", {.stack_scope = true});
  fallthrough_fn.jmp(fallthrough_work);
  (void)fallthrough_work.alloca(mirnext::Type::i64(), "slot");
  fallthrough_fn.end();
  if (fallthrough_module.error()) return 544;
  const auto &fallthrough_instructions = fallthrough_fn.instructions();
  if (fallthrough_instructions.empty()
      || fallthrough_instructions.back()->opcode() != mirnext::Opcode::BEnd) {
    return 545;
  }

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
  if (int code = check_memory_displacement_api()) return code;
  if (int code = check_memory_alias_metadata_api()) return code;
  if (int code = check_memory_pointer_dsl_api()) return code;
  if (int code = check_memory_pointer_dsl_errors()) return code;
  if (int code = check_call_result_api()) return code;
  if (int code = check_function_inline_tag_api()) return code;
  if (int code = check_vararg_api()) return code;
  if (int code = check_indirect_control_flow_api()) return code;
  if (int code = check_label_stack_scope_api()) return code;

  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m");
  mirnext::Function &import = module.new_function(
      "add1", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}},
      {.linkage = mirnext::FunctionLinkage::Import});
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
  mirnext::Value call_result = expect(function[import]({count.value()}).value(), 117);
  expect_ok(fin.ret(multiplied), 118);
  expect_ok(function.end(), 119);

  if (module.name() != "m") return 1;
  if (module.functions().size() != 2) return 2;
  if (function.name() != "loop" || function.arguments().size() != 1) return 3;
  if (!call_result.is_valid() || call_result.operand().kind() != mirnext::Operand::Kind::Register) {
    return 4;
  }

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m")) return 5;
  if (!contains(text, "import add1(i64 %arg) -> i64")) return 7;
  if (!contains(text, "func loop(i64 %arg1) -> i64")) return 8;
  if (!contains(text, "add %")) return 9;
  if (!contains(text, "mul %")) return 14;
  if (!contains(text, "ge %")) return 15;
  if (!contains(text, "label L2")) return 16;
  if (!contains(text, "lt %")) return 17;
  if (!contains(text, "bt L")) return 20;
  if (!contains(text, "call @add1 @add1 %")) return 18;
  if (!contains(text, "ret %")) return 19;

  return 0;
}
