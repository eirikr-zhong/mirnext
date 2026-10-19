/* Licensed under the MIT License. Structural edits shared with the independent oracle. */
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::test {
enum class RewriteExample { Fallthrough, Branch, Indirect, Switch, LabelData, Scope };
inline std::unique_ptr<ir::Context> rewrite_example(RewriteExample mode) {
  using namespace ir;
  auto context = std::make_unique<Context>(); auto module = context->create_module("rewrites"); REQUIRE(module);
  auto f = (*module)->create_function("entry", {{Type::i64()}, {}}); REQUIRE(f); auto &function = **f;
  auto entry = function.create_block("entry"); REQUIRE(entry); IRBuilder builder(**entry);
  if (mode == RewriteExample::Scope) REQUIRE(builder.begin_stack_scope());
  auto value = function.create_register(Type::i64(), "value"); REQUIRE(value);
  REQUIRE(builder.assign(**value, 40));
  BasicBlock *body = const_cast<BasicBlock *>(builder.insert_block());
  if (mode != RewriteExample::Fallthrough && mode != RewriteExample::Scope) {
    auto failure = function.create_block("failure"); REQUIRE(failure);
    auto target = function.create_block("target"); REQUIRE(target); body = *target;
    auto label = function.create_label(*body, "target"); REQUIRE(label);
    REQUIRE(function.create_label(*body, "unused"));
    if (mode == RewriteExample::Branch) REQUIRE(builder.branch_if(1, **label));
    else if (mode == RewriteExample::Switch) {
      auto fail = function.create_label(**failure, "failure"); REQUIRE(fail);
      const Label *labels[]{*fail, *label, *label}; REQUIRE(builder.switch_to(1, labels));
    } else {
      Result<const Register *> address = Error{ErrorCode::InvalidArgument, "uninitialized fixture address"};
      if (mode == RewriteExample::LabelData) {
        auto data = (*module)->create_data("destination", LabelReference{*label}); REQUIRE(data);
        auto pointer = builder.address_of((*data)->symbol()); REQUIRE(pointer);
        address = builder.load(Memory{Type::pointer(), *pointer});
        auto base = function.create_label(*body, "base"); REQUIRE(base);
        REQUIRE((*module)->create_data("difference", LabelReference{*label, *base, 7}));
      } else address = builder.address_of(**label);
      REQUIRE(address); REQUIRE(builder.emit(Opcode::JmpIndirect, {Operand(**address)}));
    }
    REQUIRE(IRBuilder(**failure).ret(Operand(0))); REQUIRE(builder.set_insert_point(*body));
  }
  auto sum = builder.emit(Opcode::Add, {Operand(**value), Operand(**value), Operand(2)}); REQUIRE(sum);
  auto ret = builder.ret(Operand(**value)); REQUIRE(ret);
  if (mode == RewriteExample::Scope) REQUIRE(builder.end_stack_scope(*body->stack_scope()));
  IRRewriter rewriter(function); auto split = rewriter.split_before(**sum, "suffix"); REQUIRE(split);
  CHECK(&(*sum)->block() == *split); CHECK(&(*ret)->block() == *split);
  REQUIRE(rewriter.merge_fallthrough(*body)); CHECK(&(*sum)->block() == body); CHECK(&(*ret)->block() == body);
  REQUIRE(rewriter.erase_unused_labels()); REQUIRE(context->verify()); return context;
}
} // namespace mirnext::test
