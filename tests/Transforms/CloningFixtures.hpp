/* Licensed under the MIT License. Focused MIR hot/cold cloning programs. */
#pragma once
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> cloning_fixtures() {
  std::vector<std::string> result;
  for (unsigned cold : {1u, 2u, 4u}) {
    std::string text = "m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\n";
    for (unsigned n = 0; n < cold; ++n) text += "beq cold" + std::to_string(n) + ", c, " + std::to_string(n + 1) + "\n";
    text += "hot:\nadd v, v, 1\nblt hot, v, 14\nret v\n";
    for (unsigned n = 0; n < cold; ++n) text += "cold" + std::to_string(n) + ":\nmov v, " + std::to_string(n + 10) + "\njmp hot\n";
    result.push_back(text + "endfunc\nendmodule\n");
  }
  for (unsigned padding : {0u, 1u, 4u, 12u}) {
    std::string text = "m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\n";
    for (unsigned n = 0; n < padding; ++n) text += "add v, v, 1\n";
    text += "blt hot, v, 14\nret v\ncold:\nmov v, 14\njmp hot\nendfunc\nendmodule\n";
    result.push_back(text);
  }
  for (unsigned padding : {4u, 5u, 6u, 7u}) {
    std::string text = "m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\n";
    for (unsigned n = 0; n < padding; ++n) text += "add v, v, c\n";
    text += "jmp next\nnext:\nadd v, v, c\njmp final\nfinal:\n";
    for (unsigned n = 0; n < 20; ++n) text += "pad" + std::to_string(n) + ":\n";
    text += "ret v\ncold:\nmov v, 10\njmp hot\nextra:\nmov v, 0\njmp hot\nendfunc\nextra_ref: lref extra\nnext_ref: lref next\nfinal_ref: lref final\n";
    for (unsigned n = 0; n < 20; ++n) text += "pad_ref" + std::to_string(n) + ": lref pad" + std::to_string(n) + "\n";
    text += "endmodule\n";
    result.push_back(text);
  }
  auto labeled = result.front();
  labeled.insert(labeled.find("mov v, 0"), "start:\n"); result.push_back(labeled);
  auto unreachable = result.front();
  unreachable.insert(unreachable.find("hot:\n"), "jmp hot\ndead:\nadd v, v, 99\njmp hot\n"); result.push_back(unreachable);
  result.push_back(R"(m: module
step: func i64, i64:x
local i64:r
add r, x, 1
ret r
endfunc
signature: proto i64, i64:x
entry: func i64
local i64:v, i64:c
mov v, 0
mov c, 1
bt cold, c
hot:
call signature, step, v, v
blt hot, v, 14
ret v
cold:
mov v, 10
jmp hot
endfunc
endmodule
)");
  result.push_back(R"(m: module
entry: func i64
local d:d, i64:v, i64:c
mov v, 0
mov c, 1
bt cold, c
hot:
dadd d, d, 1.0
dblt hot, d, 14.0
d2i v, d
ret v
cold:
dmov d, 10.0
jmp hot
endfunc
endmodule
)");
  auto hardware = result.front();
#if defined(__aarch64__) || defined(_M_ARM64)
  hardware.replace(hardware.find("local i64:v, i64:c"), 18, "global i64:v:r19\nlocal i64:c");
#else
  hardware.replace(hardware.find("local i64:v, i64:c"), 18, "global i64:v:r12\nlocal i64:c");
#endif
  result.push_back(hardware);
  result.push_back(R"(m: module
entry: func i64
local i64:v, i64:c
mov v, 0
mov c, 1
bt cold, c
hot:
add v, v, 3
fall:
add v, v, 1
ret v
cold:
mov v, 10
jmp hot
endfunc
anchor: lref fall
endmodule
)");
  result.push_back(R"(m: module
entry: func i64
local i64:v
mov v, 0
hot:
bt cold, v
ret v
cold:
add v, v, 1
jmp hot
endfunc
endmodule
)");
  return result;
}
inline std::vector<std::string> random_cloning_fixtures() {
  std::vector<std::string> result;
  unsigned seed = 0x7b9382u;
  auto random = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
  for (unsigned sample = 0; sample < 64; ++sample) {
    auto cold = 1 + random() % 6, selected = 1 + random() % cold, increment = 1 + random() % 4;
    std::string text = "m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, " + std::to_string(selected) + "\n";
    for (unsigned n = 0; n < cold; ++n) text += "beq cold" + std::to_string(n) + ", c, " + std::to_string(n + 1) + "\n";
    text += "hot:\nadd v, v, " + std::to_string(increment) + "\nblt hot, v, 32\nret v\n";
    for (unsigned n = 0; n < cold; ++n) text += "cold" + std::to_string(n) + ":\nmov v, " + std::to_string(random() % 32) + "\njmp hot\n";
    result.push_back(text + "endfunc\nendmodule\n");
  }
  return result;
}
inline std::vector<std::string> cloning_control_fixtures() {
  std::vector<std::string> result;
  for (auto opcode : {"prbeq", "prbne"}) result.push_back("m: module\nentry: func i64\nlocal i64:v, i64:c\nmov v, 0\nmov c, 1\nbt cold, c\nhot:\nprset v, 7\n"
    + std::string(opcode) + " yes, v, 7\nret 11\nyes:\nret 42\ncold:\nmov v, 1\njmp hot\nendfunc\nendmodule\n");
  result.push_back(R"(m: module
entry: func i64
local i64:v, i64:c, i64:p
mov v, 0
mov c, 1
bt cold, c
hot:
laddr p, yes
jmpi p
ret 11
yes:
ret 42
cold:
mov v, 1
jmp hot
endfunc
anchor: lref yes
endmodule
)");
  return result;
}
} // namespace mirnext::test
