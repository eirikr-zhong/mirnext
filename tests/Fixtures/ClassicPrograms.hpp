/* Licensed under the MIT License. Native fixtures from the original MIR drivers. */
#pragma once
#include "mirnext/Serialization/Format.hpp"
namespace mirnext::test {
// Original loop builder expressed with owning C++ blocks, labels and DSL.
inline Result<std::unique_ptr<ir::Context>> classic_loop () {
  using namespace ir;
  auto context = std::make_unique<Context> ();
  auto module = context->create_module ("m");
  if (!module) return module.error ();
  auto function = (*module)->create_function ("loop", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return function.error ();
  auto entry = (*function)->create_block ("entry"), loop = (*function)->create_block ("loop"),
       done = (*function)->create_block ("done");
  if (!entry) return entry.error ();
  if (!loop) return loop.error ();
  if (!done) return done.error ();
  auto again = (*function)->create_label (**loop, "again"),
       finish = (*function)->create_label (**done, "finish");
  if (!again) return again.error ();
  if (!finish) return finish.error ();
  auto count = (*function)->create_register (Type::i64 (), "count");
  if (!count) return count.error ();
  auto limit = (*function)->arguments ()[0];
  IRBuilder body (**entry);
  auto assigned = body.assign (**count, 0);
  if (!assigned) return assigned.error ();
  auto branch = body.branch (Comparison::GreaterEqual, body.expression (**count),
                             body.expression (*limit), **finish);
  if (!branch) return branch.error ();
  auto point = body.set_insert_point (**loop);
  if (!point) return point.error ();
  assigned = body.assign (**count, body.expression (**count) + body.literal (1));
  if (!assigned) return assigned.error ();
  branch
    = body.branch (Comparison::Less, body.expression (**count), body.expression (*limit), **again);
  if (!branch) return branch.error ();
  point = body.set_insert_point (**done);
  if (!point) return point.error ();
  auto returned = body.ret (Operand (**count));
  if (!returned) return returned.error ();
  auto valid = context->verify ();
  if (!valid) return valid.error ();
  return context;
}
inline constexpr std::string_view classic_hi_text = R"mir(
m_hi:    module
proto:	 proto i32, i32:ch
	 import print
         export hi
hi:      func i32
         local i64:h, i64:i, i64:exc, i64:nl, i64:r, i64:temp
         mov h, 104
         call proto, print, r, h
         mov i, 105
         call proto, print, temp, i
         add r, r, temp
         mov exc, 33
         call proto, print, temp, exc
         add r, r, temp
         mov nl, 10
         call proto, print, temp, nl
         add r, r, temp
         ret r
         endfunc
         endmodule
)mir";
inline constexpr std::string_view classic_args_text = R"mir(
m:     module
p_pri: proto i64:v
p_prf: proto f:v
p_prd: proto d:v
       import pri, prf, prd
       export f
f:     func i64, i8:i1, i16:i2, i32:i3, i64:i4, f:f1, d:d1, u32:i5, u8:i6, u16:i7, i32:i8, i64:i9, f:f2, f:f3, f:f4, f:f5, f:f6, f:f7, f:f8, d:d2
       call p_pri, pri, i1
       call p_pri, pri, i2
       call p_pri, pri, i3
       call p_pri, pri, i4
       call p_pri, pri, i5
       call p_pri, pri, i6
       call p_pri, pri, i7
       call p_pri, pri, i8
       call p_pri, pri, i9
       call p_prf, prf, f1
       call p_prf, prf, f2
       call p_prf, prf, f3
       call p_prf, prf, f4
       call p_prf, prf, f5
       call p_prf, prf, f6
       call p_prf, prf, f7
       call p_prf, prf, f8
       call p_prd, prd, d1
       call p_prd, prd, d2
       ret 0
       endfunc
       endmodule
)mir";
inline constexpr std::string_view classic_sieve_template = R"mir(
m_sieve: module
sieve:   func i64
         local i64:iter, i64:count, i64:i, i64:k, i64:prime, i64:flags
         alloca flags, 8190
         mov iter, 0
loop:    bge fin, iter, 2
         mov count, 0;  mov i, 0
loop2:   mov u8:(flags, i)::n1, 1;  add i, i, 1
         blt loop2, i, 8190
         mov i, 2
loop3:   beq cont3, u8:(flags,i):c, 0
         add prime, i, 1; add k, i, prime
loop4:   bge fin4, k, 8190
         mov u8:(flags, k):c:n2, 0;  add k, k, prime
         jmp loop4
fin4:    add count, count, 1
cont3:   add i, i, 1
         blt loop3, i, 8190
         add iter, iter, 1
         jmp loop
fin:     ret count
         endfunc
         endmodule
)mir";
inline constexpr std::string_view classic_readme_text = R"mir(
m_sieve:  module
          export sieve
sieve:    func i32, i32:N
          local i64:iter, i64:count, i64:i, i64:k, i64:prime, i64:temp, i64:flags
          alloca flags, 8190
          mov iter, 0
loop:     bge fin, iter, N
          mov count, 0;  mov i, 0
loop2:    bge fin2, i, 8190
          mov u8:(flags, i), 1;  add i, i, 1
          jmp loop2
fin2:     mov i, 0
loop3:    bge fin3, i, 8190
          beq cont3, u8:(flags,i), 0
          add temp, i, i;  add prime, temp, 3;  add k, i, prime
loop4:    bge fin4, k, 8190
          mov u8:(flags, k), 0;  add k, k, prime
          jmp loop4
fin4:     add count, count, 1
cont3:    add i, i, 1
          jmp loop3
fin3:     add iter, iter, 1
          jmp loop
fin:      ret count
          endfunc
          endmodule
m_ex100:  module
format:   string "sieve of 8190 200 times = %d\n"
p_printf: proto p:fmt, i32:r
p_sieve:  proto i32, i32:N
          export ex100
          import sieve, printf
ex100:    func
          local i64:r
          call p_sieve, sieve, r, 200
          call p_printf, printf, format, r
          ret
          endfunc
          endmodule
)mir";
// Parameter changes affect workload sizes only; original loop/memory/alias edges remain.
inline std::string classic_sieve_text (std::size_t flags = 8190, unsigned iterations = 2,
                                       bool odd = false) {
  std::string text (odd ? classic_readme_text : classic_sieve_template);
  for (std::size_t at = 0; (at = text.find ("8190", at)) != std::string::npos;) {
    auto replacement = std::to_string (flags);
    text.replace (at, 4, replacement);
    at += replacement.size ();
  }
  if (odd) {
    auto at = text.find ("r, 200");
    if (at != std::string::npos) text.replace (at, 6, "r, " + std::to_string (iterations));
  } else {
    auto at = text.find ("iter, 2");
    if (at != std::string::npos) text.replace (at, 7, "iter, " + std::to_string (iterations));
  }
  return text;
}
}  // namespace mirnext::test
