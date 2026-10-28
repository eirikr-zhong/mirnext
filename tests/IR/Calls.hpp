/* Licensed under the MIT License. Shared DSL and independent execution fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class CallExample {
  Direct,
  Indirect,
  Block,
  ReturnBlock,
  Variadic,
  VariadicBlock,
  MultipleResults,
  Inline
};
inline std::unique_ptr<ir::Program> call_example (CallExample mode,
                                                  bool indirect_boundary = false) {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("calls");
  REQUIRE ((module).has_value ());
  Signature signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}};
  if (mode == CallExample::Block) signature.parameters = {Type::block (16)};
  if (mode == CallExample::ReturnBlock) signature = {{}, {Type::return_block (16)}};
  if (mode == CallExample::Variadic || mode == CallExample::VariadicBlock)
    signature = {{Type::i64 ()}, {Type::i64 ()}, true};
  if (mode == CallExample::MultipleResults) signature.results = {Type::i64 (), Type::i64 ()};
  auto callee
    = mirnext::ir::detail::access (((module)).value ()).create_function ("sum", signature);
  REQUIRE ((callee).has_value ());
  auto body = mirnext::ir::detail::access (((callee)).value ()).create_block ();
  REQUIRE ((body).has_value ());
  InstructionBuilder sum ((body).value ());
  auto args = mirnext::ir::detail::ReferenceAccess::pointers (callee.value ().arguments ());
  if (mode == CallExample::ReturnBlock) {
    REQUIRE ((sum.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), args[0]),
                         Operand (std::int64_t{42})))
               .has_value ());
    REQUIRE ((sum.ret ()).has_value ());
  } else if (mode == CallExample::MultipleResults) {
    const Operand results[]{Operand (*args[0]), Operand (*args[1])};
    REQUIRE ((sum.ret (results)).has_value ());
  } else {
    const Register *left = nullptr, *right = nullptr;
    if (mode == CallExample::Block) {
      auto a = sum.load (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), args[0]));
      REQUIRE ((a).has_value ());
      left = &(a).value ();
      auto b = sum.load (
        mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), args[0], nullptr, 8));
      REQUIRE ((b).has_value ());
      right = &(b).value ();
    } else if (signature.variadic) {
      auto list = sum.allocate_stack (Operand (std::int64_t{64}), "list");
      REQUIRE ((list).has_value ());
      REQUIRE ((sum.start_varargs (Operand ((list).value ()))).has_value ());
      if (mode == CallExample::Variadic) {
        auto a = sum.next_vararg (Type::i64 (), Operand ((list).value ()));
        REQUIRE ((a).has_value ());
        left = &(a).value ();
        auto b = sum.next_vararg (Type::i64 (), Operand ((list).value ()));
        REQUIRE ((b).has_value ());
        right = &(b).value ();
      } else {
        auto pair = sum.allocate_stack (Operand (std::int64_t{16}));
        REQUIRE ((pair).has_value ());
        REQUIRE (
          (sum.next_vararg_block ((pair).value (), Operand ((list).value ()), Type::block (16)))
            .has_value ());
        auto a = sum.load (
          mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pair).value ()));
        REQUIRE ((a).has_value ());
        left = &(a).value ();
        auto b
          = sum.load (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pair).value (),
                                                                    nullptr, 8));
        REQUIRE ((b).has_value ());
        right = &(b).value ();
      }
      REQUIRE ((sum.end_varargs (Operand ((list).value ()))).has_value ());
    } else {
      left = args[0];
      right = args[1];
    }
    auto result = sum.binary (Opcode::Add, Operand (*left), Operand (*right));
    REQUIRE ((result).has_value ());
    REQUIRE ((sum.ret (Operand ((result).value ()))).has_value ());
  }
  auto entry = mirnext::ir::detail::access (((module)).value ())
                 .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((entry).has_value ());
  auto block = mirnext::ir::detail::access (((entry)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder caller ((block).value ());
  auto scope = caller.begin_stack_scope ();
  REQUIRE ((scope).has_value ());
  const Register *target = nullptr;
  if (indirect_boundary) {
    auto slot
      = mirnext::ir::detail::access (((module)).value ())
          .create_variable ("target", Type::pointer (), Operand (callee.value ().symbol ()));
    REQUIRE ((slot).has_value ());
    auto loaded = caller.load ((slot).value ());
    REQUIRE ((loaded).has_value ());
    target = &(loaded).value ();
  }
  auto invoke = [&] (std::initializer_list<mirnext::ir::detail::CallArgumentState> arguments) {
    return indirect_boundary
             ? caller.call_indirect (callee.value ().symbol (), Operand (*target), arguments)
             : caller.call ((callee).value (), arguments);
  };
  const Register *answer = nullptr;
  if (mode == CallExample::Block || mode == CallExample::ReturnBlock
      || mode == CallExample::VariadicBlock) {
    auto pair = caller.allocate_stack (Operand (std::int64_t{24}));
    REQUIRE ((pair).has_value ());
    REQUIRE (
      (caller.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pair).value (),
                                                                   nullptr, 8),
                     Operand (std::int64_t{20})))
        .has_value ());
    REQUIRE (
      (caller.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pair).value (),
                                                                   nullptr, 16),
                     Operand (std::int64_t{22})))
        .has_value ());
    if (mode == CallExample::ReturnBlock) {
      auto call
        = invoke ({mirnext::ir::detail::BlockArgumentState::return_buffer ((pair).value (), 16)});
      REQUIRE ((call).has_value ());
      CHECK (call.value ().results ().empty ());
      auto loaded = caller.load (
        mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(pair).value ()));
      REQUIRE ((loaded).has_value ());
      answer = &(loaded).value ();
    } else {
      auto argument = mirnext::ir::detail::BlockArgumentState ((pair).value (), 16,
                                                               Type::BlockClass::Default, 8);
      auto call
        = mode == CallExample::Block ? invoke ({argument}) : invoke ({std::int64_t{0}, argument});
      REQUIRE ((call).has_value ());
      auto result = call.value ().value ();
      REQUIRE ((result).has_value ());
      answer = &(result).value ();
    }
  } else if (mode == CallExample::Indirect && !indirect_boundary) {
    auto address = caller.address_of (callee.value ().symbol ());
    REQUIRE ((address).has_value ());
    auto call = caller.call_indirect (callee.value ().symbol (), Operand ((address).value ()),
                                      {std::int64_t{20}, std::int64_t{22}});
    REQUIRE ((call).has_value ());
    CHECK (call.value ().instruction ().opcode () == Opcode::Call);
    auto result = call.value ().value ();
    REQUIRE ((result).has_value ());
    answer = &(result).value ();
  } else {
    auto call = mode == CallExample::Variadic
                  ? invoke ({std::int64_t{0}, std::int64_t{20}, std::int64_t{22}})
                : mode == CallExample::Inline && !indirect_boundary
                  ? caller.inline_call ((callee).value (), {std::int64_t{20}, std::int64_t{22}})
                  : invoke ({std::int64_t{20}, std::int64_t{22}});
    REQUIRE ((call).has_value ());
    if (mode == CallExample::MultipleResults) {
      CHECK_FALSE ((call.value ().value ()).has_value ());
      REQUIRE (call.value ().results ().size () == 2);
      auto result = caller.binary (Opcode::Add,
                                   Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                                     call.value ().results ())[0]),
                                   Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                                     call.value ().results ())[1]));
      REQUIRE ((result).has_value ());
      answer = &(result).value ();
    } else {
      auto result = call.value ().value ();
      REQUIRE ((result).has_value ());
      answer = &(result).value ();
    }
  }
  REQUIRE ((caller.ret (Operand (*answer))).has_value ());
  REQUIRE ((caller.end_stack_scope ((scope).value ())).has_value ());
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
}  // namespace mirnext::test
