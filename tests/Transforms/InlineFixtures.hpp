/* Licensed under the MIT License. Inlining decisions, frames and symbol boundaries. */
#pragma once
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> inline_fixtures() {
  std::vector<std::string> cases;
  auto wrap = [&](std::string body) { cases.push_back("m:module\np:proto i64\n" + body + "endmodule\n"); };
  for (auto opcode : {"call", "inline"}) for (unsigned size : {49,50,51,199,200,201}) {
    std::string body = "leaf:func i64\nlocal i64:x\n";
    for (unsigned n = 1; n < size; ++n) body += "mov x, " + std::to_string(n) + "\n";
    body += "ret x\nendfunc\nentry:func i64\nlocal i64:r\n" + std::string(opcode) + " p, leaf, r\nret r\nendfunc\n";
    wrap(body);
  }
  for (unsigned calls : {1,4,51,100}) {
    std::string body = "leaf:func i64\nlocal i64:x\nmov x, 42\nret x\nendfunc\nentry:func i64\nlocal i64:r\n";
    for (unsigned n = 0; n < calls; ++n) body += "call p, leaf, r\n";
    wrap(body + "ret r\nendfunc\n");
  }
  for (int size : {0,1,2,3,5,9,16,33}) {
    wrap("leaf:func i64\nlocal i64:a, i64:x\nalloca a, " + std::to_string(size)
      + "\nmov i8:(a), 42\nmov x, i8:(a)\nret x\nendfunc\nentry:func i64\nlocal i64:a, i64:r\nalloca a, 24\nmov i64:(a), 42\ncall p, leaf, r\ncall p, leaf, r\nret r\nendfunc\n");
  }
  wrap("leaf:func i64\nlocal i64:a, i64:x\nalloca a, 16\nmov x, 42\nret x\nendfunc\nentry:func i64\nlocal i64:a, i64:r\nalloca a, 16\ncall p, leaf, r\nret r\nendfunc\n");
  wrap("q:proto i64, i64:n\nleaf:func i64, i64:n\nlocal i64:a, i64:x\nalloca a, n\nmov i64:(a), 42\nmov x, i64:(a)\nret x\nendfunc\nentry:func i64\nlocal i64:r\ncall q, leaf, r, 16\nret r\nendfunc\n");
  wrap("leaf:func i64\nlocal i64:a, i64:b, i64:x\nalloca a, 16\nmov i64:(a), 42\nL:alloca b, 32\nmov i64:(b), 1\nmov x, i64:(a)\nret x\nendfunc\nentry:func i64\nlocal i64:r\ncall p, leaf, r\nret r\nendfunc\n");
  wrap("leaf:func i64\nlocal i64:a, i64:x\nalloca a, 16\nmov i64:(a), 42\nmov x, i64:(a)\nret x\nendfunc\nmid:func i64\nlocal i64:a, i64:r\nalloca a, 24\nmov i64:(a), 0\ncall p, leaf, r\nret r\nendfunc\nentry:func i64\nlocal i64:r\ncall p, mid, r\ncall p, mid, r\nret r\nendfunc\n");
  wrap("q:proto i64, i64:n\nleaf:func i64, i64:n\nlocal i64:x\nbf done, n\nmov x, 40\nret x\ndone:mov x, 42\nret x\nendfunc\nentry:func i64\nlocal i64:r\ncall q, leaf, r, 0\nret r\nendfunc\n");
  wrap("leaf:func i64\nlocal i64:x\njmp start\ncold:mov x, 0\njmp end\nstart:mov x, 42\nend:ret x\nextra:mov x, 13\njmp cold\nendfunc\nentry:func i64\nlocal i64:r\ncall p, leaf, r\ncall p, leaf, r\nret r\nendfunc\n");
  wrap("entry:func i64\nlocal i64:r\ncall p, entry, r\nret r\nendfunc\n");
  wrap("a:func i64\nlocal i64:r\ncall p, b, r\nret r\nendfunc\nb:func i64\nlocal i64:r\ncall p, a, r\nret r\nendfunc\n");
  wrap("leaf:func i64\nlocal i64:x\naddress:mov x, 42\nret x\nendfunc\nanchor:lref address, 0\nentry:func i64\nlocal i64:r\ncall p, leaf, r\nret r\nendfunc\n");
  wrap("q:proto i8, i8:n\nleaf:func i8, i8:n\nret n\nendfunc\nentry:func i64\nlocal i64:r\ncall q, leaf, r, 42\nret r\nendfunc\n");
  for (auto kind : {"blk", "blk1", "blk2", "blk3", "blk4"}) {
    auto type = std::string(kind);
    wrap("q:proto i64, " + type + ":16(a)\nleaf:func i64, " + type + ":16(a)\nlocal i64:x\nmov x, i64:(a)\nret x\nendfunc\nentry:func i64\nlocal i64:a, i64:r\nalloca a, 16\nmov i64:(a), 42\ncall q, leaf, r, " + type + ":16(a)\nret r\nendfunc\n");
  }
  cases.push_back("a:module\nexport leaf\ndata:i64 35\ni64 7\nleaf:func i64\nlocal i64:a, i64:r\nmov a, data\nmov r, i64:(a)\nadd r, r, i64:8(a)\nret r\nendfunc\nendmodule\nb:module\nimport leaf\np:proto i64\nentry:func i64\nlocal i64:r\ncall p, leaf, r\nret r\nendfunc\nendmodule\n");
  cases.push_back("a:module\nexport leaf\np:proto i64\nhelper:func i64\nlocal i64:x\n");
  for (unsigned n = 0; n < 51; ++n) cases.back() += "mov x, " + std::to_string(n) + "\n";
  cases.back() += "ret x\nendfunc\nleaf:func i64\nlocal i64:r\ncall p, helper, r\nret r\nendfunc\nendmodule\nb:module\nimport leaf\np:proto i64\nhelper:func i64\nlocal i64:x\nmov x, 1\nret x\nendfunc\nentry:func i64\nlocal i64:r\ncall p, leaf, r\nret r\nendfunc\nendmodule\n";
  return cases;
}
}
