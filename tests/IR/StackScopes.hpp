/* Licensed under the MIT License. Shared native and independent execution fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class ScopeTransfer { Branch, Switch, Indirect };
inline std::unique_ptr<ir::Program> stack_scope_example (ScopeTransfer transfer) {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("scopes");
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto &fn = (function).value ();
  auto outer = mirnext::ir::detail::access (fn).create_stack_scope (nullptr, "outer");
  REQUIRE ((outer).has_value ());
  auto inner = mirnext::ir::detail::access (fn).create_stack_scope (&(outer).value (), "inner");
  REQUIRE ((inner).has_value ());
  auto entry = mirnext::ir::detail::access (fn).create_block ("entry");
  REQUIRE ((entry).has_value ());
  auto a = mirnext::ir::detail::access (fn).create_block ("outer", &(outer).value ());
  REQUIRE ((a).has_value ());
  auto b = mirnext::ir::detail::access (fn).create_block ("inner", &(inner).value ());
  REQUIRE ((b).has_value ());
  auto done = mirnext::ir::detail::access (fn).create_block ("done", &(outer).value ());
  REQUIRE ((done).has_value ());
  auto again = mirnext::ir::detail::access (fn).create_block ("again", &(inner).value ());
  REQUIRE ((again).has_value ());
  auto exit = mirnext::ir::detail::access (fn).create_block ("exit");
  REQUIRE ((exit).has_value ());
  auto success = mirnext::ir::detail::access (fn).create_block ("success");
  REQUIRE ((success).has_value ());
  auto failure = mirnext::ir::detail::access (fn).create_block ("failure");
  REQUIRE ((failure).has_value ());
  auto a_label = mirnext::ir::detail::access (fn).create_label ((a).value (), "outer");
  REQUIRE ((a_label).has_value ());
  auto b_label = mirnext::ir::detail::access (fn).create_label ((b).value (), "inner");
  REQUIRE ((b_label).has_value ());
  auto done_label = mirnext::ir::detail::access (fn).create_label ((done).value (), "done");
  REQUIRE ((done_label).has_value ());
  auto again_label = mirnext::ir::detail::access (fn).create_label ((again).value (), "again");
  REQUIRE ((again_label).has_value ());
  auto exit_label = mirnext::ir::detail::access (fn).create_label ((exit).value (), "exit");
  REQUIRE ((exit_label).has_value ());
  auto failure_label
    = mirnext::ir::detail::access (fn).create_label ((failure).value (), "failure");
  REQUIRE ((failure_label).has_value ());
  auto counter = mirnext::ir::detail::access (fn).create_register (Type::i64 (), "counter");
  REQUIRE ((counter).has_value ());
  InstructionBuilder builder ((entry).value ());
  REQUIRE ((builder.assign ((counter).value (), Operand (std::int64_t{0}))).has_value ());
  auto baseline = builder.stack_save ("baseline");
  REQUIRE ((baseline).has_value ());
  REQUIRE ((builder.jump ((a_label).value ())).has_value ());
  REQUIRE ((builder.set_insert_point ((a).value ())).has_value ());
  auto slot = builder.allocate_stack (Operand (std::int64_t{32}), "outer-slot");
  REQUIRE ((slot).has_value ());
  REQUIRE (
    (builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(slot).value ()),
                    Operand (std::int64_t{42})))
      .has_value ());
  auto outer_baseline = builder.stack_save ("outer-baseline");
  REQUIRE ((outer_baseline).has_value ());
  REQUIRE ((builder.jump ((b_label).value ())).has_value ());
  REQUIRE ((builder.set_insert_point ((b).value ())).has_value ());
  auto scratch = builder.allocate_stack (Operand (std::int64_t{64}), "scratch");
  REQUIRE ((scratch).has_value ());
  auto condition
    = builder.binary (Opcode::Eq, Operand ((counter).value ()), Operand (std::int64_t{0}));
  REQUIRE ((condition).has_value ());
  if (transfer == ScopeTransfer::Branch)
    REQUIRE (
      (builder.branch_if (Operand ((condition).value ()), (again_label).value ())).has_value ());
  else if (transfer == ScopeTransfer::Switch) {
    const Label *labels[]{&(done_label).value (), &(again_label).value ()};
    REQUIRE ((builder.switch_to (Operand ((condition).value ()), labels)).has_value ());
  } else {
    auto choose_done
      = mirnext::ir::detail::access (fn).create_block_after ((b).value (), "choose-done",
                                                             &(inner).value ());
    REQUIRE ((choose_done).has_value ());
    auto choose_again
      = mirnext::ir::detail::access (fn).create_block_after ((choose_done).value (), "choose-again",
                                                             &(inner).value ());
    REQUIRE ((choose_again).has_value ());
    auto dispatch
      = mirnext::ir::detail::access (fn).create_block_after ((choose_again).value (), "dispatch",
                                                             &(inner).value ());
    REQUIRE ((dispatch).has_value ());
    auto choose_label
      = mirnext::ir::detail::access (fn).create_label ((choose_again).value (), "choose-again");
    REQUIRE ((choose_label).has_value ());
    auto dispatch_label
      = mirnext::ir::detail::access (fn).create_label ((dispatch).value (), "dispatch");
    REQUIRE ((dispatch_label).has_value ());
    REQUIRE (
      (builder.branch_if (Operand ((condition).value ()), (choose_label).value ())).has_value ());
    for (auto [block, label] : {std::pair{&(choose_done).value (), &(done_label).value ()},
                                std::pair{&(choose_again).value (), &(again_label).value ()}}) {
      REQUIRE ((builder.set_insert_point (*block)).has_value ());
      auto address = builder.address_of (*label);
      REQUIRE ((address).has_value ());
      REQUIRE ((builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (),
                                                                             &(scratch).value ()),
                               Operand ((address).value ())))
                 .has_value ());
      REQUIRE ((builder.jump ((dispatch_label).value ())).has_value ());
    }
    REQUIRE ((builder.set_insert_point ((dispatch).value ())).has_value ());
    REQUIRE ((builder.emit (Opcode::JmpIndirect,
                            {Operand (
                              mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (),
                                                                            &(scratch).value ()))}))
               .has_value ());
  }
  REQUIRE ((builder.set_insert_point ((again).value ())).has_value ());
  auto incremented
    = builder.binary (Opcode::Add, Operand ((counter).value ()), Operand (std::int64_t{1}));
  REQUIRE ((incremented).has_value ());
  REQUIRE ((builder.assign ((counter).value (), Operand ((incremented).value ()))).has_value ());
  REQUIRE ((builder.jump ((b_label).value ())).has_value ());
  REQUIRE ((builder.set_insert_point ((done).value ())).has_value ());
  auto inner_after = builder.stack_save ("inner-after");
  REQUIRE ((inner_after).has_value ());
  auto inner_balanced = builder.binary (Opcode::Eq, Operand ((outer_baseline).value ()),
                                        Operand ((inner_after).value ()));
  REQUIRE ((inner_balanced).has_value ());
  auto result
    = builder.load (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(slot).value ()),
                    "result");
  REQUIRE ((result).has_value ());
  auto checked_result = builder.binary (Opcode::Mul, Operand ((result).value ()),
                                        Operand ((inner_balanced).value ()));
  REQUIRE ((checked_result).has_value ());
  REQUIRE ((builder.jump ((exit_label).value ())).has_value ());
  REQUIRE ((builder.set_insert_point ((exit).value ())).has_value ());
  auto after = builder.stack_save ("after");
  REQUIRE ((after).has_value ());
  auto balanced
    = builder.binary (Opcode::Eq, Operand ((baseline).value ()), Operand ((after).value ()));
  REQUIRE ((balanced).has_value ());
  REQUIRE ((builder.branch_if (Operand ((balanced).value ()), (failure_label).value (), false))
             .has_value ());
  REQUIRE ((builder.set_insert_point ((success).value ())).has_value ());
  REQUIRE ((builder.ret (Operand ((checked_result).value ()))).has_value ());
  REQUIRE ((builder.set_insert_point ((failure).value ())).has_value ());
  REQUIRE ((builder.ret (Operand (std::int64_t{0}))).has_value ());
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
}  // namespace mirnext::test
