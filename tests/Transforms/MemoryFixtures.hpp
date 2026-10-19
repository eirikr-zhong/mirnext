/* Licensed under the MIT License. Copy/extension and memory-liveness rule coverage. */
#pragma once
#include <random>
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> memory_fixtures() {
  std::vector<std::string> result;
  auto add = [&](std::string body) {
    result.push_back("m: module\nstate: i64 -12345, 257, 0, 41, 13, 5, 7, 9\nentry: func i64\nlocal i64:p, i64:q, i64:x, i64:y, i64:a, i64:b, i64:c, i64:i\nmov p, state\nmov x, i64:0(p)\nmov y, i64:8(p)\n" + body + "\nendfunc\nendmodule\n");
  };
  for (auto op : {"mul", "muls", "div", "divs", "udiv", "udivs"}) for (auto divisor : {1, 2, 8, 1024, -2, 3}) {
    auto tail = std::string(op) == "muls" || std::string(op) == "divs" ? "ext32 c, c\n" : std::string(op) == "udivs" ? "uext32 c, c\n" : "";
    add(std::string(op) + " c, x, " + std::to_string(divisor) + "\n" + tail + "ret c");
  }
  add("mul c, 8, x\nret c");
  for (auto first : {"ext8", "ext16", "ext32", "uext8", "uext16", "uext32"})
    for (auto second : {"ext8", "ext16", "ext32", "uext8", "uext16", "uext32"})
      add(std::string(first) + " a, x\n" + second + " c, a\nret c");
  for (auto cmp : {"eq", "ne", "lt", "le", "gt", "ge", "ult", "ule", "ugt", "uge", "eqs", "lts"})
    for (auto ext : {"ext8", "uext32"}) add(std::string(cmp) + " a, x, y\n" + ext + " c, a\nret c");
  for (auto ext : {"uext8", "uext16", "uext32"}) for (auto op : {"and", "ands"}) {
    add(std::string(op) + " a, x, 131071\n" + ext + " c, a\nret c");
    add(std::string(op) + " a, 131071, x\n" + ext + " c, a\nret c");
  }
  add("alloca a, 64\nmov b, a\nadd q, a, 8\nmov i64:0(b), x\nmov i64:0(q), y\nmov c, i64:0(b)\nret c");
  add("mov a, x\nmov b, a\nmov c, b\nret c");
  add("alloca q, 64\nmov i64:0(q), x\nmov i64:8(q), y\nret y");
  add("mov i64:16(p), x\nmov i64:16(p), y\nret y");
  add("alloca q, 64\nmov i64:0(q), x\nmov i64:8(q), y\nmov c, i64:8(q)\nmov i64:24(p), c\nret c");
  add("alloca q, 64\nmov i64:0(q), x\nmov i32:4(q), y\nmov c, i64:0(q)\nret c");
  for (auto suffix : {":a", ":b", "::same"})
    add("mov i64:16(p):a, x\nmov i64:24(p)" + std::string(suffix) + ", y\nmov c, i64:24(p):a\nmov i64:16(p):a, y\nret c");
  add("alloca q, 64\nbt yes, y\nmov i64:0(q), x\njmp join\nyes:\nmov i64:0(q), y\njoin:\nmov c, i64:0(q)\nret c");
  add("alloca q, 64\nmov i, 0\nloop:\nmov i64:0(q), x\nmov i64:8(q), y\nadd i, i, 1\nblt loop, i, 5\nret i");
  // A stack address stored in global memory fully escapes; a call must keep it live.
  result.push_back(R"(m: module
state: i64 0
sig: proto i64, i64:p
read: func i64, i64:p
local i64:q, i64:r
mov q, i64:0(p)
mov r, i64:0(q)
ret r
endfunc
entry: func i64
local i64:p, i64:q, i64:r
mov p, state
alloca q, 16
mov i64:0(q), 42
mov i64:0(p), q
call sig, read, r, p
mov i64:0(p), 0
ret r
endfunc
endmodule
)");
  for (bool argument : {false, true}) result.push_back(std::string(R"(m: module
sig: proto i64, i64:p
read: func i64, i64:p
local i64:r
mov r, i64:0(p)
ret r
endfunc
state: i64 7
entry: func i64
local i64:p, i64:q, i64:r
mov p, state
alloca q, 16
mov i64:0(q), 42
call sig, read, r, )") + (argument ? "q" : "p") + "\nret r\nendfunc\nendmodule\n");
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  for (bool phi : {false, true}) result.push_back("m: module\nstate: i64 -12345, 257\nentry: func i64\nglobal i64:h:" + std::string(binding)
    + "\nlocal i64:p, i64:x, i64:y, i64:a, i64:b, i64:c\nmov p, state\nmov x, i64:0(p)\nmov y, i64:8(p)\n"
    + (phi ? "bt yes, y\nadd a, x, 1\njmp join\nyes:\nadd a, x, 2\njoin:\n" : "add a, x, 1\n")
    + "mov h, a\nadd b, a, x\nadd c, b, h\nret c\nendfunc\nendmodule\n");
  return result;
}
inline std::vector<std::string> random_memory_fixtures() {
  std::mt19937 random(0x19790321); std::vector<std::string> result;
  for (unsigned n = 0; n < 48; ++n) {
    std::string text = "m: module\nstate: i64 -12345, 257, 0, 41, 13, 5, 7, 9\nentry: func i64\nlocal i64:p, i64:q, i64:a, i64:b, i64:c, i64:i\nmov p, state\nalloca q, 64\nmov a, i64:0(p)\nmov i, 0\n";
    for (unsigned k = 0; k < 8; ++k) text += "mov i64:" + std::to_string(k * 8) + "(q), a\n";
    text += "loop:\n";
    for (unsigned k = 0; k < 12; ++k) {
      auto offset = std::to_string((random() % 8) * 8);
      if (random() & 1) text += "mov i64:" + offset + "(q), a\n";
      else text += "mov b, i64:" + offset + "(q)\nmul c, b, " + std::to_string(1u << (random() % 9)) + "\nuext32 a, c\n";
    }
    text += "add i, i, 1\nblt loop, i, 3\nmov b, i64:0(q)\nmov i64:16(p), b\nret a\nendfunc\nendmodule\n";
    result.push_back(std::move(text));
  }
  return result;
}
} // namespace mirnext::test
