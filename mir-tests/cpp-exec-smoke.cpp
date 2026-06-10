/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

template <class T> static T expect(mirnext::Result<T> result, int code) {
  if (!result) {
    FAIL("error " << code << ": " << result.error().message);
    std::abort();
  }
  return result.value();
}

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
  MIRNEXT_TRY(auto bytes, module.encode_binary());
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

static void append(mirnext::Function &function, mirnext::Opcode opcode,
                   std::vector<mirnext::Operand> operands = {}) {
  function.append(mirnext::Instruction(opcode, std::move(operands)));
}

static mirnext::Label new_label(mirnext::IRBuilder &builder, int code) {
  mirnext::Label label = builder.label();
  if (!builder.ok()) {
    FAIL("error " << code << ": " << builder.error().message);
    std::abort();
  }
  return label;
}

static mirnext::Function &create_loop(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 1);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 2);

  mirnext::IRBuilder builder(function);
  mirnext::Label fin = new_label(builder, 3);
  mirnext::Label cont = new_label(builder, 4);

  append(function, mirnext::Opcode::Mov, {count, mirnext::Operand::int64(0)});
  append(function, mirnext::Opcode::Bge, {fin, count, arg1});
  function.append_label(cont);
  append(function, mirnext::Opcode::Add, {count, count, mirnext::Operand::int64(1)});
  append(function, mirnext::Opcode::Blt, {cont, count, arg1});
  function.append_label(fin);
  function.append_ret({count});
  return function;
}

static mirnext::Function &create_sum_to_n(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_sum");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "sum_to_n", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "n"}});

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Label loop = new_label(builder, 306);
  mirnext::Label body = new_label(builder, 307);
  mirnext::Label done = new_label(builder, 308);

  mirnext::Value n = entry.arg("n");
  mirnext::Var i = entry.local(mirnext::Type::i64(), "i");
  mirnext::Var sum = entry.local(mirnext::Type::i64(), "sum");

  entry.begin();
  i = 1;
  sum = 0;
  entry.jmp(loop);

  loop.begin();
  loop.if_(i > n, done);
  loop.jmp(body);
  loop.end();

  body.begin();
  sum = sum + i;
  i = i + body.i64(1);
  body.jmp(loop);
  body.end();

  done.begin();
  done.ret(sum);
  done.end();
  entry.end();
  if (!builder.ok()) {
    FAIL("sum_to_n DSL builder error: " << builder.error().message);
    std::abort();
  }
  return function;
}

static mirnext::Function &create_sieve(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_sieve");
  *module_out = &module;
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::IRBuilder builder(function);

  mirnext::Label entry = builder.entry();
  mirnext::Label loop = new_label(builder, 18);
  mirnext::Label loop2 = new_label(builder, 19);
  mirnext::Label loop3 = new_label(builder, 20);
  mirnext::Label loop4 = new_label(builder, 21);
  mirnext::Label fin = new_label(builder, 22);
  mirnext::Label fin2 = new_label(builder, 23);
  mirnext::Label fin3 = new_label(builder, 24);
  mirnext::Label fin4 = new_label(builder, 25);
  mirnext::Label cont3 = new_label(builder, 26);

  mirnext::Value flags = entry.alloca(8190, "flags");
  mirnext::Value iter = entry.local(mirnext::Type::i64(), "iter");
  mirnext::Value count = entry.local(mirnext::Type::i64(), "count");
  mirnext::Value i = entry.local(mirnext::Type::i64(), "i");
  mirnext::Value k = entry.local(mirnext::Type::i64(), "k");
  mirnext::Value prime = entry.local(mirnext::Type::i64(), "prime");
  entry.assign(iter, entry.i64(0));
  entry.jmp(loop);

  loop.if_(iter >= loop.i64(100), fin);
  loop.assign(count, loop.i64(0));
  loop.assign(i, loop.i64(0));
  loop.jmp(loop2);

  loop2.if_(i >= loop2.i64(8190), fin2);
  loop2.store(loop2.mem(mirnext::Type::u8(), flags, i), loop2.u8(1));
  loop2.assign(i, i + loop2.i64(1));
  loop2.jmp(loop2);

  fin2.assign(i, fin2.i64(1));
  fin2.jmp(loop3);

  loop3.if_(i >= loop3.i64(8190), fin3);
  loop3.if_(loop3.load(loop3.mem(mirnext::Type::u8(), flags, i)) == loop3.u8(0), cont3);
  loop3.assign(prime, i + loop3.i64(1));
  loop3.assign(k, i + prime);
  loop3.jmp(loop4);

  loop4.if_(k >= loop4.i64(8190), fin4);
  loop4.store(loop4.mem(mirnext::Type::u8(), flags, k), loop4.u8(0));
  loop4.assign(k, k + prime);
  loop4.jmp(loop4);

  fin4.assign(count, count + fin4.i64(1));
  fin4.jmp(cont3);

  cont3.assign(i, i + cont3.i64(1));
  cont3.jmp(loop3);

  fin3.assign(iter, iter + fin3.i64(1));
  fin3.jmp(loop);

  fin.ret(count);
  if (!builder.ok()) std::exit(27);
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

  append(function, mirnext::Opcode::Mov, {a, arg1});
  append(function, mirnext::Opcode::Sub, {a, a, mirnext::Operand::int64(5)});
  append(function, mirnext::Opcode::Mul, {a, a, mirnext::Operand::int64(3)});
  append(function, mirnext::Opcode::Div, {a, a, mirnext::Operand::int64(2)});
  append(function, mirnext::Opcode::Mod, {b, a, mirnext::Operand::int64(6)});
  append(function, mirnext::Opcode::Or, {c, b, mirnext::Operand::int64(8)});
  append(function, mirnext::Opcode::Xor, {c, c, mirnext::Operand::int64(3)});
  append(function, mirnext::Opcode::And, {c, c, mirnext::Operand::int64(14)});
  append(function, mirnext::Opcode::Lsh, {c, c, mirnext::Operand::int64(1)});
  append(function, mirnext::Opcode::Rsh, {c, c, mirnext::Operand::int64(2)});
  append(function, mirnext::Opcode::URsh, {c, c, mirnext::Operand::int64(1)});
  append(function, mirnext::Opcode::Neg, {b, c});
  append(function, mirnext::Opcode::Eq, {cmp, c, mirnext::Operand::int64(2)});
  append(function, mirnext::Opcode::Add, {result, b, cmp});
  function.append_ret({result});
  return function;
}

static mirnext::Function &create_integer_branch(mirnext::Context &ctx, mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_integer_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "integer_branch", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 93);

  mirnext::IRBuilder builder(function);
  mirnext::Label signed_gt = new_label(builder, 94);
  mirnext::Label unsigned_gt = new_label(builder, 95);
  append(function, mirnext::Opcode::Bgt, {signed_gt, arg1, mirnext::Operand::int64(10)});
  append(function, mirnext::Opcode::UBgt, {unsigned_gt, arg1, mirnext::Operand::int64(10)});
  function.append_ret({mirnext::Operand::int64(0)});
  function.append_label(signed_gt);
  function.append_ret({mirnext::Operand::int64(1)});
  function.append_label(unsigned_gt);
  function.append_ret({mirnext::Operand::int64(9)});
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

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Value result = entry.call(prototype, import, {entry.arg("arg1")});
  entry.ret(result);
  if (!builder.ok()) {
    FAIL("import call DSL builder error: " << builder.error().message);
    std::abort();
  }
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
  append(callee, mirnext::Opcode::Add, {callee_result, callee_arg, mirnext::Operand::int64(11)});
  callee.append_ret({callee_result});

  mirnext::Function &caller = module.new_function(
      "internal_call", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::IRBuilder builder(caller);
  mirnext::Label entry = builder.entry();
  mirnext::Value result = entry.call(prototype, callee, {entry.arg("arg1")});
  entry.ret(result);
  if (!builder.ok()) {
    FAIL("internal call DSL builder error: " << builder.error().message);
    std::abort();
  }
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

  mirnext::IRBuilder builder(caller);
  mirnext::Label entry = builder.entry();
  mirnext::Value result = entry.call(prototype, callee, {entry.arg("arg1")});
  entry.ret(result);
  if (!builder.ok()) {
    FAIL("forward call DSL builder error: " << builder.error().message);
    std::abort();
  }

  mirnext::Register callee_arg = expect(callee.argument("arg1"), 143);
  mirnext::Register callee_result
      = expect(callee.create_register(mirnext::Type::i64(), "result"), 144);
  append(callee, mirnext::Opcode::Add, {callee_result, callee_arg, mirnext::Operand::int64(17)});
  callee.append_ret({callee_result});
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

  mirnext::IRBuilder builder(function);
  mirnext::Label base = new_label(builder, 151);
  append(function, mirnext::Opcode::Ble, {base, arg1, mirnext::Operand::int64(0)});
  append(function, mirnext::Opcode::Sub, {next, arg1, mirnext::Operand::int64(1)});
  append(function, mirnext::Opcode::Call,
         {mirnext::Operand::ref(prototype), mirnext::Operand::ref(function), recursive, next});
  append(function, mirnext::Opcode::Add, {result, recursive, mirnext::Operand::int64(1)});
  function.append_ret({result});
  function.append_label(base);
  function.append_ret({mirnext::Operand::int64(0)});
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
  append(first, mirnext::Opcode::Call,
         {mirnext::Operand::ref(prototype), mirnext::Operand::ref(second), first_result,
          first_arg});
  first.append_ret({first_result});

  mirnext::Register second_arg = expect(second.argument("arg1"), 163);
  mirnext::Register second_result
      = expect(second.create_register(mirnext::Type::i64(), "result"), 164);
  append(second, mirnext::Opcode::Call,
         {mirnext::Operand::ref(prototype), mirnext::Operand::ref(first), second_result,
          second_arg});
  second.append_ret({second_result});
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

  append(function, mirnext::Opcode::DMov, {value, arg1});
  append(function, mirnext::Opcode::DAdd, {value, value, mirnext::Operand::float64(2.0)});
  append(function, mirnext::Opcode::DMul, {value, value, mirnext::Operand::float64(3.0)});
  append(function, mirnext::Opcode::DDiv, {value, value, mirnext::Operand::float64(2.0)});
  append(function, mirnext::Opcode::DSub, {value, value, mirnext::Operand::float64(1.0)});
  append(function, mirnext::Opcode::DNeg, {value, value});
  function.append_ret({value});
  return function;
}

static mirnext::Function &create_double_dsl_arithmetic(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_dsl_arithmetic");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_dsl_arithmetic", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Value arg = entry.arg("arg1");
  mirnext::Value result
      = (((arg + entry.f64(2.0)) * entry.f64(3.0)) / entry.f64(2.0)) - entry.f64(1.0);
  entry.ret(result);
  if (!builder.ok()) {
    FAIL("double DSL builder error: " << builder.error().message);
    std::abort();
  }
  return function;
}

static mirnext::Function &create_double_branch(mirnext::Context &ctx,
                                               mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_double_branch");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "double_branch", {mirnext::Type::d()}, {{mirnext::Type::d(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 189);

  mirnext::IRBuilder builder(function);
  mirnext::Label high = new_label(builder, 190);
  mirnext::Label low = new_label(builder, 191);
  append(function, mirnext::Opcode::DBgt, {high, arg1, mirnext::Operand::float64(10.0)});
  append(function, mirnext::Opcode::DBle, {low, arg1, mirnext::Operand::float64(5.0)});
  function.append_ret({mirnext::Operand::float64(0.25)});
  function.append_label(high);
  function.append_ret({mirnext::Operand::float64(1.5)});
  function.append_label(low);
  function.append_ret({mirnext::Operand::float64(-2.5)});
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

  append(function, mirnext::Opcode::D2I, {integer, arg1});
  append(function, mirnext::Opcode::I2D, {value, integer});
  function.append_ret({value});
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

  mirnext::IRBuilder builder(function);
  mirnext::Label done = new_label(builder, 212);
  append(function, mirnext::Opcode::Mov, {i, mirnext::Operand::int64(42)});
  append(function, mirnext::Opcode::Ext8, {i, i});
  append(function, mirnext::Opcode::Ext16, {i, i});
  append(function, mirnext::Opcode::Ext32, {i, i});
  append(function, mirnext::Opcode::UExt8, {i, i});
  append(function, mirnext::Opcode::UExt16, {i, i});
  append(function, mirnext::Opcode::UExt32, {i, i});
  append(function, mirnext::Opcode::I2F, {f, i});
  append(function, mirnext::Opcode::UI2LD, {ld, i});
  append(function, mirnext::Opcode::FMov, {f, mirnext::Operand::float32(1.25f)});
  append(function, mirnext::Opcode::FNeg, {f, f});
  append(function, mirnext::Opcode::FAdd, {f, f, mirnext::Operand::float32(2.0f)});
  append(function, mirnext::Opcode::FSub, {f, f, mirnext::Operand::float32(1.0f)});
  append(function, mirnext::Opcode::FMul, {f, f, mirnext::Operand::float32(3.0f)});
  append(function, mirnext::Opcode::FDiv, {f, f, mirnext::Operand::float32(2.0f)});
  append(function, mirnext::Opcode::FEq, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FNe, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FLt, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FLe, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FGt, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FGe, {cmp, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FBeq, {done, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::FBlt, {done, f, mirnext::Operand::float32(0.0f)});
  append(function, mirnext::Opcode::I2LD, {ld, i});
  append(function, mirnext::Opcode::LDMov, {ld, mirnext::Operand::long_double(1.5L)});
  append(function, mirnext::Opcode::LDNeg, {ld, ld});
  append(function, mirnext::Opcode::LDAdd, {ld, ld, mirnext::Operand::long_double(2.0L)});
  append(function, mirnext::Opcode::LDSub, {ld, ld, mirnext::Operand::long_double(1.0L)});
  append(function, mirnext::Opcode::LDMul, {ld, ld, mirnext::Operand::long_double(3.0L)});
  append(function, mirnext::Opcode::LDDiv, {ld, ld, mirnext::Operand::long_double(2.0L)});
  append(function, mirnext::Opcode::LDEq, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDNe, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDLt, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDLe, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDGt, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDGe, {cmp, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDBeq, {done, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::LDBgt, {done, ld, mirnext::Operand::long_double(0.0L)});
  append(function, mirnext::Opcode::F2D, {d, f});
  append(function, mirnext::Opcode::F2LD, {ld, f});
  append(function, mirnext::Opcode::D2F, {f, d});
  append(function, mirnext::Opcode::D2LD, {ld, d});
  append(function, mirnext::Opcode::LD2F, {f, ld});
  append(function, mirnext::Opcode::LD2D, {d, ld});
  append(function, mirnext::Opcode::F2I, {i, f});
  append(function, mirnext::Opcode::LD2I, {i, ld});
  function.append_label(done);
  function.append_ret();
  return function;
}

static mirnext::Function &create_u64_dsl_unsigned_path(mirnext::Context &ctx,
                                                       mirnext::Module **module_out) {
  mirnext::Module &module = ctx.new_module("m_u64_dsl_unsigned_path");
  *module_out = &module;
  mirnext::Function &function = module.new_function(
      "u64_dsl_unsigned_path", {mirnext::Type::u64()}, {{mirnext::Type::u64(), "arg1"}});

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  mirnext::Label small = builder.label();
  mirnext::Value arg = entry.arg("arg1");
  mirnext::Value half = arg / entry.u64(2);
  entry.if_(arg < entry.u64(10), small);
  entry.ret(half);
  small.ret(small.u64(7));
  if (!builder.ok()) {
    FAIL("u64 DSL builder error: " << builder.error().message);
    std::abort();
  }
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

  mirnext::IRBuilder builder(function);
  mirnext::Label entry = builder.entry();
  entry.call_void(prototype, import, {entry.arg("arg1")});
  entry.ret();
  if (!builder.ok()) {
    FAIL("void call DSL builder error: " << builder.error().message);
    std::abort();
  }
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
  mirnext::Function &function = create_sum_to_n(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 306);
  const std::int64_t arg = 100;
  std::int64_t result
      = expect(mirnext::interpret_i64(legacy, lowered.module, lowered.function, &arg, 1), 307);
  return result == 5050 ? 0 : 308;
}

static int check_sum_to_n_gen() {
  mirnext::Context ctx;
  mirnext::Module *module = nullptr;
  mirnext::Function &function = create_sum_to_n(ctx, &module);
  mirnext::LegacyContext legacy;
  mirnext::LegacyLoweredFunction lowered
      = expect(mirnext::lower_to_legacy(legacy, *module, function), 309);
  const std::int64_t arg = 100;
  std::int64_t result
      = expect(mirnext::generate_and_call_i64(legacy, lowered.module, lowered.function, &arg, 1),
               310);
  return result == 5050 ? 0 : 311;
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
  return result == doctest::Approx(-14.0) ? 0 : 304;
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

TEST_CASE("legacy lowering executes IRBuilder sum_to_n") {
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
  CHECK(check_double_branch_interp() == 0);
  CHECK(check_double_branch_gen() == 0);
  CHECK(check_int_double_conversion_interp() == 0);
  CHECK(check_int_double_conversion_gen() == 0);
}

TEST_CASE("legacy lowering accepts float and long double operations") {
  CHECK(check_float_long_double_lowering() == 0);
}

TEST_CASE("legacy lowering executes u64 DSL unsigned path") {
  CHECK(check_u64_dsl_unsigned_path_interp() == 0);
  CHECK(check_u64_dsl_unsigned_path_gen() == 0);
}

TEST_CASE("binary encode round-trips through MIR-C reader") {
  CHECK(check_binary_loop_interp() == 0);
  CHECK(check_binary_import_call_interp() == 0);
  CHECK(check_binary_forward_call_interp() == 0);
  CHECK(check_binary_double_interp() == 0);
  CHECK(check_binary_void_call_read() == 0);
}
