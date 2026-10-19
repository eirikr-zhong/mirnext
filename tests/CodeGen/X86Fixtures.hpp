/* Licensed under the MIT License. x86 boundaries shared with the C oracle. */
#pragma once
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> x86_lowering_fixtures(bool windows) {
  std::vector<std::string> cases;
  for (auto type : {"i8","u16","i32","i64","f","d","ld"}) for (unsigned count : {1,4,5,6,7,8,9,17}) {
    auto t = std::string(type); std::string parameters,args;
    for (unsigned n = 0; n < count; ++n) { parameters += ',' + t + ":a" + std::to_string(n); args += ",a" + std::to_string(n); }
    cases.push_back("m:module\np:proto " + t + parameters + "\nimport ext\nf:func " + t + parameters
      + "\nlocal " + t + ":r\ncall p,ext,r" + args + "\nret r\nendfunc\nendmodule\n");
  }
  for (auto prefix_type : {"i64","d"}) for (auto classification : {"blk","blk1","blk2","blk3","blk4"}) for (unsigned count : {0,3,4,5,6,8}) {
    for (unsigned size : {8,16}) {
      auto cls = std::string(classification); if (size == 8 && (cls == "blk3" || cls == "blk4")) continue;
      std::string parameters,args;
      for (unsigned n = 0; n < count; ++n) { parameters += ',' + std::string(prefix_type) + ":a" + std::to_string(n); args += ",a" + std::to_string(n); }
      auto block = cls + ':' + std::to_string(size) + "(b)";
      cases.push_back("m:module\np:proto i64" + parameters + ',' + block + ",i64:last\nimport ext\nf:func i64" + parameters + ',' + block
        + ",i64:last\nlocal i64:r\ncall p,ext,r" + args + ',' + block + ",last\nret r\nendfunc\nendmodule\n");
    }
  }
  for (unsigned size : {1,9,17,24,128,129,256}) {
    auto block = "blk:" + std::to_string(size) + "(b)";
    cases.push_back("m:module\np:proto i64,i64:x," + block + "\nimport ext\nf:func i64,i64:x," + block
      + "\nlocal i64:r\ncall p,ext,r,x," + block + "\nret r\nendfunc\nendmodule\n");
  }
  cases.push_back("m:module\np:proto rblk:24(b),i64:x\nimport ext\nf:func rblk:24(b),i64:x\ncall p,ext,rblk:24(b),x\nret\nendfunc\nendmodule\n");
  cases.push_back("m:module\np:proto i64,i64:x,...\nimport ext\nf:func i64,i64:x,d:d,blk:16(b)\nlocal i64:r\ncall p,ext,r,x,x,d,blk:16(b),x\nret r\nendfunc\nendmodule\n");
  cases.push_back("m:module\nf:func i64,i64:x,...\nlocal i64:list,i64:r,i64:block\nalloca list,64\nalloca block,32\nva_start list\nva_arg r,list,i64:0\nva_block_arg block,list,24,0\nva_end list\nret r\nendfunc\nendmodule\n");
  cases.push_back("m:module\np:proto i64,i64:x,blk:16(b),blk:136(c),blk:256(d)\nimport ext\nf:func i64,i64:x,blk:16(b),blk:136(c),blk:256(d)\nlocal i64:r\ncall p,ext,r,x,blk:16(b),blk:136(c),blk:256(d)\nret r\nendfunc\nendmodule\n");
  cases.push_back("m:module\np:proto i64,i8:x\nimport ext\nf:func i64,i8:x\nlocal i64:ptr,i64:r,i64:saved,i64:area\nmov ptr,ext\nbstart saved\nalloca area,x\ncall p,ptr,r,x\nbend saved\nret r\nendfunc\nendmodule\n");
  if (!windows) cases.push_back("m:module\np:proto i8,d,ld,i64:x\nimport ext\nf:func i8,d,ld,i64:x\nlocal i64:r,d:s,ld:t\ncall p,ext,r,s,t,x\nret r,s,t\nendfunc\nendmodule\n");
  return cases;
}
}
