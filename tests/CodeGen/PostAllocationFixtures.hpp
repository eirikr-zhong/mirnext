/* Licensed under the MIT License. Inputs preserved until machine optimization. */
#pragma once
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> post_allocation_fixtures () {
  std::vector<std::string> cases;
  for (auto a : {"ext8", "uext8", "ext16", "uext16", "ext32", "uext32"})
    for (auto b : {"ext8", "uext8", "ext16", "uext16", "ext32", "uext32"})
      cases.push_back (std::string ("m:module\nf:func i64,i64:x\nlocal i64:a,i64:b\n") + a
                       + " a,x\n" + b + " b,a\nret b\nendfunc\nendmodule\n");
  for (bool later : {false, true})
    for (bool swap : {false, true}) {
      auto text
        = std::string ("m:module\nf:func i64,i64:x\nlocal i64:a,i64:b,i64:unused\nmov a,42\nadd b,")
          + (swap ? "a,x" : "x,a") + "\nadd unused,x,17\n";
      if (later) text += "mov i64:(x),a\n";
      cases.push_back (text + "ret b\nendfunc\nendmodule\n");
    }
  for (auto op : {"addo", "addos", "subo", "subos", "mulo", "mulos", "umulo", "umulos"})
    for (auto branch : {"bo", "bno", "ubo", "ubno"}) {
      if ((op[0] == 'u') != (branch[0] == 'u')) continue;
      cases.push_back (std::string ("m:module\nf:func i64,i64:x,i64:y\nlocal i64:a,i64:r\n") + op
                       + " a,x,y\nmov r,x\n" + branch
                       + " Finish\nFinish:\nret r\nendfunc\nendmodule\n");
    }
  for (auto branch : {"prbeq", "prbne"})
    for (auto value : {"0", "1"})
      cases.push_back (
        std::string ("m:module\nf:func i64,i64:x\nlocal i64:r\nprset x,1\n") + branch + " Finish,x,"
        + value + "\nmov r,17\njmp Done\nFinish:\nmov r,42\nDone:\nret r\nendfunc\nendmodule\n");
  return cases;
}
}  // namespace mirnext::test
