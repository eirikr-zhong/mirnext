/* Licensed under the MIT License. Local-address transformation fixtures.
 */
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace mirnext::test {
inline std::string stored_pointer_address_fixture (bool direct_use = false) {
  std::string text = R"(m: module
read: func i64, i64:q
local i64:p, i64:r
mov p, i64:0(q)
mov r, i64:0(p)
ret r
endfunc
signature: proto i64, i64:q
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 42
mov p, 0
addr q, p
addr p, v
call signature, read, r, q
ret r
endfunc
endmodule
)";
  if (direct_use) text.insert (text.find ("call signature"), "sub r, p, p\n");
  return text;
}
inline std::vector<std::string> pointer_alias_address_fixtures () {
  return {
    R"(m: module
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 42
mov p, 0
addr q, p
addr p, v
mov r, i64:0(q)
mov r, i64:0(r)
ret r
endfunc
endmodule
)",
    R"(m: module
entry: func i64
local i64:p, i64:r
mov p, 0
addr p, p
mov r, i64:0(p)
sub r, r, p
ret r
endfunc
endmodule
)"};
}
inline std::vector<std::string> address_fixtures () {
  std::vector<std::string> result;
  for (auto type : {"i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64"}) {
    std::string address = type == std::string ("i8") || type == std::string ("u8")     ? "addr8"
                          : type == std::string ("i16") || type == std::string ("u16") ? "addr16"
                          : type == std::string ("i32") || type == std::string ("u32") ? "addr32"
                                                                                       : "addr";
    result.push_back ("m: module\nentry: func i64\nlocal i64:v, i64:p, i64:q, i64:r\nmov v, 7\n"
                      + address + " p, v\nmov q, p\nmov " + type + ":0(q), 255\nmov r, " + type
                      + ":0(p)\nret r\nendfunc\nendmodule\n");
  }
  result.push_back (R"(m: module
write: func i64, i64:p
mov i64:0(p), 99
ret 0
endfunc
sig: proto i64, i64:p
entry: func i64
local i64:v, i64:p, i64:r
mov v, 7
addr p, v
call sig, write, r, p
add v, v, v
ret v
endfunc
endmodule
)");
  result.push_back (R"(m: module
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 1311768464867721282
addr p, v
mov r, u32:4(p)
ret r
endfunc
endmodule
)");
  result.push_back (R"(m: module
entry: func i64, i64:choice
local i64:v, i64:p, i64:q, i64:r
mov v, 7
addr p, v
bt other, choice
mov q, p
jmp join
other:
mov q, p
join:
mov i64:0(q), 42
mov r, i64:0(p)
ret r
endfunc
endmodule
)");
  result.push_back (R"(m: module
entry: func i64
local i64:v, i64:p, i64:q, i64:r, i64:i
mov v, 0
mov i, 0
addr p, v
loop:
mov q, p
mov r, i64:0(q)
add r, r, 3
mov i64:0(q), r
add i, i, 1
blt loop, i, 14
ret v
endfunc
endmodule
)");
  for (auto type : {"f", "d", "ld"}) {
    auto move = std::string (type) + "mov", convert = std::string (type) + "2i";
    auto suffix = type == std::string ("f") ? "f" : type == std::string ("ld") ? "L" : "";
    // Integer conversions exercise extended-precision addresses without
    // relying on the host floating-point literal representation.
    const bool extended = type == std::string ("ld");
    auto initial = extended ? "i2ld v, 7\n" : move + " v, 7.0" + suffix + "\n";
    auto store = extended ? "i2ld r, 42\nldmov ld:0(q), r\n"
                          : move + " " + type + ":0(q), 42.0" + suffix + "\n";
    result.push_back ("m: module\nentry: func i64\nlocal " + std::string (type) + ":v, " + type
                      + ":r, i64:p, i64:q, i64:out\n" + initial + "addr p, v\nmov q, p\n" + store
                      + move + " r, " + type + ":0(p)\n" + convert
                      + " out, r\nret out\nendfunc\nendmodule\n");
  }
  result.push_back (R"(m: module
pair: func i64, i64
ret 17, 19
endfunc
sig: proto i64, i64
entry: func i64
local i64:a, i64:b, i64:p, i64:q, i64:r
mov a, 0
mov b, 0
addr p, a
addr q, b
add r, p, q
call sig, pair, a, b
mov r, i64:0(p)
add r, r, b
ret r
endfunc
endmodule
)");
  result.push_back (R"(m: module
write: func i64, i64:p
mov i64:0(p), 42
ret 0
endfunc
sig: proto i64, i64:p
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 7
addr p, v
addr q, v
call sig, write, r, p
mov r, i64:0(q)
ret r
endfunc
endmodule
)");
#if defined(__aarch64__) || defined(_M_ARM64)
  const std::string hardware = "r19";
#else
  const std::string hardware = "r12";
#endif
  result.push_back (R"(m: module
read: func i64, i64:q
local i64:p, i64:r
mov p, i64:0(q)
mov r, i64:0(p)
ret r
endfunc
sig: proto i64, i64:q
entry: func i64
local i64:v, i64:p, i64:q, i64:r
mov v, 42
addr p, v
addr q, p
call sig, read, r, q
ret r
endfunc
endmodule
)");
  result.push_back (R"(m: module
read: func i64, i64:q
local i64:p, i64:r
mov p, i64:0(q)
mov r, i64:0(p)
ret r
endfunc
sig: proto i64, i64:q
entry: func i64
local i64:v, i64:w, i64:p, i64:q, i64:r, i64:c
mov v, 42
mov w, 99
mov c, 1
bt other, c
addr p, v
jmp join
other:
addr p, w
join:
addr q, p
call sig, read, r, q
ret r
endfunc
endmodule
)");
  result.push_back("m: module\nwrite: func i64, i64:p\nmov i64:0(p), 42\nret 0\nendfunc\nsig: proto i64, i64:p\n"
    "entry: func i64\nglobal i64:v:" + hardware + "\nlocal i64:p, i64:r, i64:storage\nmov v, 7\nmov storage, v\naddr p, storage\ncall sig, write, r, p\nmov r, i64:0(p)\nret r\nendfunc\nendmodule\n");
  return result;
}
inline std::vector<std::string> random_address_fixtures () {
  std::uint32_t seed = 0xadc2020u;
  auto next = [&] {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
  };
  std::vector<std::string> result;
  for (unsigned n = 0; n < 128; ++n) {
    std::string text
      = "m: module\nentry: func i64\nlocal i64:v, i64:p, i64:q, i64:r, i64:i\nmov v, "
        + std::to_string (next () % 1000)
        + "\nmov i, 0\naddr p, v\nmov q, p\nloop:\nmov r, i64:0(q)\nadd r, r, "
        + std::to_string (next () % 31 + 1) + "\nmov i64:0(p), r\nadd i, i, 1\nblt loop, i, "
        + std::to_string (next () % 15 + 1) + "\n";
    if (next () & 1)
      text += "ret v\n";
    else
      text += "mov r, i64:0(p)\nret r\n";
    result.push_back (text + "endfunc\nendmodule\n");
  }
  return result;
}
inline std::vector<std::string> address_memory_consumers () {
  std::vector<std::string> result;
  for (auto type : {"blk", "blk1", "blk2"})
    result.push_back("m: module\nread: func i64, " + std::string(type) + ":8(p)\nlocal i64:r\nmov r, i64:0(p)\nret r\nendfunc\n"
      "sig: proto i64, " + type + ":8(p)\nentry: func i64\nlocal i64:v, i64:p, i64:r\nmov v, 42\naddr p, v\n"
      "call sig, read, r, " + type + ":8(p)\nret r\nendfunc\nendmodule\n");
  // Mixed INTEGER/SSE classes contain two eightbytes on SysV. Give them a
  // full 16-byte object; an address of an eight-byte scalar is not enough.
  // Carry the escaped scalar address and a second word through both banks.
  for (auto type : {"blk3", "blk4"})
    result.push_back("m: module\nread: func i64, " + std::string(type) + ":16(p)\nlocal i64:r\n"
      "mov r,i64:0(p)\nmov r,i64:0(r)\nadd r,r,i64:8(p)\nret r\nendfunc\n"
      "sig: proto i64, " + type + ":16(p)\nentry: func i64\nlocal i64:v, i64:p, i64:r, i64:buffer\n"
      "mov v,35\naddr p,v\nalloca buffer,16\nmov i64:0(buffer),p\nmov i64:8(buffer),7\n"
      "call sig,read,r," + type + ":16(buffer)\nret r\nendfunc\nendmodule\n");
  result.push_back (R"(m: module
entry: func i64
local d:v, i64:p, i64:r
dmov v, 42.0
addr p, v
mov r, i64:0(p)
ret r
endfunc
endmodule
)");
  return result;
}
}  // namespace mirnext::test
