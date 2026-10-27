/* Licensed under the MIT License. Shared function analysis fixtures. */
#pragma once
#include <string>
#include <vector>
#include <random>
#include <sstream>
namespace mirnext::test {
inline std::vector<std::string> analysis_fixtures () {
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto hardware = "rdx";
#else
  constexpr auto hardware = "r5";
#endif
  return {
    R"(m: module
entry: func i64, i64:condition
local i64:value, i64:answer
mov value, 10
bt yes, condition
mov value, 20
jmp join
yes:
add value, value, 1
join:
mov answer, value
ret answer
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:limit
local i64:i, i64:j, i64:sum
mov i, 0
mov sum, 0
outer:
mov j, 0
inner:
add sum, sum, i
add j, j, 1
blt inner, j, limit
add i, i, 1
blt outer, i, limit
ret sum
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:selector
local i64:value
switch selector, yes, no, yes
yes:
mov value, 42
jmp join
no:
mov value, 0
join:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:condition
local i64:value
bt right, condition
left:
add value, value, 1
bt right, condition
jmp done
right:
sub value, value, 1
bt left, condition
done:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64
local i64:address, i64:value
laddr address, target
jmpi address
dead:
mov value, 3
jmp done
target:
mov value, 42
done:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64
local i64:value
mov value, 0
loop:
add value, value, 1
jmp loop
dead:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64
local i64:value
mov value, 42
jmp done
dead1:
add value, value, 1
jmp dead2
dead2:
add value, value, 1
jmp dead1
done:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:address, i64:index
local i64:value
mov value, i64:8(address, index, 4)
mov i64:16(address, index, 8), value
ret value
endfunc
endmodule
)",
    std::string (R"(m: module
entry: func i64
local i64:value
global i64:bound:)")
      + hardware + R"(
mov value, 42
mov bound, 13
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:condition1, i64:condition2
local i64:value
bt left, condition1
mov value, 10
jmp second
left:
mov value, 20
bt first, condition2
mov value, 30
jmp first
first:
second:
ret value
endfunc
endmodule
)",
    R"(m: module
entry: func i64, i64:condition
local i64:value
mov value, 42
bt third, condition
jmp first
first:
second:
third:
ret value
endfunc
difference: lref first, second, 7
endmodule
)",
    R"(m: module
import target
pj: proto
entry: func i64
jcall pj, target
continuation:
ret 42
endfunc
endmodule
)",
    R"(m: module
import target
pj: proto
entry: func
jcall pj, target
endfunc
endmodule
)"};
}
inline std::vector<std::string> random_analysis_fixtures () {
  std::mt19937 random (0xc6f5aa);
  std::vector<std::string> inputs;
  for (unsigned n = 0; n < 128; ++n) {
    unsigned count = 3 + random () % 14;
    std::ostringstream out;
    out << "m: module\nentry: func i64, i64:condition\nlocal i64:value\nmov value, 0\n";
    for (unsigned b = 0; b < count; ++b) {
      out << "b" << b << ":\nadd value, value, " << random () % 17 << '\n';
      if (b + 1 == count)
        out << "ret value\n";
      else if (random () % 3 == 0)
        out << "switch condition, b" << random () % count << ", b" << random () % count << ", b"
            << random () % count << '\n';
      else
        out << (random () % 2 ? "bt" : "bf") << " b" << random () % count << ", condition\n";
    }
    out << "endfunc\nendmodule\n";
    inputs.push_back (out.str ());
  }
  return inputs;
}
}  // namespace mirnext::test
