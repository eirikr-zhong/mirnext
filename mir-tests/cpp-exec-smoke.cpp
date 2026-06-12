/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) {
    FAIL("error " << code << ": " << result.error().message);
    std::abort();
  }
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

static std::int64_t add7(std::int64_t value) { return value + 7; }

struct BinaryInput {
  const std::byte *data = nullptr;
  std::size_t size = 0;
  std::size_t pos = 0;
};

static BinaryInput *current_binary_input = nullptr;

static int binary_reader(MIR_context_t) {
  if (current_binary_input == nullptr || current_binary_input->pos >= current_binary_input->size) {
    return EOF;
  }
  return std::to_integer<int>(current_binary_input->data[current_binary_input->pos++]);
}

static mirnext::Result<mirnext::LegacyLoweredFunction>
read_binary_module(mirnext::LegacyContext &legacy, const mirnext::Module &module,
                   const char *function_name) {
  auto bytes_result = module.encode_binary();
  MIRNEXT_RESULT_RET(bytes_result);
  auto bytes = *bytes_result;
  BinaryInput input{bytes.data(), bytes.size(), 0};
  current_binary_input = &input;
  MIR_read_with_func(legacy.raw(), binary_reader);
  current_binary_input = nullptr;

  MIR_module_t decoded_module = nullptr;
  for (MIR_module_t current = DLIST_HEAD(MIR_module_t, *MIR_get_module_list(legacy.raw()));
       current != nullptr; current = DLIST_NEXT(MIR_module_t, current)) {
    if (std::strcmp(current->name, std::string(module.name()).c_str()) == 0) {
      decoded_module = current;
    }
  }
  if (decoded_module == nullptr) {
    return mirnext::Error{mirnext::ErrorCode::UnknownName, "decoded module not found"};
  }

  MIR_item_t decoded_function = nullptr;
  for (MIR_item_t item = DLIST_HEAD(MIR_item_t, decoded_module->items); item != nullptr;
       item = DLIST_NEXT(MIR_item_t, item)) {
    if (item->item_type == MIR_func_item && std::strcmp(MIR_item_name(legacy.raw(), item),
                                                        function_name)
                                                == 0) {
      decoded_function = item;
    }
  }
  if (decoded_function == nullptr) {
    return mirnext::Error{mirnext::ErrorCode::UnknownName, "decoded function not found"};
  }
  return mirnext::LegacyLoweredFunction{decoded_module, decoded_function};
}

static mirnext::Label &new_label(mirnext::Function &function, int code) {
  (void)code;
  return function.label();
}

static mirnext::Function &create_loop(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value arg1 = expect(function.arg("arg1"), 1);
  mirnext::Var count = expect(function.var(mirnext::Type::i64(), "count"), 2);

  mirnext::Label &fin = new_label(function, 3);
  mirnext::Label &cont = new_label(function, 4);

  function[count] = 0;
  function.if_(function[count] >= arg1, fin);
  function.jmp(cont);
  cont[count] = cont[count] + 1;
  cont.if_(cont[count] < arg1, cont);
  cont.jmp(fin);
  fin.ret(fin[count]);
  function.end();
  return function;
}

static mirnext::Result<mirnext::Function *> create_sum_to_n(mirnext::Context &ctx,
                                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_sum");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "sum_to_n", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});

  mirnext::Label &loop = function.label();
  mirnext::Label &body = function.label();
  mirnext::Label &done = function.label();

  mirnext::Value n = expect(function.arg("n"), 3);
  mirnext::Var i = expect(function.var(mirnext::Type::i64(), "i"), 4);
  mirnext::Var sum = expect(function.var(mirnext::Type::i64(), "sum"), 5);

  mirnext::Value d = expect(function.convert<mirnext::Type::Kind::D>(n), 6);
  mirnext::Value limit = expect(function.convert<mirnext::Type::Kind::I64>(d), 7);
  function[i] = 1;
  function[sum] = 0;
  expect_ok(function.jmp(loop), 12);

  loop.if_(loop[i] > limit, done);
  expect_ok(loop.jmp(body), 16);

  body[sum] = body[sum] + body[i];
  body[i] = body[i] + 1;
  expect_ok(body.jmp(loop), 24);

  done.ret(done[sum]);
  function.end();
  if (module.error()) return *module.error();
  return &function;
}

static mirnext::Function &create_sieve(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_sieve");
  *module_out = &module;
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::Label &loop = new_label(function, 18);
  mirnext::Label &loop2 = new_label(function, 19);
  mirnext::Label &loop3 = new_label(function, 20);
  mirnext::Label &loop4 = new_label(function, 21);
  mirnext::Label &fin = new_label(function, 22);
  mirnext::Label &fin2 = new_label(function, 23);
  mirnext::Label &fin3 = new_label(function, 24);
  mirnext::Label &fin4 = new_label(function, 25);
  mirnext::Label &cont3 = new_label(function, 26);

  mirnext::Value flags = expect(function.alloca(8190, "flags"), 27);
  mirnext::Var iter = expect(function.var(mirnext::Type::i64(), "iter"), 28);
  mirnext::Var count = expect(function.var(mirnext::Type::i64(), "count"), 29);
  mirnext::Var i = expect(function.var(mirnext::Type::i64(), "i"), 30);
  mirnext::Var k = expect(function.var(mirnext::Type::i64(), "k"), 31);
  mirnext::Var prime = expect(function.var(mirnext::Type::i64(), "prime"), 32);
  expect_ok(iter.assign(expect(function.i64(0), 33)), 34);
  expect_ok(function.jmp(loop), 35);

  mirnext::Value loop_iter = expect(loop.load(iter), 36);
  expect_ok(loop.if_(expect(loop_iter >= expect(loop.i64(100), 37), 38), fin), 39);
  expect_ok(loop.store(count, expect(loop.i64(0), 40)), 41);
  expect_ok(loop.store(i, expect(loop.i64(0), 42)), 43);
  expect_ok(loop.jmp(loop2), 44);

  mirnext::Value loop2_i = expect(loop2.load(i), 45);
  expect_ok(loop2.if_(expect(loop2_i >= expect(loop2.i64(8190), 46), 47), fin2), 48);
  expect_ok(loop2.store(expect(loop2.mem(mirnext::Type::u8(), flags, loop2_i), 49),
                        expect(loop2.u8(1), 50)),
            51);
  expect_ok(loop2.store(i, expect(loop2_i + expect(loop2.i64(1), 52), 53)), 54);
  expect_ok(loop2.jmp(loop2), 55);

  expect_ok(fin2.store(i, expect(fin2.i64(1), 56)), 57);
  expect_ok(fin2.jmp(loop3), 58);

  mirnext::Value loop3_i = expect(loop3.load(i), 59);
  expect_ok(loop3.if_(expect(loop3_i >= expect(loop3.i64(8190), 60), 61), fin3), 62);
  mirnext::Value flag = expect(loop3.load(expect(loop3.mem(mirnext::Type::u8(), flags, loop3_i), 63)),
                               64);
  expect_ok(loop3.if_(expect(flag == expect(loop3.u8(0), 65), 66), cont3), 67);
  mirnext::Value next_prime = expect(loop3_i + expect(loop3.i64(1), 68), 69);
  expect_ok(loop3.store(prime, next_prime), 70);
  expect_ok(loop3.store(k, expect(loop3_i + next_prime, 71)), 72);
  expect_ok(loop3.jmp(loop4), 73);

  mirnext::Value loop4_k = expect(loop4.load(k), 74);
  expect_ok(loop4.if_(expect(loop4_k >= expect(loop4.i64(8190), 75), 76), fin4), 77);
  expect_ok(loop4.store(expect(loop4.mem(mirnext::Type::u8(), flags, loop4_k), 78),
                        expect(loop4.u8(0), 79)),
            80);
  expect_ok(loop4.store(k, expect(loop4_k + expect(loop4.load(prime), 81), 82)), 83);
  expect_ok(loop4.jmp(loop4), 84);

  mirnext::Value fin4_count = expect(fin4.load(count), 85);
  expect_ok(fin4.store(count, expect(fin4_count + expect(fin4.i64(1), 86), 87)), 88);
  expect_ok(fin4.jmp(cont3), 89);

  mirnext::Value cont3_i = expect(cont3.load(i), 90);
  expect_ok(cont3.store(i, expect(cont3_i + expect(cont3.i64(1), 91), 92)), 93);
  expect_ok(cont3.jmp(loop3), 94);

  mirnext::Value fin3_iter = expect(fin3.load(iter), 95);
  expect_ok(fin3.store(iter, expect(fin3_iter + expect(fin3.i64(1), 96), 97)), 98);
  expect_ok(fin3.jmp(loop), 99);

  expect_ok(fin.ret(expect(fin.load(count), 100)), 101);
  expect_ok(function.end(), 102);
  return function;
}

static mirnext::Function &create_integer_ops(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_integer_ops");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "integer_ops", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value arg1 = expect(function.arg("arg1"), 72);
  mirnext::Value arithmetic = expect(expect(expect((arg1 - expect(function.i64(5), 73))
                                                * expect(function.i64(3), 74),
                                            75)
                                         / expect(function.i64(2), 76),
                                     77)
                                  % expect(function.i64(6), 78),
                              79);
  mirnext::Value masked = expect(arg1 & expect(function.i64(7), 80), 81);
  mirnext::Value ored = expect(masked | expect(function.i64(8), 82), 83);
  mirnext::Value xored = expect(ored ^ expect(function.i64(3), 84), 85);
  mirnext::Value shifted = expect((xored << expect(function.i64(1), 86))
                                      >> expect(function.i64(2), 87),
                                  88);
  mirnext::Value result = expect((-shifted) + arithmetic, 89);
  expect_ok(function.ret(result), 80);
  expect_ok(function.end(), 81);
  return function;
}

static mirnext::Function &create_integer_branch(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_integer_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "integer_branch", {mirnext::Type::i64()}, {{mirnext::Type::u64(), "arg1"}});
  mirnext::Value arg1 = expect(function.arg("arg1"), 93);

  mirnext::Label &unsigned_gt = new_label(function, 95);
  function.if_(arg1 > expect(function.u64(10), 98), unsigned_gt);
  function.ret(expect(function.i64(0), 99));
  unsigned_gt.ret(expect(unsigned_gt.i64(9), 101));
  function.end();
  return function;
}

static mirnext::Function &create_is_less(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_is_less");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "is_less", {mirnext::Type::b()}, {{mirnext::Type::i64(), "a"},
                                       {mirnext::Type::i64(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 102);
  mirnext::Value b = expect(function.arg("b"), 103);
  expect_ok(function.ret(expect(a < b, 104)), 105);
  expect_ok(function.end(), 106);
  return function;
}

static mirnext::Function &create_branch_on_bool_literal(mirnext::Context &ctx,
                                                        mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_branch_on_bool_literal");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "branch_on_bool_literal", {mirnext::Type::i64()}, {});
  mirnext::Label &target = new_label(function, 107);
  expect_ok(function.if_(expect(function.b(true), 108), target), 109);
  expect_ok(function.ret(expect(function.i64(0), 110)), 111);
  expect_ok(target.ret(expect(target.i64(1), 112)), 113);
  expect_ok(function.end(), 114);
  return function;
}

static mirnext::Function &create_checked_add_branch(mirnext::Context &ctx,
                                                    mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_checked_add");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "checked_add", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                             {mirnext::Type::i64(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 346).overflow(true);
  mirnext::Value b = expect(function.arg("b"), 347);
  mirnext::Label &overflow = new_label(function, 348);
  mirnext::Value sum = expect(a + b, 349);
  expect_ok(function.if_overflow(sum, overflow), 350);
  expect_ok(function.ret(sum), 351);
  expect_ok(overflow.ret(expect(overflow.i64(-1), 352)), 353);
  expect_ok(function.end(), 354);
  return function;
}

static mirnext::Function &create_checked_add_i32_branch(mirnext::Context &ctx,
                                                        mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_checked_add_i32");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "checked_add_i32", {mirnext::Type::i32()}, {{mirnext::Type::i32(), "a"},
                                                 {mirnext::Type::i32(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 355).overflow(true);
  mirnext::Value b = expect(function.arg("b"), 356);
  mirnext::Label &overflow = new_label(function, 357);
  mirnext::Value sum = expect(a + b, 358);
  expect_ok(function.if_overflow(sum, overflow), 359);
  expect_ok(function.ret(sum), 360);
  expect_ok(overflow.ret(expect(overflow.i32(-1), 361)), 362);
  expect_ok(function.end(), 363);
  return function;
}

static mirnext::Function &create_checked_chain_branch(mirnext::Context &ctx,
                                                      mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_checked_chain");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "checked_chain", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "a"},
                                               {mirnext::Type::i64(), "b"}});
  mirnext::Value a = expect(function.arg("a"), 364).overflow(true);
  mirnext::Value b = expect(function.arg("b"), 365);
  mirnext::Label &overflow = new_label(function, 366);
  mirnext::Value sum = expect(a + b, 367);
  mirnext::Value product = expect(sum * b, 368);
  expect_ok(function.if_overflow(product, overflow), 369);
  expect_ok(function.ret(product), 370);
  expect_ok(overflow.ret(expect(overflow.i64(-1), 371)), 372);
  expect_ok(function.end(), 373);
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

  mirnext::Value arg = expect(function.arg("arg1"), 115);
  mirnext::Value result = expect(function.call(prototype, import, {arg}), 116);
  expect_ok(function.ret(result), 117);
  expect_ok(function.end(), 118);
  return function;
}

static mirnext::Function &create_internal_call(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_internal_call");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "add11_p", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Function &callee = module.new_function(
      "add11", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value callee_arg = expect(callee.arg("arg1"), 119);
  expect_ok(callee.ret(expect(callee_arg + expect(callee.i64(11), 120), 121)), 122);
  expect_ok(callee.end(), 123);

  mirnext::Function &caller = module.new_function(
      "internal_call", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value arg = expect(caller.arg("arg1"), 124);
  mirnext::Value result = expect(caller.call(prototype, callee, {arg}), 125);
  expect_ok(caller.ret(result), 126);
  expect_ok(caller.end(), 127);
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

  mirnext::Value arg = expect(caller.arg("arg1"), 139);
  mirnext::Value result = expect(caller.call(prototype, callee, {arg}), 140);
  expect_ok(caller.ret(result), 141);
  expect_ok(caller.end(), 142);

  mirnext::Value callee_arg = expect(callee.arg("arg1"), 143);
  expect_ok(callee.ret(expect(callee_arg + expect(callee.i64(17), 144), 145)), 146);
  expect_ok(callee.end(), 147);
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
  mirnext::Value arg1 = expect(function.arg("arg1"), 147);
  mirnext::Label &base = new_label(function, 151);
  function.if_(arg1 <= expect(function.i64(0), 152), base);
  mirnext::Value next = expect(arg1 - expect(function.i64(1), 153), 154);
  mirnext::Value recursive = expect(function.call(prototype, function, {next}), 155);
  function.ret(expect(recursive + expect(function.i64(1), 156), 157));
  base.ret(expect(base.i64(0), 158));
  function.end();
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

  mirnext::Value first_arg = expect(first.arg("arg1"), 159);
  expect_ok(first.ret(expect(first.call(prototype, second, {first_arg}), 160)), 161);
  expect_ok(first.end(), 162);

  mirnext::Value second_arg = expect(second.arg("arg1"), 163);
  expect_ok(second.ret(expect(second.call(prototype, first, {second_arg}), 164)), 165);
  expect_ok(second.end(), 166);
  return first;
}

static mirnext::Function &create_double_arithmetic(mirnext::Context &ctx,
                                                   mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_arithmetic");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_arithmetic", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Value arg = expect(function.arg("arg1"), 180);
  mirnext::Value value = expect(
      expect(expect(expect(arg + expect(function.f64(2.0), 181), 182)
                   * expect(function.f64(3.0), 183),
                   184)
             / expect(function.f64(2.0), 185),
             186)
          - expect(function.f64(1.0), 187),
      188);
  expect_ok(function.ret(value), 189);
  expect_ok(function.end(), 190);
  return function;
}

static mirnext::Function &create_double_dsl_arithmetic(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_dsl_arithmetic");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_dsl_arithmetic", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});

  mirnext::Value arg = expect(function.arg("arg1"), 182);
  mirnext::Value result = expect(
      expect(expect(expect(arg + expect(function.f64(2.0), 183), 184)
                   * expect(function.f64(3.0), 185),
                   186)
             / expect(function.f64(2.0), 187),
             188)
          - expect(function.f64(1.0), 189),
      190);
  expect_ok(function.ret(result), 191);
  expect_ok(function.end(), 192);
  return function;
}

static mirnext::Function &create_double_branch(mirnext::Context &ctx,
                                               mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_branch", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Value arg1 = expect(function.arg("arg1"), 189);

  mirnext::Label &high = new_label(function, 190);
  mirnext::Label &low = new_label(function, 191);
  function.if_(arg1 > expect(function.f64(10.0), 192), high);
  function.if_(arg1 <= expect(function.f64(5.0), 193), low);
  function.ret(expect(function.f64(0.25), 194));
  high.ret(expect(high.f64(1.5), 195));
  low.ret(expect(low.f64(-2.5), 196));
  function.end();
  return function;
}

static mirnext::Function &create_int_double_conversion(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_int_double_conversion");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "int_double_conversion", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Value arg1 = expect(function.arg("arg1"), 199);
  mirnext::Value integer = expect(arg1.convert(mirnext::Type::Kind::I64), 200);
  mirnext::Value value = expect(integer.convert(mirnext::Type::Kind::D), 201);
  expect_ok(function.ret(value), 202);
  expect_ok(function.end(), 203);
  return function;
}

static mirnext::Function &create_dsl_i64_double_round_trip(mirnext::Context &ctx,
                                                           mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_dsl_i64_double_round_trip");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "dsl_i64_double_round_trip", {mirnext::Type::i64()},
      {{mirnext::Type::i64(), "arg1"}});

  mirnext::Value as_double = expect(
      function.convert<mirnext::Type::Kind::D>(expect(function.arg("arg1"), 213)), 214);
  mirnext::Value back = expect(function.convert<mirnext::Type::Kind::I64>(as_double), 215);
  expect_ok(function.ret(back), 216);
  expect_ok(function.end(), 217);
  return function;
}

static mirnext::Function &create_dsl_u64_to_double(mirnext::Context &ctx,
                                                   mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_dsl_u64_to_double");
  *module_out = &module;
  mirnext::Function &function = module.new_function("dsl_u64_to_double", {mirnext::Type::d()}, {});

  mirnext::Value result = expect(
      function.convert<mirnext::Type::Kind::D>(expect(function.u64(4097), 218)), 219);
  expect_ok(function.ret(result), 220);
  expect_ok(function.end(), 221);
  return function;
}

static mirnext::Function &create_dsl_f64_to_i64(mirnext::Context &ctx,
                                                mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_dsl_f64_to_i64");
  *module_out = &module;
  mirnext::Function &function = module.new_function("dsl_f64_to_i64", {mirnext::Type::i64()}, {});

  mirnext::Value result = expect(
      function.convert<mirnext::Type::Kind::I64>(expect(function.f64(41.75), 222)), 223);
  expect_ok(function.ret(result), 224);
  expect_ok(function.end(), 225);
  return function;
}

static mirnext::Function &create_float_long_double_lowering(mirnext::Context &ctx,
                                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_float_long_double_lowering");
  *module_out = &module;
  mirnext::Function &function = module.new_function("float_long_double_lowering", {}, {});
  mirnext::Label &done = new_label(function, 212);
  mirnext::Value i8 = expect(function.i8(42), 207);
  mirnext::Value u8 = expect(function.u8(42), 208);
  mirnext::Value i16 = expect(i8.convert(mirnext::Type::Kind::I64), 209);
  mirnext::Value u16 = expect(u8.convert(mirnext::Type::Kind::U64), 210);
  mirnext::Value f = expect(i16.convert(mirnext::Type::Kind::F), 211);
  mirnext::Value ld = expect(u16.convert(mirnext::Type::Kind::LD), 212);
  mirnext::Value f_math = expect(
      expect((f + expect(function.f32(2.0f), 213)) * expect(function.f32(3.0f), 214), 215)
          / expect(function.f32(2.0f), 216),
      217);
  mirnext::Value d = expect(f_math.convert(mirnext::Type::Kind::D), 218);
  mirnext::Value back_to_f = expect(d.convert(mirnext::Type::Kind::F), 219);
  mirnext::Value ld_math = expect(
      expect((ld + expect(function.ld(2.0L), 220)) * expect(function.ld(3.0L), 221), 222)
          / expect(function.ld(2.0L), 223),
      224);
  expect_ok(function.if_(back_to_f < expect(function.f32(0.0f), 225), done), 226);
  expect_ok(function.if_(ld_math > expect(function.ld(0.0L), 227), done), 228);
  mirnext::Value ld_to_d = expect(ld_math.convert(mirnext::Type::Kind::D), 229);
  expect_ok(function.call_void(
                module.new_prototype("sink", {}, {{mirnext::Type::d(), "arg"}}),
                module.new_import("float_long_double_sink"), {ld_to_d}),
            230);
  expect_ok(done.ret(), 231);
  expect_ok(function.end(), 232);
  return function;
}

static mirnext::Function &create_u64_dsl_unsigned_path(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_u64_dsl_unsigned_path");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "u64_dsl_unsigned_path", {mirnext::Type::u64()}, {{mirnext::Type::u64(), "arg1"}});

  mirnext::Label &small = function.label();
  mirnext::Value arg = expect(function.arg("arg1"), 226);
  mirnext::Value half = expect(arg >> expect(function.u64(1), 227), 228);
  expect_ok(function.if_(expect(arg < expect(function.u64(10), 229), 230), small), 231);
  expect_ok(function.ret(half), 232);
  expect_ok(small.ret(expect(small.u64(7), 233)), 234);
  expect_ok(function.end(), 235);
  return function;
}

static mirnext::Function &create_double_negation(mirnext::Context &ctx,
                                                 mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_negation");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_negation", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});

  mirnext::Value arg = expect(function.arg("arg1"), 240);
  expect_ok(function.ret(expect(-arg, 241)), 242);
  expect_ok(function.end(), 243);
  return function;
}

static mirnext::Function &create_void_call_lowering(mirnext::Context &ctx,
                                                    mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_void_call_lowering");
  *module_out = &module;
  mirnext::Prototype &prototype = module.new_prototype(
      "void_p", {}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Import &import = module.new_import("void_sink");
  mirnext::Function &function = module.new_function(
      "void_call_lowering", {}, {{mirnext::Type::i64(), "arg1"}});

  expect_ok(function.call_void(prototype, import, {expect(function.arg("arg1"), 236)}), 237);
  expect_ok(function.ret(), 238);
  expect_ok(function.end(), 239);
  return function;
}

static mirnext::Function &create_raw_switch(mirnext::Context &ctx,
                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_raw_switch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "raw_switch", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value arg = expect(function.arg("arg1"), 244);
  mirnext::Label &zero = function.label();
  mirnext::Label &one = function.label();
  mirnext::Label &two = function.label();
  function.switch_(arg, {&zero, &one, &two});
  zero.ret(zero.i64(7));
  one.ret(one.i64(11));
  two.ret(two.i64(13));
  function.end();
  return function;
}

static mirnext::Function &create_case_switch(mirnext::Context &ctx,
                                             mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_case_switch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "case_switch", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Value arg = expect(function.arg("arg1"), 245);
  mirnext::Label &ten = function.label();
  mirnext::Label &twenty = function.label();
  mirnext::Label &other = function.label();
  function.switch_(arg, {{10, &ten}, {20, &twenty}}, other);
  ten.ret(ten.i64(100));
  twenty.ret(twenty.i64(200));
  other.ret(other.i64(-1));
  function.end();
  return function;
}

static mirnext::Function &create_load_first(mirnext::Context &ctx,
                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_load_first");
  *module_out = &module;
  mirnext::Data &nums = module.data("nums", mirnext::Type::i64(),
                                    std::vector<std::int64_t>{11, 22, 33});
  mirnext::Function &function = module.new_function("load_first", {mirnext::Type::i64()}, {});

  mirnext::Value nums_ref = expect(module.ref(nums), 246);
  mirnext::Value nums_ptr = expect(function.addr(nums_ref, "nums_ptr"), 247);
  mirnext::Value first = expect(function.load(expect(function.mem(mirnext::Type::i64(), nums_ptr),
                                               248)),
                                249);
  expect_ok(function.ret(first), 250);
  expect_ok(function.end(), 251);
  return function;
}

static mirnext::Function &create_load_index(mirnext::Context &ctx,
                                            mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_load_index");
  *module_out = &module;
  mirnext::Data &nums = module.data("nums", mirnext::Type::i64(),
                                    std::vector<std::int64_t>{11, 22, 33});
  mirnext::Function &function = module.new_function(
      "load_index", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});

  mirnext::Value nums_ref = expect(module.ref(nums), 252);
  mirnext::Value nums_ptr = expect(function.addr(nums_ref, "nums_ptr"), 253);
  mirnext::Value index = expect(function.arg("arg1"), 254);
  mirnext::Value item = expect(function.load(expect(function.mem(mirnext::Type::i64(), nums_ptr,
                                                                 index, 8),
                                             255)),
                               256);
  expect_ok(function.ret(item), 257);
  expect_ok(function.end(), 258);
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

static int check_sum_to_n_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = *expect(create_sum_to_n(ctx, &module), 306);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 307);
  const std::int64_t arg = 100;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 308);
  return result == 5050 ? 0 : 309;
}

static int check_sum_to_n_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = *expect(create_sum_to_n(ctx, &module), 310);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 311);
  const std::int64_t arg = 100;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               312);
  return result == 5050 ? 0 : 313;
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
  return result == -3 ? 0 : 105;
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
  return result == -3 ? 0 : 108;
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

static int check_is_less_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_is_less(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 333);
  const std::int64_t less_args[] = {3, 5};
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, less_args, 2),
               334);
  if (result != 1) return 335;
  const std::int64_t greater_args[] = {5, 3};
  result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, greater_args, 2),
                  336);
  return result == 0 ? 0 : 337;
}

static int check_is_less_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_is_less(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 338);
  MIR_load_module(legacy.raw(), lowered.module);
  MIR_gen_init(legacy.raw());
  MIR_gen_set_optimize_level(legacy.raw(), 2);
  MIR_link(legacy.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(legacy.raw(), lowered.function);
  auto fn = reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t)>(addr);
  std::int64_t less = fn(3, 5);
  std::int64_t greater = fn(5, 3);
  MIR_gen_finish(legacy.raw());
  if (less != 1) return 339;
  return greater == 0 ? 0 : 340;
}

static int check_branch_on_bool_literal_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_branch_on_bool_literal(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 341);
  std::int64_t result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function),
                               342);
  return result == 1 ? 0 : 343;
}

static int check_branch_on_bool_literal_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_branch_on_bool_literal(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 344);
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function), 345);
  return result == 1 ? 0 : 346;
}

static int check_checked_add_branch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_checked_add_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 346);
  const std::int64_t ok_args[] = {40, 2};
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, ok_args, 2),
               347);
  if (result != 42) return 348;
  const std::int64_t overflow_args[] = {std::numeric_limits<std::int64_t>::max(), 1};
  result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, overflow_args,
                                         2),
                  349);
  return result == -1 ? 0 : 350;
}

static int check_checked_add_branch_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_checked_add_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 351);
  MIR_load_module(legacy.raw(), lowered.module);
  MIR_gen_init(legacy.raw());
  MIR_gen_set_optimize_level(legacy.raw(), 2);
  MIR_link(legacy.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(legacy.raw(), lowered.function);
  auto fn = reinterpret_cast<std::int64_t (*)(std::int64_t, std::int64_t)>(addr);
  std::int64_t ok = fn(40, 2);
  std::int64_t overflow = fn(std::numeric_limits<std::int64_t>::max(), 1);
  MIR_gen_finish(legacy.raw());
  if (ok != 42) return 352;
  return overflow == -1 ? 0 : 353;
}

static int check_checked_add_i32_branch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_checked_add_i32_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 354);
  const std::int64_t ok_args[] = {100, 23};
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, ok_args, 2),
               355);
  if (static_cast<std::int32_t>(result) != 123) return 356;
  const std::int64_t overflow_args[] = {std::numeric_limits<std::int32_t>::max(), 1};
  result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, overflow_args,
                                         2),
                  357);
  return static_cast<std::int32_t>(result) == -1 ? 0 : 358;
}

static int check_checked_chain_branch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_checked_chain_branch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 359);
  const std::int64_t ok_args[] = {5, 6};
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, ok_args, 2),
               360);
  if (result != 66) return 361;
  const std::int64_t overflow_args[] = {std::numeric_limits<std::int64_t>::max() - 1, 2};
  result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, overflow_args,
                                         2),
                  362);
  return result == -1 ? 0 : 363;
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
  return result == doctest::Approx(14.0) ? 0 : 263;
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
  return result == doctest::Approx(14.0) ? 0 : 266;
}

static int check_double_dsl_arithmetic_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_dsl_arithmetic(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 280);
  const double arg = 8.0;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         281);
  return result == doctest::Approx(14.0) ? 0 : 282;
}

static int check_double_dsl_arithmetic_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_dsl_arithmetic(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 283);
  const double arg = 8.0;
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function, &arg, 1),
               284);
  return result == doctest::Approx(14.0) ? 0 : 285;
}

static int check_double_negation_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_negation(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 332);
  const double arg = 12.5;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         333);
  return result == doctest::Approx(-12.5) ? 0 : 334;
}

static int check_double_negation_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_negation(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 335);
  const double arg = 12.5;
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function, &arg, 1),
               336);
  return result == doctest::Approx(-12.5) ? 0 : 337;
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

static int check_dsl_i64_double_round_trip_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_i64_double_round_trip(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 314);
  const std::int64_t arg = 4097;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 315);
  return result == arg ? 0 : 316;
}

static int check_dsl_i64_double_round_trip_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_i64_double_round_trip(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 317);
  const std::int64_t arg = 4097;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               318);
  return result == arg ? 0 : 319;
}

static int check_dsl_u64_to_double_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_u64_to_double(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 320);
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function), 321);
  return result == doctest::Approx(4097.0) ? 0 : 322;
}

static int check_dsl_u64_to_double_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_u64_to_double(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 323);
  double result
      = expect(mirnext::generate_and_call_double(legacy, lowered.module, lowered.function), 324);
  return result == doctest::Approx(4097.0) ? 0 : 325;
}

static int check_dsl_f64_to_i64_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_f64_to_i64(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 326);
  std::int64_t result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function), 327);
  return result == 41 ? 0 : 328;
}

static int check_dsl_f64_to_i64_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_dsl_f64_to_i64(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 329);
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function), 330);
  return result == 41 ? 0 : 331;
}

static int check_float_long_double_lowering() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_float_long_double_lowering(ctx, &module);
  mirnext::LegacyContext legacy;
  expect(mirnext::lower_to_legacy(legacy, *module, function), 279);
  return 0;
}

static int check_u64_dsl_unsigned_path_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_u64_dsl_unsigned_path(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 286);
  const std::uint64_t arg = UINT64_MAX;
  std::uint64_t result
      = expect(mirnext::interpret_u64(legacy, lowered.module, lowered.function, &arg, 1), 287);
  return result == UINT64_MAX / 2 ? 0 : 288;
}

static int check_u64_dsl_unsigned_path_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_u64_dsl_unsigned_path(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 289);
  const std::uint64_t arg = UINT64_MAX;
  std::uint64_t result
      = expect(mirnext::generate_and_call_u64(legacy, lowered.module, lowered.function, &arg, 1),
               290);
  return result == UINT64_MAX / 2 ? 0 : 291;
}

static int check_void_call_lowering() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_void_call_lowering(ctx, &module);
  mirnext::LegacyContext legacy;
  expect(mirnext::lower_to_legacy(legacy, *module, function), 292);
  return 0;
}

static int check_raw_switch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_raw_switch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 313);
  const std::int64_t arg = 2;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 314);
  return result == 13 ? 0 : 315;
}

static int check_case_switch_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_case_switch(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 316);
  const std::int64_t twenty = 20;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &twenty, 1), 317);
  if (result != 200) return 318;
  const std::int64_t miss = 15;
  result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &miss, 1), 319);
  return result == -1 ? 0 : 320;
}

static int check_load_first_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_load_first(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 321);
  std::int64_t result = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function),
                               322);
  return result == 11 ? 0 : 323;
}

static int check_load_first_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_load_first(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 324);
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function), 325);
  return result == 11 ? 0 : 326;
}

static int check_load_index_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_load_index(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 327);
  const std::int64_t arg = 2;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 328);
  return result == 33 ? 0 : 329;
}

static int check_load_index_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_load_index(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 330);
  const std::int64_t arg = 2;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               331);
  return result == 33 ? 0 : 332;
}

static int check_binary_loop_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_loop(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(read_binary_module(legacy, *module, std::string(function.name()).c_str()), 293);
  const std::int64_t arg = 10000;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 294);
  return result == arg ? 0 : 295;
}

static int check_binary_import_call_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_import_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(read_binary_module(legacy, *module, std::string(function.name()).c_str()), 296);
  MIR_load_external(legacy.raw(), "add7", reinterpret_cast<void *>(add7));
  const std::int64_t arg = 35;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 297);
  return result == 42 ? 0 : 298;
}

static int check_binary_forward_call_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_forward_call(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(read_binary_module(legacy, *module, std::string(function.name()).c_str()), 299);
  const std::int64_t arg = 25;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 300);
  return result == 42 ? 0 : 301;
}

static int check_binary_double_interp() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_double_arithmetic(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(read_binary_module(legacy, *module, std::string(function.name()).c_str()), 302);
  const double arg = 8.0;
  double result = expect(mirnext::interpret_double(legacy, lowered.module, lowered.function, &arg, 1),
                         303);
  return result == doctest::Approx(14.0) ? 0 : 304;
}

static int check_binary_void_call_read() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_void_call_lowering(ctx, &module);
  mirnext::LegacyContext legacy;
  expect(read_binary_module(legacy, *module, std::string(function.name()).c_str()), 305);
  return 0;
}

TEST_CASE("legacy lowering executes loop") {
  CHECK(check_loop_interp() == 0);
  CHECK(check_loop_gen() == 0);
}

TEST_CASE("legacy lowering executes Block tree sum_to_n") {
  CHECK(check_sum_to_n_interp() == 0);
  CHECK(check_sum_to_n_gen() == 0);
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
  CHECK(check_is_less_interp() == 0);
  CHECK(check_is_less_gen() == 0);
  CHECK(check_branch_on_bool_literal_interp() == 0);
  CHECK(check_branch_on_bool_literal_gen() == 0);
  CHECK(check_checked_add_branch_interp() == 0);
  CHECK(check_checked_add_branch_gen() == 0);
  CHECK(check_checked_add_i32_branch_interp() == 0);
  CHECK(check_checked_chain_branch_interp() == 0);
}

TEST_CASE("legacy lowering executes import and earlier internal calls") {
  CHECK(check_import_call_interp() == 0);
  CHECK(check_import_call_gen() == 0);
  CHECK(check_internal_call_interp() == 0);
  CHECK(check_internal_call_gen() == 0);
  CHECK(check_void_call_lowering() == 0);
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
  CHECK(check_double_dsl_arithmetic_interp() == 0);
  CHECK(check_double_dsl_arithmetic_gen() == 0);
  CHECK(check_double_negation_interp() == 0);
  CHECK(check_double_negation_gen() == 0);
  CHECK(check_double_branch_interp() == 0);
  CHECK(check_double_branch_gen() == 0);
  CHECK(check_int_double_conversion_interp() == 0);
  CHECK(check_int_double_conversion_gen() == 0);
  CHECK(check_dsl_i64_double_round_trip_interp() == 0);
  CHECK(check_dsl_i64_double_round_trip_gen() == 0);
  CHECK(check_dsl_u64_to_double_interp() == 0);
  CHECK(check_dsl_u64_to_double_gen() == 0);
  CHECK(check_dsl_f64_to_i64_interp() == 0);
  CHECK(check_dsl_f64_to_i64_gen() == 0);
}

TEST_CASE("legacy lowering accepts float and long double operations") {
  CHECK(check_float_long_double_lowering() == 0);
}

TEST_CASE("legacy lowering executes u64 DSL unsigned path") {
  CHECK(check_u64_dsl_unsigned_path_interp() == 0);
  CHECK(check_u64_dsl_unsigned_path_gen() == 0);
}

TEST_CASE("legacy lowering executes switch DSL") {
  CHECK(check_raw_switch_interp() == 0);
  CHECK(check_case_switch_interp() == 0);
}

TEST_CASE("legacy lowering executes addr data loads") {
  CHECK(check_load_first_interp() == 0);
  CHECK(check_load_first_gen() == 0);
  CHECK(check_load_index_interp() == 0);
  CHECK(check_load_index_gen() == 0);
}

TEST_CASE("binary encode round-trips through MIR-C reader") {
  CHECK(check_binary_loop_interp() == 0);
  CHECK(check_binary_import_call_interp() == 0);
  CHECK(check_binary_forward_call_interp() == 0);
  CHECK(check_binary_double_interp() == 0);
  CHECK(check_binary_void_call_read() == 0);
}
