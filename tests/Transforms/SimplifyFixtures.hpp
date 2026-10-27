/* Licensed under the MIT License. Positive, forbidden and boundary transformations. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <string>
#include <vector>
namespace mirnext::test {
inline std::vector<std::string> simplify_fixtures () {
  std::vector<std::string> result;
  auto function = [&] (std::string body, std::string locals = "i64:x, i64:y, i64:p, i64:q, i64:i") {
    result.push_back ("m:module\nf:func i64\nlocal " + locals + "\n" + body
                      + "\nendfunc\nendmodule\n");
  };
  for (auto op : {"mul", "muls", "mulo", "mulos", "div", "divs", "add", "adds", "sub",  "subs",
                  "or",  "ors",  "xor",  "xors",  "lsh", "lshs", "rsh", "rshs", "ursh", "urshs"}) {
    auto one = std::string (op).starts_with ("mul") || std::string (op).starts_with ("div");
    for (bool same : {false, true})
      for (int constant : {one ? 1 : 0, 2})
        function ("mov y, 42\n" + std::string (op) + " " + (same ? "y" : "x") + ", y, "
                  + std::to_string (constant) + "\nret " + (same ? "y" : "x"));
  }
  for (auto op : {"udiv", "udivs", "umod", "umods", "and", "ands", "umulo", "umulos"})
    function ("mov y, 42\n" + std::string (op) + " x, y, 1\nret x");
  for (auto op : {"bt", "bts", "bf", "bfs"})
    for (int constant : {0, 1, 2})
      function (std::string (op) + " target, " + std::to_string (constant)
                + "\nret 0\ntarget:ret 42");
  for (auto op :
       {"beq",  "beqs",  "bne", "bnes", "blt",  "blts",  "ublt", "ublts", "ble",  "bles",
        "uble", "ubles", "bgt", "bgts", "ubgt", "ubgts", "bge",  "bges",  "ubge", "ubges"}) {
    function (std::string (op) + " target, 1, 2\njmp other\nalias:target:ret 42\nother:ret 0");
    function (std::string (op) + " target, 1, 2\njmp alias\nalias:target:ret 42");
    function (std::string (op) + " target, 1, 2\ntarget:ret 42");
  }
  for (auto op : {"fbeq", "fbne", "fblt", "fble", "fbgt", "fbge", "dbeq", "dbne", "dblt", "dble",
                  "dbgt", "dbge", "ldbeq", "ldbne", "ldblt", "ldble", "ldbgt", "ldbge"}) {
    auto f = std::string (op).starts_with ("f"), ld = std::string (op).starts_with ("ld");
    auto literal = f ? "0.0f" : ld ? "0.0L" : "0.0";
    function (std::string (op) + " target, " + literal + ", " + literal + "\ntarget:ret 42");
    function (std::string (op) + " target, " + literal + ", " + literal
              + "\njmp other\ntarget:ret 42\nother:ret 0");
  }
  for (auto op : {"bo", "bno", "ubo", "ubno"}) {
    function ("addo x, 1, 2\nmov y, x\n" + std::string (op)
              + " target\njmp other\ntarget:ret 42\nother:ret 0");
    function ("addo x, 1, 2\n" + std::string (op) + " target\ntarget:ret 42");
  }
  for (auto op : {"mulo", "mulos"})
    for (auto test : {"bo", "bno"})
      function (std::string (op) + " x, 42, 1\nmov y, x\n" + test + " target\nret y\ntarget:ret 0");
  for (auto op : {"addo", "addos", "subo", "subos", "mulo", "mulos", "umulo", "umulos"})
    function ("mov p, 1024\nmov i, 2\n" + std::string (op) + " i64:8(p,i,4), 42, 1\n"
              + (std::string (op).starts_with ("u") ? "ubo" : "bo")
              + " target\nret 42\ntarget:ret 0");
  for (auto op : {"prbeq", "prbne"})
    function ("prset x, 5\n" + std::string (op)
              + " target, x, 5\njmp other\ntarget:ret 42\nother:ret 0");
  for (auto size : {0, 1, 2, 3, 5, 9, 16, 17}) {
    function ("alloca p, " + std::to_string (size) + "\nalloca q, 9\nalloca i, 3\nret 42");
    function ("alloca p, " + std::to_string (size) + "\nalloca p, 9\nret 42");
    function ("alloca p, " + std::to_string (size) + "\nanchor:alloca q, 9\nret 42");
  }
  function (
    "mov p, 1024\nmov i, 2\nmov i64:8(p,i,4), i64:16(p,i,2)\nadd i64:8(p,i,4), i64:16(p,i,2), "
    "2\nret i64:8(p,i,4)");
  function (
    "mov p, 1024\nmov i, 2\ndmov d:8(p,i,4), d:16(p,i,2)\ndadd d:8(p,i,4), d:16(p,i,2), 2.0\nret "
    "42");
  function ("mov x, \"a\\000b\"\nmov y, \"a\\000b\"\nret 42");
  function ("laddr p, anchor\njmpi p\nret 0\nanchor:ret 42");
  function ("jmp first\nfirst:jmp second\nsecond:jmp first\nret 42");
  std::string chain = "jmp l0\n";
  for (unsigned n = 0; n < 40; ++n)
    chain += "l" + std::to_string (n) + ":jmp l" + std::to_string (n + 1) + "\n";
  function (chain + "l40:ret 42");
  result.push_back (
    "m:module\nvalue:lref anchor, base, 7\nf:func i64\nlocal i64:x\nbt anchor, x\nbase:anchor:ret "
    "42\nendfunc\nendmodule\n");
  result.push_back (
    "m:module\nf:func i8, i8:a, u8:b, i16:c, u16:d, i32:e, u32:f\nbt target, a\nret b\ntarget:ret "
    "c\nendfunc\nendmodule\n");
  result.push_back (
    "m:module\nf:func i64, d\nlocal i64:x, d:y\nbt target, 2\nret 0, 0.0\ntarget:ret 42, "
    "42.0\nendfunc\nendmodule\n");
  result.push_back (
    "m:module\nvalue:expr f\nf:func d\nlocal d:x\ndadd x, 20.5, 21.5\nret x\nendfunc\nendmodule\n");
  return result;
}
}  // namespace mirnext::test
