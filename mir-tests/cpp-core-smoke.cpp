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
  mirnext::Function &function = module.new_function(
      "loop", {mirnext::Type::i64()}, {{mirnext::Type::i64(), "arg1"}});
  mirnext::Register arg1 = expect(function.argument("arg1"), 17);
  mirnext::Register count = expect(function.create_register(mirnext::Type::i64(), "count"), 18);

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
  expect(builder.create_bge(fin, count, arg1), 25);
  expect(builder.bind(cont), 26);
  expect(builder.create_add(count, count, mirnext::Operand::int64(1)), 27);
  expect(builder.create_blt(cont, count, arg1), 28);
  expect(builder.bind(fin), 29);
  expect(builder.create_ret({count}), 30);

  if (module.name() != "m") return 1;
  if (function.name() != "loop") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 1) return 4;
  if (function.local_registers().size() != 1) return 5;
  if (function.instruction_count() != 7) return 6;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (!contains(text, "module m")) return 7;
  if (!contains(text, "func loop(i64 %arg1) -> i64")) return 8;
  if (!contains(text, "reg i64 %count")) return 9;
  if (!contains(text, "mov %count 0")) return 10;
  if (!contains(text, "bge L1 %count %arg1")) return 11;
  if (!contains(text, "label L2")) return 12;
  if (!contains(text, "add %count %count 1")) return 13;
  if (!contains(text, "blt L2 %count %arg1")) return 14;
  if (!contains(text, "label L1")) return 15;
  if (!contains(text, "ret %count")) return 16;

  return 0;
}
