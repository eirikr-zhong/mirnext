/* Licensed under the MIT License. Shared DSL and independent execution fixtures. */
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
inline std::unique_ptr<ir::Context> call_example (CallExample mode,
                                                  bool indirect_boundary = false) {
  using namespace ir;
  auto context = std::make_unique<Context> ();
  auto module = context->create_module ("calls");
  REQUIRE (module);
  Signature signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}};
  if (mode == CallExample::Block) signature.parameters = {Type::block (16)};
  if (mode == CallExample::ReturnBlock) signature = {{}, {Type::return_block (16)}};
  if (mode == CallExample::Variadic || mode == CallExample::VariadicBlock)
    signature = {{Type::i64 ()}, {Type::i64 ()}, true};
  if (mode == CallExample::MultipleResults) signature.results = {Type::i64 (), Type::i64 ()};
  auto callee = (*module)->create_function ("sum", signature);
  REQUIRE (callee);
  auto body = (*callee)->create_block ();
  REQUIRE (body);
  IRBuilder sum (**body);
  auto args = (*callee)->arguments ();
  if (mode == CallExample::ReturnBlock) {
    REQUIRE (sum.store (Memory{Type::i64 (), args[0]}, Operand (std::int64_t{42})));
    REQUIRE (sum.ret ());
  } else if (mode == CallExample::MultipleResults) {
    const Operand results[]{Operand (*args[0]), Operand (*args[1])};
    REQUIRE (sum.ret (results));
  } else {
    const Register *left = nullptr, *right = nullptr;
    if (mode == CallExample::Block) {
      auto a = sum.load (Memory{Type::i64 (), args[0]});
      REQUIRE (a);
      left = *a;
      auto b = sum.load (Memory{Type::i64 (), args[0], nullptr, 8});
      REQUIRE (b);
      right = *b;
    } else if (signature.variadic) {
      auto list = sum.allocate_stack (Operand (std::int64_t{64}), "list");
      REQUIRE (list);
      REQUIRE (sum.start_varargs (Operand (**list)));
      if (mode == CallExample::Variadic) {
        auto a = sum.next_vararg (Type::i64 (), Operand (**list));
        REQUIRE (a);
        left = *a;
        auto b = sum.next_vararg (Type::i64 (), Operand (**list));
        REQUIRE (b);
        right = *b;
      } else {
        auto pair = sum.allocate_stack (Operand (std::int64_t{16}));
        REQUIRE (pair);
        REQUIRE (sum.next_vararg_block (**pair, Operand (**list), Type::block (16)));
        auto a = sum.load (Memory{Type::i64 (), *pair});
        REQUIRE (a);
        left = *a;
        auto b = sum.load (Memory{Type::i64 (), *pair, nullptr, 8});
        REQUIRE (b);
        right = *b;
      }
      REQUIRE (sum.end_varargs (Operand (**list)));
    } else {
      left = args[0];
      right = args[1];
    }
    auto result = sum.binary (Opcode::Add, Operand (*left), Operand (*right));
    REQUIRE (result);
    REQUIRE (sum.ret (Operand (**result)));
  }
  auto entry = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (entry);
  auto block = (*entry)->create_block ();
  REQUIRE (block);
  IRBuilder caller (**block);
  auto scope = caller.begin_stack_scope ();
  REQUIRE (scope);
  const Register *target = nullptr;
  if (indirect_boundary) {
    auto slot
      = (*module)->create_variable ("target", Type::pointer (), Operand ((*callee)->symbol ()));
    REQUIRE (slot);
    auto loaded = caller.load (**slot);
    REQUIRE (loaded);
    target = *loaded;
  }
  auto invoke = [&] (std::initializer_list<CallArgument> arguments) {
    return indirect_boundary
             ? caller.call_indirect ((*callee)->symbol (), Operand (*target), arguments)
             : caller.call (**callee, arguments);
  };
  const Register *answer = nullptr;
  if (mode == CallExample::Block || mode == CallExample::ReturnBlock
      || mode == CallExample::VariadicBlock) {
    auto pair = caller.allocate_stack (Operand (std::int64_t{24}));
    REQUIRE (pair);
    REQUIRE (caller.store (Memory{Type::i64 (), *pair, nullptr, 8}, Operand (std::int64_t{20})));
    REQUIRE (caller.store (Memory{Type::i64 (), *pair, nullptr, 16}, Operand (std::int64_t{22})));
    if (mode == CallExample::ReturnBlock) {
      auto call = invoke ({BlockArgument::return_buffer (**pair, 16)});
      REQUIRE (call);
      CHECK (call->results ().empty ());
      auto loaded = caller.load (Memory{Type::i64 (), *pair});
      REQUIRE (loaded);
      answer = *loaded;
    } else {
      auto argument = BlockArgument (**pair, 16, Type::BlockClass::Default, 8);
      auto call
        = mode == CallExample::Block ? invoke ({argument}) : invoke ({std::int64_t{0}, argument});
      REQUIRE (call);
      auto result = call->value ();
      REQUIRE (result);
      answer = *result;
    }
  } else if (mode == CallExample::Indirect && !indirect_boundary) {
    auto address = caller.address_of ((*callee)->symbol ());
    REQUIRE (address);
    auto call = caller.call_indirect ((*callee)->symbol (), Operand (**address),
                                      {std::int64_t{20}, std::int64_t{22}});
    REQUIRE (call);
    CHECK (call->instruction ().opcode () == Opcode::Call);
    auto result = call->value ();
    REQUIRE (result);
    answer = *result;
  } else {
    auto call = mode == CallExample::Variadic
                  ? invoke ({std::int64_t{0}, std::int64_t{20}, std::int64_t{22}})
                : mode == CallExample::Inline && !indirect_boundary
                  ? caller.inline_call (**callee, {std::int64_t{20}, std::int64_t{22}})
                  : invoke ({std::int64_t{20}, std::int64_t{22}});
    REQUIRE (call);
    if (mode == CallExample::MultipleResults) {
      CHECK_FALSE (call->value ());
      REQUIRE (call->results ().size () == 2);
      auto result = caller.binary (Opcode::Add, Operand (*call->results ()[0]),
                                   Operand (*call->results ()[1]));
      REQUIRE (result);
      answer = *result;
    } else {
      auto result = call->value ();
      REQUIRE (result);
      answer = *result;
    }
  }
  REQUIRE (caller.ret (Operand (*answer)));
  REQUIRE (caller.end_stack_scope (**scope));
  REQUIRE (context->verify ());
  return context;
}
}  // namespace mirnext::test
