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

int main() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("m_sieve");
  mirnext::Function &function = module.new_function("sieve", {mirnext::Type::i64()}, {});

  mirnext::IRBuilder builder(function);

  mirnext::Label entry = builder.entry();
  mirnext::Label loop = builder.label();
  mirnext::Label loop2 = builder.label();
  mirnext::Label loop3 = builder.label();
  mirnext::Label loop4 = builder.label();
  mirnext::Label fin = builder.label();
  mirnext::Label fin2 = builder.label();
  mirnext::Label fin3 = builder.label();
  mirnext::Label fin4 = builder.label();
  mirnext::Label cont3 = builder.label();
  if (!builder.ok()) return 21;

  mirnext::Value flags = entry.alloca(819000, "flags");
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

  loop2.if_(i >= loop2.i64(819000), fin2);
  loop2.store(loop2.mem(mirnext::Type::u8(), flags, i), loop2.u8(1));
  loop2.assign(i, i + loop2.i64(1));
  loop2.jmp(loop2);

  fin2.assign(i, fin2.i64(1));
  fin2.jmp(loop3);

  loop3.if_(i >= loop3.i64(819000), fin3);
  loop3.if_(loop3.load(loop3.mem(mirnext::Type::u8(), flags, i)) == loop3.u8(0), cont3);
  loop3.assign(prime, i + loop3.i64(1));
  loop3.assign(k, i + prime);
  loop3.jmp(loop4);

  loop4.if_(k >= loop4.i64(819000), fin4);
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
  if (!builder.ok()) return 62;

  if (module.name() != "m_sieve") return 1;
  if (function.name() != "sieve") return 2;
  if (function.return_types().size() != 1) return 3;
  if (function.arguments().size() != 0) return 4;
  if (function.local_registers().size() != 14) return 5;
  if (function.instruction_count() != 47) return 6;

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
