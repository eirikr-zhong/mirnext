/* Licensed under the MIT License. Shared canonical and independent jump inputs. */
#pragma once
#include <random>
#include <sstream>
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> jump_fixtures() {
  std::vector<std::string> inputs{
    "m: module\nentry: func i64\njmp done\ndone:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64\njmp first\nfirst:\nsecond:\nthird:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64\njmp first\nfirst:\njmp second\nsecond:\njmp third\nthird:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64, i64:c\nbt done, c\nret 0\ndone:\nret 42\ndead:\nret 99\nendfunc\nendmodule\n",
    "m: module\nentry: func i64, i64:c\nswitch c, first, first, second\nfirst:\njmp second\nsecond:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64\nlocal i64:p\nladdr p, first\njmpi p\nfirst:\nsecond:\nret 42\nendfunc\ndiff: lref first, second, 7\nendmodule\n",
    "m: module\nentry: func i64\nlocal i64:p\nladdr p, first\nret 42\nfirst:\nsecond:\njmp done\ndone:\nret 13\nendfunc\nanchor: lref first\nendmodule\n",
    "m: module\nentry: func i64, i64:c\nbt done, c\njmp first\nfirst:\njmp second\nsecond:\njmp first\ndone:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64, i64:c\nbt done, c\nloop:\njmp loop\ndone:\nret 42\nendfunc\nendmodule\n",
    "m: module\nentry: func i64\njmp done\ndead1:\njmp dead2\ndead2:\njmp dead1\ndone:\nret 42\nendfunc\nendmodule\n"
  };
  for (auto code : {"bt", "bf", "bts", "bfs"})
    inputs.push_back("m: module\nentry: func i64, i64:c\n" + std::string(code) + " done, c\ndone:\nret 42\nendfunc\nendmodule\n");
  for (auto code : {"beq", "bne", "blt", "ble", "bgt", "bge", "ublt", "uble", "ubgt", "ubge",
                    "beqs", "bnes", "blts", "bles", "bgts", "bges", "ublts", "ubles", "ubgts", "ubges"})
    inputs.push_back("m: module\nentry: func i64, i64:x, i64:y\n" + std::string(code) + " done, x, y\ndone:\nret 42\nendfunc\nendmodule\n");
  for (auto prefix : {"f", "d", "ld"}) for (auto code : {"beq", "bne", "blt", "ble", "bgt", "bge"})
    inputs.push_back("m: module\nentry: func i64, " + std::string(prefix) + ":x, " + prefix + ":y\n"
      + prefix + code + " done, x, y\ndone:\nret 42\nendfunc\nendmodule\n");
  for (auto code : {"prbeq", "prbne"})
    inputs.push_back("m: module\nentry: func i64, i64:c\nprset c, 1\n" + std::string(code) + " first, c, 1\nfirst:\njmp second\nsecond:\nret 42\nendfunc\nendmodule\n");
  for (auto code : {"bo", "bno", "ubo", "ubno"})
    inputs.push_back("m: module\nentry: func i64, i64:x, i64:y\nlocal i64:r\naddo r, x, y\n" + std::string(code) + " done\ndone:\nret r\nendfunc\nendmodule\n");
  return inputs;
}
inline std::vector<std::string> random_jump_fixtures() {
  std::mt19937 random(0x20261017); std::vector<std::string> inputs;
  for (unsigned n = 0; n < 64; ++n) {
    unsigned blocks = 4 + random() % 13; std::ostringstream out;
    out << "m: module\nstate: i64 " << random() % 2 << ", 0\nentry: func i64\nlocal i64:p, i64:c, i64:v\nmov p, state\nmov c, i64:0(p)\nmov v, 0\n";
    for (unsigned b = 0; b < blocks; ++b) {
      out << 'b' << b << ":\n";
      if (random() % 2) out << "alias" << b << ":\n";
      out << "add v, v, " << random() % 97 << '\n';
      if (b + 1 == blocks) out << "mov i64:8(p), v\nret v\n";
      else {
        auto target = b + 1 + random() % (blocks - b - 1);
        if (random() % 3) out << (random() % 2 ? "bt" : "bf") << " b" << target << ", c\n";
        else out << "switch c, b" << target << ", b" << b + 1 << ", b" << target << '\n';
        out << "bridge" << b << ":\njmp b" << b + 1 << '\n';
      }
    }
    out << "endfunc\nendmodule\n"; inputs.push_back(out.str());
  }
  return inputs;
}
}
