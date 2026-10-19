/* Licensed under the MIT License. Finite MIR value numbering and availability cases. */
#pragma once
#include <string>
#include <vector>
#include <random>
#include "mirnext/Transforms/IntegerFolding.hpp"
namespace mirnext::test {
inline std::vector<std::string> gvn_fixtures () {
  std::vector<std::string> result;
  auto add = [&] (std::string_view locals, std::string_view body) {
    result.push_back ("m: module\nentry: func i64\nlocal " + std::string (locals) + "\n"
                      + std::string (body) + "\nendfunc\nendmodule\n");
  };
  add ("i64:a, i64:b, i64:c, i64:d", "mov a, 40\nmov b, 2\nadd c, a, b\nadd d, c, b\nret d");
  add ("i64:a, i64:b, i64:c",
       "mov a, 42\nbt yes, a\nmov b, 7\njmp done\nyes:\nmov b, 42\ndone:\nadd c, b, a\nret c");
  add ("i64:a, i64:b, i64:c",
       "mov a, 0\nbt yes, a\nmov b, 42\njmp done\nyes:\nmov b, 7\ndone:\nadd c, b, a\nret c");
  add ("i64:a, i64:b, i64:c, i64:d, i64:p",
       "alloca p, 16\nmov i64:0(p), 17\nmov a, i64:0(p)\nmov b, 2\nadd c, a, b\nadd d, a, b\nadd "
       "c, c, d\nret c");
  add ("i64:a, i64:b, i64:c, i64:d, i64:p",
       "alloca p, 16\nmov a, 42\nmov i64:0(p), a\nmov b, i64:0(p)\nmov c, i64:0(p)\nadd d, b, "
       "c\nret d");
  add ("i64:a, i64:b, i64:c, i64:p",
       "alloca p, 16\nmov i64:0(p), 42\nmov a, i64:0(p)\nmov i64:0(p), a\nmov b, i64:0(p)\nret b");
  add ("i64:a, i64:b, i64:c, i64:p",
       "alloca p, 16\nmov a, 1\nadd b, p, a\nmov a, 2\nsub c, b, a\nsub c, c, p\nret c");
  add ("i64:a, i64:b, i64:c, i64:p",
       "alloca p, 16\nmov a, 1\nadd b, p, a\nmov a, 1\nsub c, b, a\nsub c, c, p\nret c");
  add ("i64:a, i64:b, i64:c",
       "mov a, 1\nmov b, 2\naddo c, a, b\nbo overflow\nret c\noverflow:\nret b");
  add ("i64:a, i64:b, i64:c",
       "mov a, 0\nmov b, 0\nloop:\nadd a, a, 1\nadd b, b, 2\nblt loop, a, 5\nadd c, a, b\nret c");
  result.push_back (R"(m: module
state: i64 21
entry: func i64
local i64:p, i64:a, i64:b, i64:c
mov p, state
mov a, i64:0(p)
mov b, i64:0(p)
add c, a, b
ret c
endfunc
endmodule
)");
  result.push_back (R"(m: module
entry: func i64, i64:x, i64:y
local i64:a, i64:b, i64:c, i64:d
add a, x, y
add b, x, y
mul c, a, b
ret c
endfunc
endmodule
)");
  return result;
}
inline std::vector<std::string> gvn_rule_fixtures () {
  using namespace ir;
  std::vector<std::string> result;
  auto add = [&] (std::string_view body) {
    result.push_back (
      "m: module\nstate: i64 17, 29, 41\nentry: func i64\nlocal i64:p, i64:q, i64:a, i64:b, i64:c, "
      "i64:i\nmov p, state\nmov q, state\n"
      + std::string (body) + "\nendfunc\nendmodule\n");
  };
  // Exercise each integer opcode with positive values. Separate boundary
  // cases cover overflow and other arithmetic edge conditions.
  for (auto &descriptor : instruction_descriptors ()) {
    if (!transforms::fold_integer (descriptor.opcode, 42, 5)) continue;
    if (descriptor.flow == ControlFlow::Branch) {
      auto args = descriptor.roles.size () == 2 ? " a" : " a, b";
      add ("mov a, 42\nmov b, 5\n" + std::string (descriptor.name) + " yes," + args
           + "\nret 0\nyes:\nret 1");
    } else {
      auto args = descriptor.roles.size () == 2 ? " a" : " a, b";
      add ("mov a, 42\nmov b, 5\n" + std::string (descriptor.name) + " c," + args
           + (descriptor.opcode == Opcode::Negs ? "\next32 c, c" : "") + "\nret c");
    }
  }
  for (auto suffix : {"", ":left", ":right", "::separate"}) {
    add ("mov a, i64:0(p):left\nmov i64:8(q)" + std::string (suffix)
         + ", 13\nmov b, i64:0(p):left\nadd c, a, b\nret c");
  }
  add ("mov a, i64:0(p)\nmov i64:0(p), a\nmov b, i64:0(p)\nadd c, a, b\nret c");
  add (
    "mov i, 0\nloop:\nmov a, i64:0(p)\nadd a, a, 1\nmov i64:0(p), a\nadd i, i, 1\nblt loop, i, "
    "7\nmov b, i64:0(p)\nret b");
  add ("alloca q, 16\nmov i64:0(q), 42\nmov i64:0(p), q\nmov a, i64:0(q)\nret a");
  add (
    "mov a, i64:0(p)\nbt yes, a\nmov b, i64:8(p)\njmp join\nyes:\nmov b, i64:0(p)\njoin:\nmov c, "
    "i64:0(p)\nadd c, c, b\nret c");
  add (
    "mov a, i64:0(p)\nbt yes, a\nadd b, a, 129\njmp join\nyes:\nadd b, a, 129\njoin:\nadd c, a, "
    "129\nadd c, c, b\nret c");
  // Single memory-use addresses in the preferred displacement interval are
  // intentionally recalculated; values immediately outside may be shared.
  for (auto offset : {-129, -128, 127, 128}) {
    std::string prefix
      = "m: module\nentry: func i64, i64:base\nlocal i64:p, i64:q, i64:a, i64:b, i64:c\n";
    result.push_back (prefix + "add p, base, " + std::to_string (offset)
                      + "\nmov a, i64:0(p)\nadd q, base, " + std::to_string (offset)
                      + "\nmov b, i64:0(q)\nadd c, a, b\nret c\nendfunc\nendmodule\n");
  }
  add (
    "mov a, 0\nbt yes, a\nmov b, 42\nmov c, 42\njmp join\nyes:\nmov b, 42\nmov c, 42\njoin:\nadd "
    "a, b, c\nret a");
  result.push_back (R"(m: module
state: i64 21
sig: proto i64, i64:p
change: func i64, i64:p
mov i64:0(p), 42
ret 0
endfunc
entry: func i64
local i64:p, i64:a, i64:b, i64:c
mov p, state
mov a, i64:0(p)
call sig, change, c, p
mov b, i64:0(p)
add c, a, b
ret c
endfunc
endmodule
)");
  result.push_back (R"(m: module
sig: proto i64, i64, i64:x
pair: func i64, i64, i64:x
local i64:y
add y, x, 1
ret x, y
endfunc
entry: func i64
local i64:a, i64:b, i64:c, i64:d
call sig, pair, a, b, 20
add c, a, 1
add d, b, 1
add c, c, d
ret c
endfunc
endmodule
)");
  result.push_back (R"(m: module
entry: func i64, d:x, d:y
local d:a, d:b, i64:equal
dadd a, x, y
dadd b, x, y
deq equal, a, b
ret equal
endfunc
endmodule
)");
  for (auto number : {"+nan", "-0.0", "0.0", "+inf"}) {
    result.push_back("m: module\nstate: d " + std::string(number) + "\nentry: func i64\nlocal i64:p, i64:r, d:a, d:b, d:c, d:e\nmov p, state\ndmov a, d:0(p)\ndmov b, d:0(p)\ndadd c, a, b\ndadd e, a, b\ndeq r, c, e\nret r\nendfunc\nendmodule\n");
  }
  // Keep the host ABI intact, including compilers without inline assembly.
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  result.push_back("m: module\nentry: func i64\nglobal i64:bound:" + std::string(binding)
    + "\nlocal i64:a, i64:b\nmov bound, 13\nmov a, bound\nmov bound, 29\nadd b, a, bound\nret b\nendfunc\nendmodule\n");
  return result;
}
inline std::vector<std::string> random_gvn_fixtures () {
  std::mt19937 random (0x67f3);
  std::vector<std::string> result;
  for (unsigned n = 0; n < 64; ++n) {
    std::string text = "m: module\nstate: i64 " + std::to_string(random() % 100) + ", 0\nentry: func i64\nlocal i64:p, i64:a, i64:b, i64:c, i64:i\nmov p, state\nmov a, i64:0(p)\nmov i, 0\nloop:\n";
    auto k = std::to_string (random () % 19 + 1);
    text += "add b, a, " + k + "\nadd c, a, " + k + "\nxor b, b, c\nadd a, a, "
            + std::to_string (random () % 5 + 1) + "\n";
    text += "beq same, b, 0\nadd a, a, 100\njmp join\nsame:\nadd a, a, "
            + std::to_string (random () % 7 + 1) + "\njoin:\n";
    text += "mov i64:8(p), a\nmov b, i64:8(p)\nmov c, i64:8(p)\nxor b, b, c\nadd a, a, b\nadd i, i, 1\nblt loop, i, " + std::to_string(random() % 8 + 1) + "\nmov i64:0(p), a\nret a\nendfunc\nendmodule\n";
    result.push_back (std::move (text));
  }
  return result;
}
}  // namespace mirnext::test
