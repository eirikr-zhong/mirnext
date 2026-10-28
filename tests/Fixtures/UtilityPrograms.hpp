/* Licensed under the MIT License. Owning replacements for old MIR utility fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include "mirnext/Serialization/Format.hpp"
#include <array>
namespace mirnext::test {
inline constexpr std::string_view utility_memory_text = R"mir(memory:module
memop:func i64,i64:arg1,i64:arg2
add i64:(arg1,arg2,8),i64:64(arg1),i64:(,arg1,8)
ret i64:(arg1)
ret i64:(,arg2,1)
ret i64:1024
mov i64:(arg1,arg2,8),i64:(arg1)
ret 0
endfunc
endmodule
)mir";
inline std::string utility_unterminated_memory_text () {
  std::string text (utility_memory_text);
  text.erase (text.find ("ret 0\nendfunc"), 6);
  return text;
}
// Signature and inline intent of issue 219; sessions load immutable snapshots
// rather than extending an old mutable runtime after linking.
inline constexpr std::string_view utility_add_text = R"mir(m_add:module
export add
add:func i32,i32:a0,i32:a1
local i64:r0
add r0,a0,a1
ret r0
endfunc
endmodule
)mir";
inline Result<ir::Function &> append_api_sieve (ir::Program &context, std::string_view name,
                                                std::size_t flags = 819000, unsigned passes = 100) {
  using namespace ir;
  if (flags < 4 || flags > std::size_t (INT64_MAX) || !passes)
    return Error{ErrorCode::InvalidArgument, "invalid API sieve workload"};
  auto module = mirnext::ir::detail::access (context).create_module (name);
  if (!(module).has_value ()) return module.error ();
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("sieve", {{Type::i64 ()}, {}});
  if (!(function).has_value ()) return function.error ();
  std::array<BasicBlock *, 15> blocks{};
  std::array<Label *, 15> labels{};
  for (unsigned n = 0; n < blocks.size (); ++n) {
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    if (!(block).has_value ()) return block.error ();
    blocks[n] = &(block).value ();
    auto label = mirnext::ir::detail::access (((function)).value ())
                   .create_label ((block).value (), "L" + std::to_string (n));
    if (!(label).has_value ()) return label.error ();
    labels[n] = &(label).value ();
  }
  std::array<Register *, 5> regs{};
  constexpr std::array names{"iter", "count", "i", "k", "prime"};
  for (unsigned n = 0; n < regs.size (); ++n) {
    auto r = mirnext::ir::detail::access (((function)).value ())
               .create_register (Type::i64 (), names[n]);
    if (!(r).has_value ()) return r.error ();
    regs[n] = &(r).value ();
  }
  auto [iter, count, i, k, prime] = regs;
  InstructionBuilder b (*blocks[0]);
  auto memory = b.allocate_stack (Operand (std::int64_t (flags)), "flags");
  if (!(memory).has_value ()) return memory.error ();
  std::optional<Error> error;
  auto check = [&] (auto result) {
    if (!result.has_value () && !error) error = result.error ();
  };
  auto point = [&] (unsigned n) { check (b.set_insert_point (*blocks[n])); };
  auto value = [&] (auto r) { return b.expression (*r); };
  auto increment = [&] (auto r) { check (b.assign (*r, value (r) + b.literal (1))); };
  check (b.assign (*iter, 0));
  point (1);
  check (b.branch (Comparison::GreaterEqual, value (iter), b.literal (std::int64_t (passes)),
                   *labels[14]));
  point (2);
  check (b.assign (*count, 0));
  check (b.assign (*i, 0));
  point (3);
  check (
    b.branch (Comparison::GreaterEqual, value (i), b.literal (std::int64_t (flags)), *labels[5]));
  point (4);
  check (b.store (mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::U8),
                                                                &(memory).value (), i),
                  1));
  increment (i);
  check (b.jump (*labels[3]));
  point (5);
  check (b.assign (*i, 1));
  point (6);
  check (
    b.branch (Comparison::GreaterEqual, value (i), b.literal (std::int64_t (flags)), *labels[13]));
  point (7);
  check (
    b.branch (Comparison::Equal,
              b.expression (mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::U8),
                                                                          &(memory).value (), i)),
              b.literal (Type (Type::Kind::U8), 0), *labels[12]));
  point (8);
  check (b.assign (*prime, value (i) + b.literal (1)));
  check (b.assign (*k, value (i) + value (prime)));
  point (9);
  check (
    b.branch (Comparison::GreaterEqual, value (k), b.literal (std::int64_t (flags)), *labels[11]));
  point (10);
  check (b.store (mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::U8),
                                                                &(memory).value (), k),
                  0));
  check (b.assign (*k, value (k) + value (prime)));
  check (b.jump (*labels[9]));
  point (11);
  increment (count);
  point (12);
  increment (i);
  check (b.jump (*labels[6]));
  point (13);
  increment (iter);
  check (b.jump (*labels[1]));
  point (14);
  check (b.ret (Operand (*count)));
  if (error) return *error;
  return (function).value ();
}
inline Result<ir::Program> utility_hello () {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("hello");
  if (!(module).has_value ()) return module.error ();
  InitializedData data{Type (Type::Kind::U8), {}};
  constexpr char greetings[] = "world\0all\0\0";
  for (auto byte : greetings) data.values.emplace_back (static_cast<unsigned char> (byte));
  auto strings
    = mirnext::ir::detail::access (((module)).value ()).create_data ("greetings", std::move (data));
  if (!(strings).has_value ()) return strings.error ();
  auto callback = mirnext::ir::detail::access (((module)).value ())
                    .prototype ("cb", {{Type (Type::Kind::I32)}, {Type::pointer ()}});
  if (!(callback).has_value ()) return callback.error ();
  auto function
    = mirnext::ir::detail::access (((module)).value ())
        .create_function ("hello", {{Type (Type::Kind::I32)},
                                    {Type::pointer (), Type::pointer (), Type (Type::Kind::I32)}});
  if (!(function).has_value ()) return function.error ();
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  if (!(block).has_value ()) return block.error ();
  InstructionBuilder b ((block).value ());
  auto args = mirnext::ir::detail::ReferenceAccess::pointers (function.value ().arguments ());
  auto base = b.address_of (strings.value ().symbol ());
  if (!(base).has_value ()) return base.error ();
  auto offset = b.binary (Opcode::Mul, Operand (*args[2]), 6);
  if (!(offset).has_value ()) return offset.error ();
  auto selected = b.binary (Opcode::Add, Operand ((base).value ()), Operand ((offset).value ()));
  if (!(selected).has_value ()) return selected.error ();
  auto first = b.call_indirect ((callback).value (), Operand (*args[1]), {Operand (*args[0])});
  if (!(first).has_value ()) return first.error ();
  auto second
    = b.call_indirect ((callback).value (), Operand (*args[1]), {Operand ((selected).value ())});
  if (!(second).has_value ()) return second.error ();
  auto left = first.value ().value (), right = second.value ().value ();
  if (!(left).has_value ()) return left.error ();
  if (!(right).has_value ()) return right.error ();
  auto returned = b.ret (b.expression ((left).value ()) + b.expression ((right).value ()));
  if (!(returned).has_value ()) return returned.error ();
  auto valid = context->verify ();
  if (!(valid).has_value ()) return valid.error ();
  return std::move (*context);
}
}  // namespace mirnext::test
