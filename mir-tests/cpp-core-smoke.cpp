/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdint>
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
  return result.value();
}

static mirnext::Result<int> result_success() { return 7; }

static mirnext::Result<int> result_failure() {
  return mirnext::Error{mirnext::ErrorCode::InvalidArgument, "expected failure"};
}

static mirnext::Result<int> try_success() {
  MIRNEXT_TRY(auto value, result_success());
  return value + 1;
}

static mirnext::Result<int> try_failure() {
  MIRNEXT_TRY(auto value, result_failure());
  return value + 1;
}

static mirnext::Result<int> try_assign_success() {
  int value = 0;
  MIRNEXT_TRY(value, result_success());
  return value + 2;
}

static int check_builder_errors() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("errors");
  mirnext::Function &function = module.new_function(
      "bad", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();

  mirnext::Value missing = entry.arg("missing");
  if (missing.is_valid()) return 70;
  if (builder.ok() || builder.error().code != mirnext::ErrorCode::UnknownName) return 71;

  entry.local(mirnext::Type::i64(), "after_error");
  if (builder.error().code != mirnext::ErrorCode::UnknownName) return 72;
  if (function.local_registers().size() != 0) return 73;

  mirnext::Function &type_function = module.new_function(
      "type_bad", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder type_builder(type_function);
  mirnext::Label type_entry = type_builder.entry();
  mirnext::Value lhs = type_entry.arg("arg1");
  mirnext::Value rhs = type_entry.local(mirnext::Type::u64(), "u");
  mirnext::Value invalid_sum = lhs + rhs;
  if (invalid_sum.is_valid()) return 74;
  if (type_builder.ok() || type_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 75;
  }

  mirnext::Function &invalid_function = module.new_function("invalid_value", {mirnext::Type::i64()},
                                                            {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder invalid_builder(invalid_function);
  mirnext::Label invalid_entry = invalid_builder.entry();
  mirnext::Value invalid_value_sum = invalid_entry.i64(1) + mirnext::Value();
  if (invalid_value_sum.is_valid()) return 78;
  if (invalid_builder.ok()
      || invalid_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 79;
  }

  mirnext::Function &left_function = module.new_function(
      "left", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Function &right_function = module.new_function(
      "right", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder left_builder(left_function);
  mirnext::IRBuilder right_builder(right_function);
  mirnext::Label left_entry = left_builder.entry();
  mirnext::Label right_entry = right_builder.entry();
  mirnext::Value cross = left_entry.arg("arg1") + right_entry.arg("arg1");
  if (cross.is_valid()) return 76;
  if (left_builder.ok() || left_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 77;
  }

  mirnext::Function &late_entry_function = module.new_function("late_entry", {}, {});
  mirnext::IRBuilder late_entry_builder(late_entry_function);
  mirnext::Label late_entry = late_entry_builder.entry();
  mirnext::Label late_body = late_entry_builder.label();
  late_body.ret();
  if (late_entry_builder.ok()
      || late_entry_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 80;
  }
  late_entry.ret();
  if (late_entry_function.instruction_count() != 0) return 81;

  mirnext::Function &closed_function = module.new_function("closed", {}, {});
  mirnext::IRBuilder closed_builder(closed_function);
  mirnext::Label closed_entry = closed_builder.entry();
  mirnext::Label closed_next = closed_builder.label();
  closed_entry.ret();
  closed_next.ret();
  closed_entry.ret();
  if (closed_builder.ok()
      || closed_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 82;
  }

  mirnext::Function &cond_function = module.new_function(
      "cond_label", {}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder cond_builder(cond_function);
  mirnext::Label cond_entry = cond_builder.entry();
  mirnext::Label cond_other = cond_builder.label();
  mirnext::Value cond_arg = cond_entry.arg("arg1");
  mirnext::Cond cond = cond_arg >= cond_entry.i64(0);
  cond_other.if_(cond, cond_entry);
  if (cond_builder.ok() || cond_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 83;
  }

  mirnext::Function &target_left_function = module.new_function("target_left", {}, {});
  mirnext::Function &target_right_function = module.new_function("target_right", {}, {});
  mirnext::IRBuilder target_left_builder(target_left_function);
  mirnext::IRBuilder target_right_builder(target_right_function);
  mirnext::Label target_left_entry = target_left_builder.entry();
  mirnext::Label target_right_entry = target_right_builder.entry();
  target_left_entry.jmp(target_right_entry);
  if (target_left_builder.ok()
      || target_left_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 84;
  }

  return 0;
}

template <class MakeValue>
static int add_integer_dsl_case(mirnext::Module &module, const char *name, mirnext::Type type,
                                MakeValue make_value) {
  mirnext::Function &function = module.new_function(name, {type}, {{type, "arg"}});
  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Label done = builder.label();
  mirnext::Value arg = entry.arg("arg");
  mirnext::Value result = (arg / make_value(entry, 2)) % make_value(entry, 3);
  entry.if_(result < make_value(entry, 4), done);
  entry.if_(result >= make_value(entry, 5), done);
  entry.if_(result == make_value(entry, 6), done);
  entry.if_(result != make_value(entry, 7), done);
  done.ret(result);
  return builder.ok() ? 0 : 90;
}

template <class MakeValue>
static int add_float_dsl_case(mirnext::Module &module, const char *name, mirnext::Type type,
                              MakeValue make_value) {
  mirnext::Function &function = module.new_function(name, {type}, {{type, "arg"}});
  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Label done = builder.label();
  mirnext::Value arg = entry.arg("arg");
  mirnext::Value result
      = (((arg + make_value(entry, 1)) - make_value(entry, 2)) * make_value(entry, 3))
        / make_value(entry, 4);
  entry.if_(result == make_value(entry, 1), done);
  entry.if_(result != make_value(entry, 2), done);
  entry.if_(result < make_value(entry, 3), done);
  entry.if_(result <= make_value(entry, 4), done);
  entry.if_(result > make_value(entry, 5), done);
  entry.if_(result >= make_value(entry, 6), done);
  done.ret(result);
  return builder.ok() ? 0 : 91;
}

static int check_numeric_label_dsl() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("numeric_dsl");

  mirnext::Function &literals = module.new_function("literals", {}, {});
  mirnext::IRBuilder literal_builder(literals);
  mirnext::Label literal_entry = literal_builder.entry();
  literal_entry.ret({literal_entry.i8(-8), literal_entry.u8(250), literal_entry.i16(-1600),
                     literal_entry.u16(65000), literal_entry.i32(-32000),
                     literal_entry.u32(4000000000u), literal_entry.i64(-64000),
                     literal_entry.u64(std::numeric_limits<std::uint64_t>::max()),
                     literal_entry.f32(1.25f), literal_entry.f64(2.5),
                     literal_entry.ld(3.75L)});
  if (!literal_builder.ok()) return 92;

  if (add_integer_dsl_case(module, "dsl_i8", mirnext::Type::i8(),
                           [](mirnext::Label label, int value) {
                             return label.i8(static_cast<std::int8_t>(value));
                           })) {
    return 93;
  }
  if (add_integer_dsl_case(module, "dsl_u8", mirnext::Type::u8(),
                           [](mirnext::Label label, int value) {
                             return label.u8(static_cast<std::uint8_t>(value));
                           })) {
    return 94;
  }
  if (add_integer_dsl_case(module, "dsl_i16", mirnext::Type::i16(),
                           [](mirnext::Label label, int value) {
                             return label.i16(static_cast<std::int16_t>(value));
                           })) {
    return 95;
  }
  if (add_integer_dsl_case(module, "dsl_u16", mirnext::Type::u16(),
                           [](mirnext::Label label, int value) {
                             return label.u16(static_cast<std::uint16_t>(value));
                           })) {
    return 96;
  }
  if (add_integer_dsl_case(module, "dsl_i32", mirnext::Type::i32(),
                           [](mirnext::Label label, int value) {
                             return label.i32(static_cast<std::int32_t>(value));
                           })) {
    return 97;
  }
  if (add_integer_dsl_case(module, "dsl_u32", mirnext::Type::u32(),
                           [](mirnext::Label label, int value) {
                             return label.u32(static_cast<std::uint32_t>(value));
                           })) {
    return 98;
  }
  if (add_integer_dsl_case(module, "dsl_i64", mirnext::Type::i64(),
                           [](mirnext::Label label, int value) {
                             return label.i64(static_cast<std::int64_t>(value));
                           })) {
    return 99;
  }
  if (add_integer_dsl_case(module, "dsl_u64", mirnext::Type::u64(),
                           [](mirnext::Label label, int value) {
                             return label.u64(static_cast<std::uint64_t>(value));
                           })) {
    return 100;
  }
  if (add_float_dsl_case(module, "dsl_f32", mirnext::Type::f(),
                         [](mirnext::Label label, int value) {
                           return label.f32(static_cast<float>(value));
                         })) {
    return 101;
  }
  if (add_float_dsl_case(module, "dsl_f64", mirnext::Type::d(),
                         [](mirnext::Label label, int value) {
                           return label.f64(static_cast<double>(value));
                         })) {
    return 102;
  }
  if (add_float_dsl_case(module, "dsl_ld", mirnext::Type::ld(),
                         [](mirnext::Label label, int value) {
                           return label.ld(static_cast<long double>(value));
                         })) {
    return 103;
  }

  mirnext::Function &explicit_block = module.new_function("explicit_block", {mirnext::Type::i64()},
                                                          {});
  mirnext::IRBuilder explicit_builder(explicit_block);
  mirnext::Label explicit_entry = explicit_builder.entry();
  mirnext::Label explicit_body = explicit_builder.label();
  mirnext::Value a = explicit_entry.local(mirnext::Type::i64(), "a");
  mirnext::Value b = explicit_entry.local(mirnext::Type::i64(), "b");
  explicit_entry.assign(a, explicit_entry.i64(1));
  explicit_entry.assign(b, explicit_entry.i64(2));
  explicit_entry.jmp(explicit_body);
  explicit_body.begin();
  explicit_body.ret(a + b);
  explicit_body.end();
  if (!explicit_builder.ok()) return 89;

  mirnext::Function &var_assignment = module.new_function("var_assignment",
                                                          {mirnext::Type::i64()}, {});
  mirnext::IRBuilder var_builder(var_assignment);
  mirnext::Label var_entry = var_builder.entry();
  mirnext::Label var_body = var_builder.label();
  mirnext::Label var_done = var_builder.label();
  mirnext::Var i = var_entry.local(mirnext::Type::i64(), "i");
  mirnext::Var sum = var_entry.local(mirnext::Type::i64(), "sum");
  mirnext::Value compatible = var_entry.local(mirnext::Type::i64(), "compatible");
  mirnext::Var ptr = var_entry.local(mirnext::Type::p(), "ptr");
  if (!i.is_valid() || i.type() != mirnext::Type::i64() || !i.value().is_valid()) return 174;
  if (!compatible.is_valid() || compatible.type() != mirnext::Type::i64()) return 175;
  var_entry.begin();
  i = 1;
  sum = 0;
  ptr = 0;
  var_entry.jmp(var_body);
  var_entry.end();
  var_body.begin();
  sum = sum + i;
  var_body.if_(sum > compatible, var_done);
  var_body.jmp(var_done);
  var_body.end();
  var_done.begin();
  var_done.ret(sum);
  var_done.end();
  if (!var_builder.ok()) return 176;

  mirnext::Function &float_mod_function = module.new_function("float_mod", {}, {});
  mirnext::IRBuilder float_mod_builder(float_mod_function);
  mirnext::Label float_mod_entry = float_mod_builder.entry();
  mirnext::Value bad_mod = float_mod_entry.f32(1.0f) % float_mod_entry.f32(2.0f);
  if (bad_mod.is_valid() || float_mod_builder.ok()
      || float_mod_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 104;
  }

  mirnext::Function &pointer_function = module.new_function("pointer_bad", {}, {});
  mirnext::IRBuilder pointer_builder(pointer_function);
  mirnext::Label pointer_entry = pointer_builder.entry();
  mirnext::Value bad_pointer = pointer_entry.local(mirnext::Type::p(), "a")
                               + pointer_entry.local(mirnext::Type::p(), "b");
  if (bad_pointer.is_valid() || pointer_builder.ok()
      || pointer_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 105;
  }

  mirnext::Function &mixed_function = module.new_function("mixed_bad", {}, {});
  mirnext::IRBuilder mixed_builder(mixed_function);
  mirnext::Label mixed_entry = mixed_builder.entry();
  mirnext::Value bad_mixed = mixed_entry.i32(1) + mixed_entry.i64(2);
  if (bad_mixed.is_valid() || mixed_builder.ok()
      || mixed_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 106;
  }

  mirnext::Function &first_error_function = module.new_function("first_error", {}, {});
  mirnext::IRBuilder first_error_builder(first_error_function);
  mirnext::Label first_error_entry = first_error_builder.entry();
  (void) (first_error_entry.f64(1.0) % first_error_entry.f64(2.0));
  first_error_entry.arg("missing");
  if (first_error_builder.ok()
      || first_error_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 107;
  }

  mirnext::Function &bad_var_type = module.new_function("bad_var_type", {}, {});
  mirnext::IRBuilder bad_var_type_builder(bad_var_type);
  mirnext::Label bad_var_type_entry = bad_var_type_builder.entry();
  mirnext::Var bad_var_dst = bad_var_type_entry.local(mirnext::Type::i64(), "dst");
  bad_var_dst = bad_var_type_entry.u64(1);
  if (bad_var_type_builder.ok()
      || bad_var_type_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 177;
  }

  mirnext::Function &bad_float_literal = module.new_function("bad_float_literal", {}, {});
  mirnext::IRBuilder bad_float_literal_builder(bad_float_literal);
  mirnext::Label bad_float_literal_entry = bad_float_literal_builder.entry();
  mirnext::Var bad_float_dst = bad_float_literal_entry.local(mirnext::Type::d(), "dst");
  bad_float_dst = 1;
  if (bad_float_literal_builder.ok()
      || bad_float_literal_builder.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 178;
  }

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "ret -8 250 -1600 65000 -32000 4000000000 -64000 "
                      "18446744073709551615 1.25f 2.5d 3.75ld")) {
    return 108;
  }
  if (!contains(text, "func dsl_i8(i8 %arg) -> i8")
      || !contains(text, "func dsl_u8(u8 %arg) -> u8")
      || !contains(text, "func dsl_i16(i16 %arg) -> i16")
      || !contains(text, "func dsl_u16(u16 %arg) -> u16")
      || !contains(text, "func dsl_i32(i32 %arg) -> i32")
      || !contains(text, "func dsl_u32(u32 %arg) -> u32")
      || !contains(text, "func dsl_i64(i64 %arg) -> i64")
      || !contains(text, "func dsl_u64(u64 %arg) -> u64")) {
    return 109;
  }
  if (!contains(text, "div %.t0 %arg 2") || !contains(text, "mod %.t1 %.t0 3")
      || !contains(text, "blt L") || !contains(text, "bge L")
      || !contains(text, "beq L") || !contains(text, "bne L")) {
    return 110;
  }
  if (!contains(text, "udiv %.t0 %arg 2") || !contains(text, "umod %.t1 %.t0 3")
      || !contains(text, "ublt L") || !contains(text, "ubge L")) {
    return 111;
  }
  if (!contains(text, "fadd %.t0 %arg 1f") || !contains(text, "fsub %.t1 %.t0 2f")
      || !contains(text, "fmul %.t2 %.t1 3f") || !contains(text, "fdiv %.t3 %.t2 4f")
      || !contains(text, "fbeq L") || !contains(text, "fbne L")
      || !contains(text, "fblt L") || !contains(text, "fble L")
      || !contains(text, "fbgt L") || !contains(text, "fbge L")) {
    return 112;
  }
  if (!contains(text, "dadd %.t0 %arg 1d") || !contains(text, "dsub %.t1 %.t0 2d")
      || !contains(text, "dmul %.t2 %.t1 3d") || !contains(text, "ddiv %.t3 %.t2 4d")
      || !contains(text, "dbeq L") || !contains(text, "dbne L")
      || !contains(text, "dblt L") || !contains(text, "dble L")
      || !contains(text, "dbgt L") || !contains(text, "dbge L")) {
    return 113;
  }
  if (!contains(text, "ldadd %.t0 %arg 1ld") || !contains(text, "ldsub %.t1 %.t0 2ld")
      || !contains(text, "ldmul %.t2 %.t1 3ld") || !contains(text, "lddiv %.t3 %.t2 4ld")
      || !contains(text, "ldbeq L") || !contains(text, "ldbne L")
      || !contains(text, "ldblt L") || !contains(text, "ldble L")
      || !contains(text, "ldbgt L") || !contains(text, "ldbge L")) {
    return 114;
  }
  if (!contains(text, "func var_assignment() -> i64") || !contains(text, "reg i64 %i")
      || !contains(text, "reg i64 %sum") || !contains(text, "reg i64 %compatible")
      || !contains(text, "reg p %ptr") || !contains(text, "mov %i 1")
      || !contains(text, "mov %sum 0") || !contains(text, "mov %ptr 0")
      || !contains(text, "add %.t0 %sum %i") || !contains(text, "bgt L")) {
    return 179;
  }

  return 0;
}

static int check_call_label_dsl() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("call_dsl");
  mirnext::Prototype &single_proto = module.new_prototype(
      "single_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Prototype &multi_proto = module.new_prototype(
      "multi_p", {mirnext::Type::i64(), mirnext::Type::d()},
      {{mirnext::Type::i64(), "arg"}});
  mirnext::Prototype &void_proto = module.new_prototype(
      "void_p", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Import &single_import = module.new_import("single_import");
  mirnext::Import &void_import = module.new_import("void_import");

  mirnext::Function &import_caller = module.new_function(
      "import_caller", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder import_builder(import_caller);
  mirnext::Label import_entry = import_builder.entry();
  mirnext::Value import_arg = import_entry.arg("arg");
  mirnext::Value import_result = import_entry.call(single_proto, single_import, {import_arg});
  if (!import_result.is_valid() || import_result.type() != mirnext::Type::i64()) return 115;
  if (import_result.builder() != &import_builder
      || import_result.operand().kind() != mirnext::Operand::Kind::Register) {
    return 116;
  }
  import_entry.ret(import_result);
  if (!import_builder.ok()) return 117;

  mirnext::Function &callee = module.new_function(
      "single_callee", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder callee_builder(callee);
  mirnext::Label callee_entry = callee_builder.entry();
  mirnext::Value callee_arg = callee_entry.arg("arg");
  callee_entry.ret(callee_arg + callee_entry.i64(1));
  if (!callee_builder.ok()) return 118;

  mirnext::Function &function_caller = module.new_function(
      "function_caller", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder function_builder(function_caller);
  mirnext::Label function_entry = function_builder.entry();
  mirnext::Value function_result
      = function_entry.call(single_proto, callee, {function_entry.arg("arg")});
  function_entry.ret(function_result);
  if (!function_builder.ok()) return 119;

  mirnext::Function &forward_callee = module.new_function(
      "forward_callee", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &forward_caller = module.new_function(
      "forward_caller", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder forward_caller_builder(forward_caller);
  mirnext::Label forward_caller_entry = forward_caller_builder.entry();
  mirnext::Value forward_result
      = forward_caller_entry.call(single_proto, forward_callee,
                                  {forward_caller_entry.arg("arg")});
  forward_caller_entry.ret(forward_result);
  if (!forward_caller_builder.ok()) return 120;
  mirnext::IRBuilder forward_callee_builder(forward_callee);
  mirnext::Label forward_callee_entry = forward_callee_builder.entry();
  forward_callee_entry.ret(forward_callee_entry.arg("arg"));
  if (!forward_callee_builder.ok()) return 121;

  mirnext::Function &multi_caller = module.new_function(
      "multi_caller", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder multi_builder(multi_caller);
  mirnext::Label multi_entry = multi_builder.entry();
  mirnext::CallResult multi_results
      = multi_entry.call_multi(multi_proto, single_import, {multi_entry.arg("arg")});
  if (multi_results.size() != 2 || multi_results[0].type() != mirnext::Type::i64()
      || multi_results[1].type() != mirnext::Type::d()) {
    return 122;
  }
  multi_entry.ret();
  if (!multi_builder.ok()) return 123;

  mirnext::Function &void_caller = module.new_function(
      "void_caller", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder void_builder(void_caller);
  mirnext::Label void_entry = void_builder.entry();
  void_entry.call_void(void_proto, void_import, {void_entry.arg("arg")});
  void_entry.ret();
  if (!void_builder.ok()) return 124;

  mirnext::Function &multi_void_caller = module.new_function(
      "multi_void_caller", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::IRBuilder multi_void_builder(multi_void_caller);
  mirnext::Label multi_void_entry = multi_void_builder.entry();
  mirnext::CallResult no_results
      = multi_void_entry.call_multi(void_proto, void_import, {multi_void_entry.arg("arg")});
  if (!no_results.empty()) return 125;
  multi_void_entry.ret();
  if (!multi_void_builder.ok()) return 126;

  auto expect_invalid_operand = [](mirnext::IRBuilder &builder) {
    return !builder.ok() && builder.error().code == mirnext::ErrorCode::InvalidOperand;
  };

  {
    mirnext::Function &function = module.new_function("bad_single_void", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Value result = entry.call(void_proto, void_import, {entry.i64(1)});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 127;
  }
  {
    mirnext::Function &function = module.new_function("bad_single_multi", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Value result = entry.call(multi_proto, single_import, {entry.i64(1)});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 128;
  }
  {
    mirnext::Function &function = module.new_function("bad_void_return", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    entry.call_void(single_proto, single_import, {entry.i64(1)});
    if (!expect_invalid_operand(builder)) return 129;
  }
  {
    mirnext::Function &function = module.new_function("bad_arg_count", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Value result = entry.call(single_proto, single_import, {});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 130;
  }
  {
    mirnext::Function &function = module.new_function("bad_arg_type", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Value result = entry.call(single_proto, single_import, {entry.u64(1)});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 131;
  }
  {
    mirnext::Function &function = module.new_function("bad_invalid_arg", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Value result = entry.call(single_proto, single_import, {mirnext::Value()});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 132;
  }
  {
    mirnext::Function &left = module.new_function("bad_cross_builder_left", {}, {});
    mirnext::Function &right = module.new_function("bad_cross_builder_right", {}, {});
    mirnext::IRBuilder left_builder(left);
    mirnext::IRBuilder right_builder(right);
    mirnext::Label left_entry = left_builder.entry();
    mirnext::Label right_entry = right_builder.entry();
    mirnext::Value result = left_entry.call(single_proto, single_import, {right_entry.i64(1)});
    if (result.is_valid() || !expect_invalid_operand(left_builder)) return 133;
  }
  {
    mirnext::Function &function = module.new_function("bad_cross_label", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    mirnext::Label other = builder.label();
    mirnext::Value result = entry.call(single_proto, single_import, {other.i64(1)});
    if (result.is_valid() || !expect_invalid_operand(builder)) return 134;
  }
  {
    mirnext::Function &function = module.new_function("bad_first_error", {}, {});
    mirnext::IRBuilder builder(function);
    mirnext::Label entry = builder.entry();
    (void) (entry.f64(1.0) % entry.f64(2.0));
    (void) entry.call(single_proto, single_import, {});
    if (builder.ok() || builder.error().code != mirnext::ErrorCode::InvalidOperand) return 135;
  }

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "call @single_p @single_import %.t0 %arg")) return 136;
  if (!contains(text, "call @single_p @single_callee %.t0 %arg")) return 137;
  if (!contains(text, "call @single_p @forward_callee %.t0 %arg")) return 138;
  if (!contains(text, "call @multi_p @single_import %.t0 %.t1 %arg")) return 139;
  if (!contains(text, "call @void_p @void_import %arg")) return 140;

  return 0;
}

static int check_builder_memory_dsl() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("memory");
  mirnext::Function &function = module.new_function("memory_ops", {mirnext::Type::u8()}, {});
  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Value flags = entry.alloca(16, "flags");
  mirnext::Value i = entry.local(mirnext::Type::i64(), "i");
  entry.assign(i, entry.i64(3));
  mirnext::Memory slot = entry.mem(mirnext::Type::u8(), flags, i);
  entry.store(slot, entry.u8(7));
  mirnext::Value loaded = entry.load(slot);
  entry.ret(loaded);
  if (!builder.ok()) return 145;
  if (!flags.is_valid() || flags.type() != mirnext::Type::p()) return 146;
  if (!slot.is_valid() || slot.type() != mirnext::Type::u8()) return 147;
  if (!loaded.is_valid() || loaded.type() != mirnext::Type::u8()) return 148;
  if (loaded.operand().kind() != mirnext::Operand::Kind::Register) return 149;
  if (function.local_registers().size() != 3) return 150;
  if (function.instruction_count() != 6) return 151;

  const auto &instructions = function.instructions();
  if (instructions[1]->opcode() != mirnext::Opcode::Alloca) return 152;
  if (instructions[2]->opcode() != mirnext::Opcode::Mov
      || instructions[2]->operands()[0].kind() != mirnext::Operand::Kind::Register) {
    return 153;
  }
  if (instructions[3]->opcode() != mirnext::Opcode::Mov
      || instructions[3]->operands()[0].kind() != mirnext::Operand::Kind::Memory) {
    return 154;
  }
  if (instructions[4]->opcode() != mirnext::Opcode::Mov
      || instructions[4]->operands()[1].kind() != mirnext::Operand::Kind::Memory) {
    return 155;
  }

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "reg p %flags")) return 164;
  if (!contains(text, "alloca %flags 16")) return 156;
  if (!contains(text, "u8:(%flags, %i, 1)")) return 157;
  if (!contains(text, "mov %i 3")) return 158;

  {
    mirnext::Function &typed = module.new_function("typed_alloca", {}, {});
    mirnext::IRBuilder typed_builder(typed);
    mirnext::Label typed_entry = typed_builder.entry();
    mirnext::Value pair = typed_entry.alloca(mirnext::Type::i64(), 2, "pair");
    mirnext::Value single = typed_entry.alloca(mirnext::Type::i64(), "single");
    typed_entry.ret();
    if (!typed_builder.ok()) return 167;
    if (!pair.is_valid() || pair.type() != mirnext::Type::p()) return 168;
    if (!single.is_valid() || single.type() != mirnext::Type::p()) return 169;

    std::ostringstream typed_out;
    ctx.dump(typed_out);
    const std::string typed_text = typed_out.str();
    if (!contains(typed_text, "alloca %pair 16")) return 170;
    if (!contains(typed_text, "alloca %single 8")) return 171;
  }

  auto expect_invalid_operand = [](mirnext::IRBuilder &bad_builder) {
    return !bad_builder.ok() && bad_builder.error().code == mirnext::ErrorCode::InvalidOperand;
  };
  auto expect_invalid_argument = [](mirnext::IRBuilder &bad_builder) {
    return !bad_builder.ok() && bad_builder.error().code == mirnext::ErrorCode::InvalidArgument;
  };

  {
    mirnext::Function &bad = module.new_function("bad_alloca_size", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value allocated = bad_entry.alloca(-1, "bad");
    if (allocated.is_valid() || !expect_invalid_argument(bad_builder)) return 165;
  }
  {
    mirnext::Function &bad = module.new_function("bad_alloca_count", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value allocated = bad_entry.alloca(mirnext::Type::i64(), -1, "bad");
    if (allocated.is_valid() || !expect_invalid_argument(bad_builder)) return 172;
  }
  {
    mirnext::Function &bad = module.new_function("bad_alloca_overflow", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value allocated = bad_entry.alloca(
        mirnext::Type::i64(), std::numeric_limits<std::int64_t>::max(), "bad");
    if (allocated.is_valid() || !expect_invalid_argument(bad_builder)) return 173;
  }
  {
    mirnext::Function &bad = module.new_function("bad_alloca_duplicate", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value first = bad_entry.alloca(1, "slot");
    mirnext::Value second = bad_entry.alloca(2, "slot");
    if (!first.is_valid() || second.is_valid() || bad_builder.ok()
        || bad_builder.error().code != mirnext::ErrorCode::DuplicateName) {
      return 166;
    }
  }
  {
    mirnext::Function &bad = module.new_function("bad_literal_lvalue", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    bad_entry.assign(bad_entry.i64(0), bad_entry.i64(1));
    if (!expect_invalid_operand(bad_builder)) return 159;
  }
  {
    mirnext::Function &bad = module.new_function("bad_cross_builder_memory", {}, {});
    mirnext::Function &other = module.new_function("bad_cross_builder_memory_other", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::IRBuilder other_builder(other);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Label other_entry = other_builder.entry();
    mirnext::Value base = other_entry.local(mirnext::Type::i64(), "base");
    mirnext::Value index = bad_entry.local(mirnext::Type::i64(), "index");
    (void) bad_entry.mem(mirnext::Type::u8(), base, index);
    if (!expect_invalid_operand(bad_builder)) return 160;
  }
  {
    mirnext::Function &bad = module.new_function("bad_cross_label_memory", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Label other = bad_builder.label();
    mirnext::Value base = bad_entry.local(mirnext::Type::i64(), "base");
    mirnext::Value index = bad_entry.local(mirnext::Type::i64(), "index");
    mirnext::Memory memory = bad_entry.mem(mirnext::Type::u8(), base, index);
    other.load(memory);
    if (!expect_invalid_operand(bad_builder)) return 161;
  }
  {
    mirnext::Function &bad = module.new_function("bad_store_type", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value base = bad_entry.local(mirnext::Type::i64(), "base");
    mirnext::Value index = bad_entry.local(mirnext::Type::i64(), "index");
    bad_entry.store(bad_entry.mem(mirnext::Type::u8(), base, index), bad_entry.i64(1));
    if (!expect_invalid_operand(bad_builder)) return 162;
  }
  {
    mirnext::Function &bad = module.new_function("bad_scale", {}, {});
    mirnext::IRBuilder bad_builder(bad);
    mirnext::Label bad_entry = bad_builder.entry();
    mirnext::Value base = bad_entry.local(mirnext::Type::i64(), "base");
    mirnext::Value index = bad_entry.local(mirnext::Type::i64(), "index");
    (void) bad_entry.mem(mirnext::Type::u8(), base, index, 3);
    if (!expect_invalid_argument(bad_builder)) return 163;
  }

  return 0;
}

static int check_binary_encode_api() {
  mirnext::Context ctx;
  mirnext::Module &empty = ctx.new_module("binary_empty");
  mirnext::Result<std::vector<std::byte>> empty_bytes = empty.encode_binary();
  if (!empty_bytes || empty_bytes->empty()) return 141;

  mirnext::Module &bad = ctx.new_module("binary_bad");
  mirnext::Function &nop = bad.new_function("nop", {}, {});
  nop.append(mirnext::Instruction(mirnext::Opcode::Nop));
  mirnext::Result<std::vector<std::byte>> bad_bytes = bad.encode_binary();
  if (bad_bytes || bad_bytes.error().code != mirnext::ErrorCode::InvalidArgument) return 142;

  mirnext::Module &left = ctx.new_module("binary_left");
  mirnext::Module &right = ctx.new_module("binary_right");
  mirnext::Prototype &prototype = left.new_prototype(
      "p", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &foreign = right.new_function(
      "foreign", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Function &caller = left.new_function(
      "caller", {}, {{mirnext::Type::i64(), "arg"}});
  mirnext::Register arg = expect(caller.argument("arg"), 143);
  caller.append(mirnext::Instruction(mirnext::Opcode::Call,
                                     {mirnext::Operand::ref(prototype),
                                      mirnext::Operand::ref(foreign), arg}));
  caller.append_ret();
  mirnext::Result<std::vector<std::byte>> cross_bytes = left.encode_binary();
  if (cross_bytes || cross_bytes.error().code != mirnext::ErrorCode::InvalidOperand) {
    return 144;
  }

  return 0;
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

  mirnext::Result<mirnext::Register> arg1_result = function.argument("arg1");
  if (!arg1_result || !arg1_result.has_value() || arg1_result.has_error()) return 59;
  if (arg1_result.value_ptr() == nullptr || arg1_result.error_ptr() != nullptr) return 60;
  if ((*arg1_result).name() != "arg1" || arg1_result->name() != "arg1") return 61;

  if (missing.value_ptr() != nullptr || missing.error_ptr() == nullptr || !missing.has_error()) {
    return 62;
  }

  mirnext::Result<int> try_ok = try_success();
  if (!try_ok || *try_ok != 8) return 63;

  mirnext::Result<int> try_bad = try_failure();
  if (try_bad || try_bad.error().code != mirnext::ErrorCode::InvalidArgument
      || try_bad.error().message != "expected failure") {
    return 64;
  }

  mirnext::Result<int> try_assign_ok = try_assign_success();
  if (!try_assign_ok || try_assign_ok.value() != 9) return 65;

  if (int code = check_builder_errors()) return code;
  if (int code = check_numeric_label_dsl()) return code;
  if (int code = check_call_label_dsl()) return code;
  if (int code = check_builder_memory_dsl()) return code;
  if (int code = check_binary_encode_api()) return code;

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Label fin = builder.label();
  mirnext::Label cont = builder.label();
  if (!builder.ok() || !entry.is_valid() || !fin.is_valid() || !cont.is_valid()) return 22;

  function.append(mirnext::Instruction(mirnext::Opcode::Mov, {count, mirnext::Operand::int64(0)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Neg, {tmp, count}));
  function.append(mirnext::Instruction(mirnext::Opcode::Sub, {tmp, tmp, mirnext::Operand::int64(1)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Mul, {tmp, tmp, mirnext::Operand::int64(2)}));
  function.append(mirnext::Instruction(mirnext::Opcode::UDiv, {tmp, tmp, mirnext::Operand::int64(2)}));
  function.append(mirnext::Instruction(mirnext::Opcode::UMod, {tmp, tmp, mirnext::Operand::int64(7)}));
  function.append(mirnext::Instruction(mirnext::Opcode::And, {tmp, tmp, mirnext::Operand::int64(15)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Or, {tmp, tmp, mirnext::Operand::int64(16)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Xor, {tmp, tmp, mirnext::Operand::int64(3)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Lsh, {tmp, tmp, mirnext::Operand::int64(1)}));
  function.append(mirnext::Instruction(mirnext::Opcode::URsh, {tmp, tmp, mirnext::Operand::int64(1)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Eq, {tmp, tmp, mirnext::Operand::int64(0)}));
  function.append(mirnext::Instruction(mirnext::Opcode::ULt, {tmp, tmp, mirnext::Operand::int64(10)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Addo, {tmp, tmp, mirnext::Operand::int64(1)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Bo, {fin}));
  function.append(mirnext::Instruction(mirnext::Opcode::Bt, {fin, tmp}));
  function.append(mirnext::Instruction(mirnext::Opcode::Bne, {fin, tmp, mirnext::Operand::int64(0)}));
  function.append(mirnext::Instruction(mirnext::Opcode::UBgt, {fin, tmp, mirnext::Operand::int64(1)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Mov, {ptr, mirnext::Operand::int64(0)}));
  function.append(mirnext::Instruction(mirnext::Opcode::FMov, {fp, mirnext::Operand::float32(1.25f)}));
  function.append(mirnext::Instruction(mirnext::Opcode::DMov, {dp, mirnext::Operand::float64(2.5)}));
  function.append(mirnext::Instruction(mirnext::Opcode::LDMov, {ldp, mirnext::Operand::long_double(3.75L)}));
  function.append(mirnext::Instruction(mirnext::Opcode::Call,
                                       {mirnext::Operand::ref(prototype), mirnext::Operand::ref(import),
                                        tmp, count}));
  function.append(mirnext::Instruction(mirnext::Opcode::Call,
                                       {mirnext::Operand::ref(prototype), mirnext::Operand::ref(function),
                                        tmp, count}));

  mirnext::Value arg = entry.arg("arg1");
  mirnext::Value counter = cont.local(mirnext::Type::i64(), "dsl_count");
  mirnext::Value result = (arg + entry.i64(1)) * entry.i64(3);
  if (!result.is_valid() || result.type() != mirnext::Type::i64()) return 23;
  if (result.builder() != &builder || result.operand().kind() != mirnext::Operand::Kind::Register) {
    return 24;
  }
  entry.if_(result >= entry.i64(42), fin);
  mirnext::Value next = counter + cont.i64(1);
  cont.if_(next < cont.i64(42), cont);
  fin.ret(result);
  if (!builder.ok()) return 25;

  if (module.name() != "m") return 1;
  if (module.prototypes().size() != 1) return 51;
  if (module.imports().size() != 1) return 52;
  if (function.name() != "loop") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 1) return 4;
  if (function.local_registers().size() != 10) return 5;
  if (function.instruction_count() != 33) return 6;

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
  if (!contains(text, "reg i64 %dsl_count")) return 26;
  if (!contains(text, "reg i64 %.t0")) return 27;
  if (!contains(text, "reg i64 %.t1")) return 28;
  if (!contains(text, "reg i64 %.t2")) return 29;
  if (!contains(text, "mov %count 0")) return 10;
  if (!contains(text, "add %.t0 %arg1 1")) return 11;
  if (!contains(text, "mul %.t1 %.t0 3")) return 12;
  if (!contains(text, "label L1")) return 13;
  if (!contains(text, "bge L2 %.t1 42")) return 14;
  if (!contains(text, "label L3")) return 66;
  if (!contains(text, "add %.t2 %dsl_count 1")) return 15;
  if (!contains(text, "blt L3 %.t2 42")) return 16;
  if (!contains(text, "label L2")) return 30;
  if (!contains(text, "ret %.t1")) return 33;
  if (!contains(text, "neg %tmp %count")) return 34;
  if (!contains(text, "udiv %tmp %tmp 2")) return 35;
  if (!contains(text, "umod %tmp %tmp 7")) return 36;
  if (!contains(text, "and %tmp %tmp 15")) return 37;
  if (!contains(text, "or %tmp %tmp 16")) return 38;
  if (!contains(text, "xor %tmp %tmp 3")) return 39;
  if (!contains(text, "lsh %tmp %tmp 1")) return 40;
  if (!contains(text, "ursh %tmp %tmp 1")) return 41;
  if (!contains(text, "eq %tmp %tmp 0")) return 42;
  if (!contains(text, "ult %tmp %tmp 10")) return 43;
  if (!contains(text, "addo %tmp %tmp 1")) return 44;
  if (!contains(text, "bo L2")) return 45;
  if (!contains(text, "bt L2 %tmp")) return 46;
  if (!contains(text, "bne L2 %tmp 0")) return 47;
  if (!contains(text, "ubgt L2 %tmp 1")) return 48;
  if (!contains(text, "fmov %fp 1.25f")) return 56;
  if (!contains(text, "dmov %dp 2.5d")) return 57;
  if (!contains(text, "ldmov %ldp 3.75ld")) return 58;
  if (!contains(text, "call @add1_p @add1 %tmp %count")) return 49;
  if (!contains(text, "call @add1_p @loop %tmp %count")) return 50;

  return 0;
}
