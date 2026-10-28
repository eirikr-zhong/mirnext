/* Licensed under the MIT License. Structural IR editing fixtures. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class RewriteExample { Fallthrough, Branch, Indirect, Switch, LabelData, Scope };
inline std::unique_ptr<ir::Program> rewrite_example (RewriteExample mode) {
  using namespace ir;
  auto context = std::make_unique<Program> ();
  auto module = mirnext::ir::detail::access (*(context)).create_module ("rewrites");
  REQUIRE ((module).has_value ());
  auto f = mirnext::ir::detail::access (((module)).value ())
             .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto &function = (f).value ();
  auto entry = mirnext::ir::detail::access (function).create_block ("entry");
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  if (mode == RewriteExample::Scope) REQUIRE ((builder.begin_stack_scope ()).has_value ());
  auto value = mirnext::ir::detail::access (function).create_register (Type::i64 (), "value");
  REQUIRE ((value).has_value ());
  REQUIRE ((builder.assign ((value).value (), 40)).has_value ());
  BasicBlock *body = const_cast<BasicBlock *> (builder.insert_block ());
  if (mode != RewriteExample::Fallthrough && mode != RewriteExample::Scope) {
    auto failure = mirnext::ir::detail::access (function).create_block ("failure");
    REQUIRE ((failure).has_value ());
    auto target = mirnext::ir::detail::access (function).create_block ("target");
    REQUIRE ((target).has_value ());
    body = &(target).value ();
    auto label = mirnext::ir::detail::access (function).create_label (*body, "target");
    REQUIRE ((label).has_value ());
    REQUIRE (
      (mirnext::ir::detail::access ((function)).create_label (*body, "unused")).has_value ());
    if (mode == RewriteExample::Branch)
      REQUIRE ((builder.branch_if (1, (label).value ())).has_value ());
    else if (mode == RewriteExample::Switch) {
      auto fail
        = mirnext::ir::detail::access (function).create_label ((failure).value (), "failure");
      REQUIRE ((fail).has_value ());
      const Label *labels[]{&(fail).value (), &(label).value (), &(label).value ()};
      REQUIRE ((builder.switch_to (1, labels)).has_value ());
    } else {
      Result<const Register &> address
        = Error{ErrorCode::InvalidArgument, "uninitialized fixture address"};
      if (mode == RewriteExample::LabelData) {
        auto data
          = mirnext::ir::detail::access (((module)).value ())
              .create_data ("destination", mirnext::ir::detail::NodeAccess::make_LabelReference (
                                             &(label).value ()));
        REQUIRE ((data).has_value ());
        auto pointer = builder.address_of (data.value ().symbol ());
        REQUIRE ((pointer).has_value ());
        address = builder.load (
          mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (), &(pointer).value ()));
        auto base = mirnext::ir::detail::access (function).create_label (*body, "base");
        REQUIRE ((base).has_value ());
        REQUIRE (
          (mirnext::ir::detail::access ((((module)).value ()))
             .create_data ("difference",
                           mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                                 &(base).value (),
                                                                                 7)))
            .has_value ());
      } else
        address = builder.address_of ((label).value ());
      REQUIRE ((address).has_value ());
      REQUIRE ((builder.emit (Opcode::JmpIndirect, {Operand ((address).value ())})).has_value ());
    }
    REQUIRE ((InstructionBuilder ((failure).value ()).ret (Operand (0))).has_value ());
    REQUIRE ((builder.set_insert_point (*body)).has_value ());
  }
  auto sum = builder.emit (Opcode::Add,
                           {Operand ((value).value ()), Operand ((value).value ()), Operand (2)});
  REQUIRE ((sum).has_value ());
  auto ret = builder.ret (Operand ((value).value ()));
  REQUIRE ((ret).has_value ());
  if (mode == RewriteExample::Scope)
    REQUIRE ((builder.end_stack_scope ((body->stack_scope ()).value ())).has_value ());
  InstructionRewriter rewriter (function);
  auto split = rewriter.split_before ((sum).value (), "suffix");
  REQUIRE ((split).has_value ());
  CHECK (&sum.value ().block () == &(split).value ());
  CHECK (&ret.value ().block () == &(split).value ());
  REQUIRE ((rewriter.merge_fallthrough (*body)).has_value ());
  CHECK (&sum.value ().block () == body);
  CHECK (&ret.value ().block () == body);
  REQUIRE ((rewriter.erase_unused_labels ()).has_value ());
  REQUIRE ((context->verify ()).has_value ());
  return context;
}
}  // namespace mirnext::test
