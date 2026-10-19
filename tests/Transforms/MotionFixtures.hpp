/* Licensed under the MIT License. Loop motion, dead SSA and pressure boundaries. */
#pragma once
#include <random>
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> motion_fixtures() {
  std::vector<std::string> result;
  auto add = [&](std::string body) {
    result.push_back("m: module\nstate: i64 7, 11, 3, 0\nentry: func i64\nlocal i64:p, i64:x, i64:y, i64:a, i64:b, i64:c, i64:d, i64:i, i64:j, i64:sum\nmov p, state\nmov x, i64:0(p)\nmov y, i64:8(p)\nmov i, 0\nmov sum, 0\n" + body + "\nendfunc\nendmodule\n");
  };
  for (bool conditional : {false, true}) {
    auto prefix = conditional ? "bt done, x\n" : "";
    add(std::string(prefix) + "loop:\nmul a, x, y\nadd b, a, x\nadd sum, sum, b\nadd i, i, 1\nblt loop, i, 4\ndone:\nret sum");
    add(std::string(prefix) + "loop:\nadd a, x, y\nmul b, a, x\nsub c, a, y\nadd d, c, b\nadd sum, sum, d\nadd i, i, 1\nblt loop, i, 4\ndone:\nret sum");
  }
  for (auto op : {"div", "divs", "udiv", "udivs", "mod", "mods", "umod", "umods"})
    add("loop:\n" + std::string(op) + " a, x, y\nadd sum, sum, a\nadd i, i, 1\nblt loop, i, 4\nret sum");
  add("loop:\nmov a, i64:0(p)\nadd b, a, y\nadd sum, sum, b\nmov i64:16(p), sum\nadd i, i, 1\nblt loop, i, 4\nret sum");
  add("outer:\nmov j, 0\ninner:\nmul a, x, y\nadd b, a, x\nadd sum, sum, b\nadd j, j, 1\nblt inner, j, 3\nadd i, i, 1\nblt outer, i, 2\nret sum");
  add("jmp loop\nbefore:\nadd sum, sum, x\njmp done\nloop:\nmul a, x, y\nadd sum, sum, a\nadd i, i, 1\nblt loop, i, 4\njmp before\ndone:\nret sum");
  add("bt other, x\nloop:\nmul a, x, y\nadd sum, sum, a\nadd i, i, 1\nblt loop, i, 4\nret sum\nother:\nsub x, x, 1\njmp loop");
  add("mov a, 42\nbt yes, x\nret x\nyes:\nadd b, a, y\nret b");
  add("loop:\nmov a, 42\nadd sum, sum, a\nadd i, i, 1\nblt loop, i, 4\nret sum");
  add("mov j, i64:24(p)\nswitch j, loop, done\nloop:\nmul a, x, y\nadd sum, sum, a\nadd i, i, 1\nblt loop, i, 4\ndone:\nret sum");
  result.push_back("m: module\nentry: func i64, i64:x, i64:y, i64:z\nlocal i64:a\nloop:\nmul a, y, z\nsub x, x, 1\nbgt loop, x, 0\nret a\nendfunc\nendmodule\n");
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  result.push_back("m: module\nstate: i64 7, 11\nentry: func i64\nglobal i64:h:" + std::string(binding)
    + "\nlocal i64:p, i64:x, i64:y, i64:i, i64:sum\nmov p, state\nmov x, i64:0(p)\nmov y, i64:8(p)\nmov sum, 0\nmov i, 0\nloop:\nmul h, x, y\nadd sum, sum, h\nadd i, i, 1\nblt loop, i, 4\nret sum\nendfunc\nendmodule\n");
  result.push_back("m: module\nentry: func i64, i64:x\nglobal i64:h:" + std::string(binding)
    + "\nmov h, 42\nbt yes, x\nyes:\nret x\nendfunc\nendmodule\n");
  return result;
}
inline std::vector<std::string> random_motion_fixtures() {
  std::mt19937 random(0x20030205); std::vector<std::string> result;
  for (unsigned n = 0; n < 64; ++n) {
    std::string text = "m: module\nstate: i64 7, 11\nentry: func i64\nlocal i64:p, i64:x, i64:y, i64:i, i64:sum";
    for (unsigned k = 0; k < 16; ++k) text += ", i64:t" + std::to_string(k);
    text += "\nmov p, state\nmov x, i64:0(p)\nmov y, i64:8(p)\nmov i, 0\nmov sum, 0\n";
    if (n & 1) text += "bt done, x\n";
    text += "loop:\n";
    for (unsigned k = 0; k < 16; ++k) {
      auto source = k && (random() & 1) ? "t" + std::to_string(random() % k) : "x";
      auto source2 = k && (random() & 1) ? "t" + std::to_string(random() % k) : "y";
      const char *ops[]{"add", "sub", "mul", "xor", "and", "or"};
      text += std::string(ops[random() % 6]) + " t" + std::to_string(k) + ", " + source + ", " + source2 + "\n";
      if (random() & 1) text += "add sum, sum, t" + std::to_string(k) + "\n";
    }
    text += "add i, i, 1\nblt loop, i, 3\ndone:\nret sum\nendfunc\nendmodule\n"; result.push_back(std::move(text));
  }
  return result;
}
}
