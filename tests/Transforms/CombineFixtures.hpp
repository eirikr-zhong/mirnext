/* Licensed under the MIT License. Comparison, address and conventional-PHI combine boundaries. */
#pragma once
#include <random>
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> combine_fixtures () {
  std::vector<std::string> result;
  for (auto compare : {"eq",  "eqs",  "ne", "nes", "lt",  "lts",  "ult", "ults", "le",  "les",
                       "ule", "ules", "gt", "gts", "ugt", "ugts", "ge",  "ges",  "uge", "uges"})
    for (auto branch : {"bt", "bf", "bts", "bfs"})
      result.push_back ("m: module\nentry: func i64, i64:x, i64:y\nlocal i64:c\n"
                        + std::string (compare) + " c, x, y\n" + branch
                        + " yes, c\nret 0\nyes:\nret 1\nendfunc\nendmodule\n");
  for (auto type : {"f", "d", "ld"})
    for (auto compare : {"eq", "ne", "lt", "le", "gt", "ge"})
      for (auto branch : {"bt", "bf"})
        result.push_back ("m: module\nentry: func i64, " + std::string (type) + ":x, " + type
                          + ":y\nlocal i64:c\n" + type + compare + " c, x, y\n" + branch
                          + " yes, c\nret 0\nyes:\nret 1\nendfunc\nendmodule\n");
  auto address = [&] (std::string body, std::string type = "i64") {
    result.push_back ("m: module\nentry: func i64, i64:p, i64:x\nlocal i64:a, i64:b, i64:c, i64:r\n"
                      + body + "\nmov r, " + type
                      + ":0(c):object:other\nret r\nendfunc\nendmodule\n");
  };
  for (auto offset : {0, 1, 3, 6, 255, 12285, 12288, 2147483647}) {
    address ("add c, p, " + std::to_string (offset));
    address ("sub c, p, " + std::to_string (offset));
    address ("add a, p, 3\nadd c, a, " + std::to_string (offset));
  }
  for (auto scale : {0, 1, 2, 3, 4, 8, 255, 256, -1}) {
    address ("mul a, x, " + std::to_string (scale) + "\nadd c, p, a");
    address ("mul a, " + std::to_string (scale) + ", x\nadd c, a, p");
  }
  for (auto shift : {0, 1, 2, 3, 7, 8, 63, 64, -1})
    address ("lsh a, x, " + std::to_string (shift) + "\nadd c, p, a");
  address ("add c, p, p");
  address ("mov c, p");
  address ("add c, p, 1", "u8");
  result.push_back (
    "m: module\nentry: func i64, i64:p, i64:x\nlocal i64:a, i64:i, i64:r\nmov i, 0\nmov a, "
    "p\nloop:\nadd a, a, 8\nadd i, i, 1\nblt loop, i, x\nmov r, i64:0(a)\nret "
    "r\nendfunc\nendmodule\n");
  result.push_back (
    "m: module\nentry: func i64, i64:p, i64:x\nlocal i64:a, i64:r\nbt yes, x\nadd a, p, 3\njmp "
    "done\nyes:\nadd a, p, 6\ndone:\nmov r, i64:0(a)\nret r\nendfunc\nendmodule\n");
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  result.push_back ("m: module\nentry: func i64, i64:p, i64:x\nglobal i64:h:"
                    + std::string (binding)
                    + "\nlocal i64:r\nadd h, p, 3\nmov r, i64:0(h)\nret r\nendfunc\nendmodule\n");
  result.push_back ("m: module\nentry: func i64, i64:x, i64:y\nglobal i64:h:"
                    + std::string (binding)
                    + "\ngt h, x, y\nbt yes, h\nret 0\nyes:\nret h\nendfunc\nendmodule\n");
  return result;
}
inline std::vector<std::string> random_combine_fixtures () {
  std::mt19937 random (0x20261016);
  std::vector<std::string> result;
  for (unsigned n = 0; n < 64; ++n) {
    std::string text
      = "m: module\nstate: i64 7, 11, 13, 17, 19, 23, 29, 31\nentry: func i64\nlocal i64:p, i64:x, "
        "i64:a, i64:b, i64:r, i64:c\nmov p, state\nmov x, i64:0(p)\n";
    auto offset = (random () % 7) * 8;
    text += "add a, p, " + std::to_string (offset) + "\nadd b, a, " + std::to_string (56 - offset)
            + "\nmov r, i64:0(b)\n";
    const char *compares[]{"eq", "ne", "lt", "ult", "le", "ule", "gt", "ugt", "ge", "uge"};
    text += std::string (compares[random () % 10]) + " c, r, x\n" + (random () & 1 ? "bt" : "bf")
            + " yes, c\nret r\nyes:\nadd r, r, x\nret r\nendfunc\nendmodule\n";
    result.push_back (std::move (text));
  }
  return result;
}
}  // namespace mirnext::test
