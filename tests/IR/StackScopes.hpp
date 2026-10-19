/* Licensed under the MIT License. Shared native and independent execution fixtures. */
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class ScopeTransfer { Branch, Switch, Indirect };
inline std::unique_ptr<ir::Context> stack_scope_example (ScopeTransfer transfer) {
  using namespace ir;
  auto context = std::make_unique<Context> ();
  auto module = context->create_module ("scopes");
  REQUIRE (module);
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (function);
  auto &fn = **function;
  auto outer = fn.create_stack_scope (nullptr, "outer");
  REQUIRE (outer);
  auto inner = fn.create_stack_scope (*outer, "inner");
  REQUIRE (inner);
  auto entry = fn.create_block ("entry");
  REQUIRE (entry);
  auto a = fn.create_block ("outer", *outer);
  REQUIRE (a);
  auto b = fn.create_block ("inner", *inner);
  REQUIRE (b);
  auto done = fn.create_block ("done", *outer);
  REQUIRE (done);
  auto again = fn.create_block ("again", *inner);
  REQUIRE (again);
  auto exit = fn.create_block ("exit");
  REQUIRE (exit);
  auto success = fn.create_block ("success");
  REQUIRE (success);
  auto failure = fn.create_block ("failure");
  REQUIRE (failure);
  auto a_label = fn.create_label (**a, "outer");
  REQUIRE (a_label);
  auto b_label = fn.create_label (**b, "inner");
  REQUIRE (b_label);
  auto done_label = fn.create_label (**done, "done");
  REQUIRE (done_label);
  auto again_label = fn.create_label (**again, "again");
  REQUIRE (again_label);
  auto exit_label = fn.create_label (**exit, "exit");
  REQUIRE (exit_label);
  auto failure_label = fn.create_label (**failure, "failure");
  REQUIRE (failure_label);
  auto counter = fn.create_register (Type::i64 (), "counter");
  REQUIRE (counter);
  IRBuilder builder (**entry);
  REQUIRE (builder.assign (**counter, Operand (std::int64_t{0})));
  auto baseline = builder.stack_save ("baseline");
  REQUIRE (baseline);
  REQUIRE (builder.jump (**a_label));
  REQUIRE (builder.set_insert_point (**a));
  auto slot = builder.allocate_stack (Operand (std::int64_t{32}), "outer-slot");
  REQUIRE (slot);
  REQUIRE (builder.store (Memory{Type::i64 (), *slot}, Operand (std::int64_t{42})));
  auto outer_baseline = builder.stack_save ("outer-baseline");
  REQUIRE (outer_baseline);
  REQUIRE (builder.jump (**b_label));
  REQUIRE (builder.set_insert_point (**b));
  auto scratch = builder.allocate_stack (Operand (std::int64_t{64}), "scratch");
  REQUIRE (scratch);
  auto condition = builder.binary (Opcode::Eq, Operand (**counter), Operand (std::int64_t{0}));
  REQUIRE (condition);
  if (transfer == ScopeTransfer::Branch)
    REQUIRE (builder.branch_if (Operand (**condition), **again_label));
  else if (transfer == ScopeTransfer::Switch) {
    const Label *labels[]{*done_label, *again_label};
    REQUIRE (builder.switch_to (Operand (**condition), labels));
  } else {
    auto choose_done = fn.create_block_after (**b, "choose-done", *inner);
    REQUIRE (choose_done);
    auto choose_again = fn.create_block_after (**choose_done, "choose-again", *inner);
    REQUIRE (choose_again);
    auto dispatch = fn.create_block_after (**choose_again, "dispatch", *inner);
    REQUIRE (dispatch);
    auto choose_label = fn.create_label (**choose_again, "choose-again");
    REQUIRE (choose_label);
    auto dispatch_label = fn.create_label (**dispatch, "dispatch");
    REQUIRE (dispatch_label);
    REQUIRE (builder.branch_if (Operand (**condition), **choose_label));
    for (auto [block, label] :
         {std::pair{*choose_done, *done_label}, std::pair{*choose_again, *again_label}}) {
      REQUIRE (builder.set_insert_point (*block));
      auto address = builder.address_of (*label);
      REQUIRE (address);
      REQUIRE (builder.store (Memory{Type::pointer (), *scratch}, Operand (**address)));
      REQUIRE (builder.jump (**dispatch_label));
    }
    REQUIRE (builder.set_insert_point (**dispatch));
    REQUIRE (builder.emit (Opcode::JmpIndirect, {Operand (Memory{Type::pointer (), *scratch})}));
  }
  REQUIRE (builder.set_insert_point (**again));
  auto incremented = builder.binary (Opcode::Add, Operand (**counter), Operand (std::int64_t{1}));
  REQUIRE (incremented);
  REQUIRE (builder.assign (**counter, Operand (**incremented)));
  REQUIRE (builder.jump (**b_label));
  REQUIRE (builder.set_insert_point (**done));
  auto inner_after = builder.stack_save ("inner-after");
  REQUIRE (inner_after);
  auto inner_balanced
    = builder.binary (Opcode::Eq, Operand (**outer_baseline), Operand (**inner_after));
  REQUIRE (inner_balanced);
  auto result = builder.load (Memory{Type::i64 (), *slot}, "result");
  REQUIRE (result);
  auto checked_result
    = builder.binary (Opcode::Mul, Operand (**result), Operand (**inner_balanced));
  REQUIRE (checked_result);
  REQUIRE (builder.jump (**exit_label));
  REQUIRE (builder.set_insert_point (**exit));
  auto after = builder.stack_save ("after");
  REQUIRE (after);
  auto balanced = builder.binary (Opcode::Eq, Operand (**baseline), Operand (**after));
  REQUIRE (balanced);
  REQUIRE (builder.branch_if (Operand (**balanced), **failure_label, false));
  REQUIRE (builder.set_insert_point (**success));
  REQUIRE (builder.ret (Operand (**checked_result)));
  REQUIRE (builder.set_insert_point (**failure));
  REQUIRE (builder.ret (Operand (std::int64_t{0})));
  REQUIRE (context->verify ());
  return context;
}
}  // namespace mirnext::test
