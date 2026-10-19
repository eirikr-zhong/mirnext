/* Licensed under the MIT License. AArch64 ABI boundary fixtures. */
#pragma once
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> aarch64_lowering_fixtures () {
  std::vector<std::string> cases;
  for (auto type : {"i8", "u16", "i32", "i64", "f", "d", "ld"})
    for (unsigned count : {1, 7, 8, 9, 12}) {
      auto t = std::string (type);
      std::string parameters, args;
      for (unsigned i = 0; i < count; ++i) {
        parameters += "," + t + ":a" + std::to_string (i);
        args += ",a" + std::to_string (i);
      }
      cases.push_back ("m:module\np:proto " + t + parameters + "\nimport ext\nf:func " + t
                       + parameters + "\nlocal " + t + ":r\ncall p,ext,r" + args
                       + "\nret r\nendfunc\nendmodule\n");
    }
  for (unsigned count : {0, 7, 8, 9})
    for (unsigned size : {1, 8, 9, 16, 17, 128, 129, 136}) {
      std::string parameters, args;
      for (unsigned i = 0; i < count; ++i) {
        parameters += ",i64:a" + std::to_string (i);
        args += ",a" + std::to_string (i);
      }
      auto block = "blk:" + std::to_string (size) + "(b)";
      cases.push_back ("m:module\np:proto i64" + parameters + ',' + block
                       + "\nimport ext\nf:func i64" + parameters + ',' + block
                       + "\nlocal i64:r\ncall p,ext,r" + args + ',' + block
                       + "\nret r\nendfunc\nendmodule\n");
    }
  for (auto type : {"blk", "blk1", "blk2", "blk3", "blk4"}) {
    auto block = std::string (type) + ":16(b)";
    cases.push_back ("m:module\np:proto i64," + block + "\nimport ext\nf:func i64," + block
                     + "\nlocal i64:r\ncall p,ext,r," + block + "\nret r\nendfunc\nendmodule\n");
  }
  cases.push_back (
    "m:module\np:proto rblk:24(b),i64:x\nimport ext\nf:func rblk:24(b),i64:x\ncall "
    "p,ext,rblk:24(b),x\nret\nendfunc\nendmodule\n");
  cases.push_back (
    "m:module\np:proto i8,d,ld,i64:x\nimport ext\nf:func i8,d,ld,i64:x\nlocal i64:r,d:s,ld:t\ncall "
    "p,ext,r,s,t,x\nret r,s,t\nendfunc\nendmodule\n");
  cases.push_back (
    "m:module\np:proto i64,i64:x,...\nimport ext\nf:func i64,i64:x,d:d,blk:16(b)\nlocal "
    "i64:r\ncall p,ext,r,x,x,d,blk:16(b),x\nret r\nendfunc\nendmodule\n");
  cases.push_back (
    "m:module\nf:func i64,i64:x,...\nlocal i64:list,i64:r,i64:block\nalloca list,64\nalloca "
    "block,32\nva_start list\nva_arg r,list,i64:0\nva_block_arg block,list,24,0\nva_end list\nret "
    "r\nendfunc\nendmodule\n");
  // Large copies call a builtin after earlier argument registers are populated.
  cases.push_back (
    "m:module\np:proto i64,i64:x,blk:16(b),blk:136(c),blk:256(d)\nimport ext\nf:func "
    "i64,i64:x,blk:16(b),blk:136(c),blk:256(d)\nlocal i64:r\ncall "
    "p,ext,r,x,blk:16(b),blk:136(c),blk:256(d)\nret r\nendfunc\nendmodule\n");
  return cases;
}
inline std::string aarch64_large_argument_fixture () {
  std::string parameters, arguments;
  for (unsigned n = 0; n < 4110; ++n) {
    parameters += ",i64:a" + std::to_string (n);
    arguments += ",a" + std::to_string (n);
  }
  return "m:module\np:proto i64" + parameters + "\nimport ext\nf:func i64" + parameters
         + "\nlocal i64:r\ncall p,ext,r" + arguments + "\nret r\nendfunc\nendmodule\n";
}
}  // namespace mirnext::test
