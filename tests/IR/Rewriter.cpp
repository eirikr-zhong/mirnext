#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. Node identity, structural edit boundaries and atomic diagnostics.
 */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "PublicInstructions.hpp"
#include "Rewriter.hpp"
#include <array>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *entry;
  Register *value;
  Example () {
    auto m = mirnext::ir::detail::access (context).create_module ("rewriter");
    REQUIRE ((m).has_value ());
    module = &(m).value ();
    auto f
      = mirnext::ir::detail::access (*(module)).create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    function = &(f).value ();
    auto b = mirnext::ir::detail::access (*(function)).create_block ("entry");
    REQUIRE ((b).has_value ());
    entry = &(b).value ();
    auto r = mirnext::ir::detail::access (*(function)).create_register (Type::i64 (), "value");
    REQUIRE ((r).has_value ());
    value = &(r).value ();
  }
};
}  // namespace
TEST_CASE ("native rewriter insertion supports open blocks and preserves every existing node") {
  Example e;
  InstructionRewriter rewriter (*e.function);
  auto first = rewriter.insert (*e.entry, nullptr, Opcode::Mov, {Operand (*e.value), Operand (40)});
  REQUIRE ((first).has_value ());
  auto before
    = rewriter.insert_before ((first).value (), Opcode::Mov, {Operand (*e.value), Operand (0)});
  REQUIRE ((before).has_value ());
  auto sum = rewriter.insert_after ((first).value (), Opcode::Add,
                                    {Operand (*e.value), Operand (*e.value), Operand (2)});
  REQUIRE ((sum).has_value ());
  auto ret = rewriter.insert_after ((sum).value (), Opcode::Ret, {Operand (*e.value)});
  REQUIRE ((ret).has_value ());
  auto inserted = rewriter.insert_after ((first).value (), Opcode::Mov,
                                         {Operand (*e.value), Operand (*e.value)});
  REQUIRE ((inserted).has_value ());
  const std::array expected{&(before).value (), &(first).value (), &(inserted).value (),
                            &(sum).value (), &(ret).value ()};
  std::size_t n = 0;
  for (auto &instruction : e.entry->instructions ()) CHECK (&instruction == expected[n++]);
  CHECK (n == expected.size ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native rewriter rejects invalid insertion atomically and permits correction") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto first = builder.assign (*e.value, 42);
  REQUIRE ((first).has_value ());
  auto ret = builder.ret (Operand (*e.value));
  REQUIRE ((ret).has_value ());
  InstructionRewriter rewriter (*e.function);
  auto other = mirnext::ir::detail::access (*(e.function)).create_block ("other");
  REQUIRE ((other).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label ((other).value ());
  REQUIRE ((label).has_value ());
  REQUIRE ((InstructionBuilder ((other).value ()).ret (Operand (0))).has_value ());
  Example foreign;
  auto foreign_ret = InstructionBuilder (*foreign.entry).ret (Operand (0));
  REQUIRE ((foreign_ret).has_value ());
  const auto revisions = e.function->revisions ();
  const auto original = dump (e.context);
  CHECK_FALSE ((rewriter.insert (*e.entry, &(foreign_ret).value (), Opcode::Mov,
                                 {Operand (*e.value), Operand (1)}))
                 .has_value ());
  CHECK_FALSE (
    (rewriter.insert (*foreign.entry, nullptr, Opcode::Mov, {Operand (*e.value), Operand (1)}))
      .has_value ());
  CHECK_FALSE (
    (rewriter.insert_before ((first).value (), Opcode::Jmp, {Operand ((label).value ())}))
      .has_value ());
  CHECK_FALSE ((rewriter.insert_after ((first).value (), Opcode::Ret, {Operand (0)})).has_value ());
  CHECK_FALSE (
    (rewriter.insert_after ((ret).value (), Opcode::Mov, {Operand (*e.value), Operand (1)}))
      .has_value ());
  CHECK_FALSE ((rewriter.insert (*e.entry, nullptr, Opcode::Mov, {Operand (*e.value), Operand (1)}))
                 .has_value ());
  CHECK_FALSE (
    (rewriter.insert_before ((ret).value (), Opcode::Mov, {Operand (*e.value), Operand (1.0)}))
      .has_value ());
  CHECK_FALSE (
    (rewriter.insert_before ((ret).value (), static_cast<Opcode> (-1), {})).has_value ());
  CHECK (e.function->revisions () == revisions);
  CHECK (dump (e.context) == original);
  CHECK_FALSE ((e.module->error ()).has_value ());
  REQUIRE (
    (rewriter.insert_before ((ret).value (), Opcode::Mov, {Operand (*e.value), Operand (42)}))
      .has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE (
  "native splitting and merging transfer instructions without copying operands or labels") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto first = builder.assign (*e.value, 40);
  REQUIRE ((first).has_value ());
  auto sum = builder.emit (Opcode::Add, {Operand (*e.value), Operand (*e.value), Operand (2)});
  REQUIRE ((sum).has_value ());
  auto ret = builder.ret (Operand (*e.value));
  REQUIRE ((ret).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label (*e.entry, "entry");
  REQUIRE ((label).has_value ());
  auto alias = mirnext::ir::detail::access (*(e.function)).create_label (*e.entry, "alias");
  REQUIRE ((alias).has_value ());
  auto operands = sum.value ().operands ().data ();
  auto ret_operands = ret.value ().operands ().data ();
  auto first_iterator = e.entry->instructions ().begin ();
  auto sum_iterator = std::next (first_iterator);
  InstructionRewriter rewriter (*e.function);
  const auto revision = e.function->revisions ();
  auto split = rewriter.split_before ((sum).value (), "suffix");
  REQUIRE ((split).has_value ());
  CHECK (e.entry->size () == 1);
  CHECK (split.value ().size () == 2);
  CHECK (&label.value ().block () == e.entry);
  CHECK (&alias.value ().block () == e.entry);
  CHECK (split.value ().labels ().empty ());
  CHECK (&sum.value ().block () == &(split).value ());
  CHECK (&ret.value ().block () == &(split).value ());
  CHECK (&*sum_iterator == &(sum).value ());
  CHECK (sum.value ().operands ().data () == operands);
  CHECK (ret.value ().operands ().data () == ret_operands);
  CHECK (e.function->find_block ("suffix").get () == &(split).value ());
  CHECK (e.function->revisions ().control_flow > revision.control_flow);
  REQUIRE ((e.context.verify ()).has_value ());
  REQUIRE ((rewriter.merge_fallthrough (*e.entry)).has_value ());
  CHECK (e.function->find_block ("suffix").get () == nullptr);
  CHECK (e.entry->size () == 3);
  CHECK (&sum.value ().block () == e.entry);
  CHECK (&ret.value ().block () == e.entry);
  CHECK (&*sum_iterator == &(sum).value ());
  CHECK (sum.value ().operands ().data () == operands);
  REQUIRE ((e.context.verify ()).has_value ());
  REQUIRE ((mirnext::ir::detail::access (*(e.function)).create_block_before (*e.entry, "suffix"))
             .has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native splits at the first instruction and terminator retain fallthrough semantics") {
  for (bool at_first : {false, true}) {
    Example e;
    InstructionBuilder builder (*e.entry);
    auto first = builder.assign (*e.value, 42);
    REQUIRE ((first).has_value ());
    auto ret = builder.ret (Operand (*e.value));
    REQUIRE ((ret).has_value ());
    InstructionRewriter rewriter (*e.function);
    auto split = rewriter.split_before (at_first ? (first).value () : (ret).value ());
    REQUIRE ((split).has_value ());
    CHECK (e.entry->size () == (at_first ? 0 : 1));
    CHECK (split.value ().size () == (at_first ? 2 : 1));
    REQUIRE ((e.context.verify ()).has_value ());
    REQUIRE ((rewriter.merge_fallthrough (*e.entry)).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native split failures do not allocate a block or change revisions") {
  Example e;
  auto ret = InstructionBuilder (*e.entry).ret (Operand (42));
  REQUIRE ((ret).has_value ());
  Example other;
  auto foreign = InstructionBuilder (*other.entry).ret (Operand (0));
  REQUIRE ((foreign).has_value ());
  InstructionRewriter rewriter (*e.function);
  auto original = dump (e.context);
  auto revision = e.function->revisions ();
  CHECK_FALSE ((rewriter.split_before ((ret).value (), "entry")).has_value ());
  CHECK_FALSE ((rewriter.split_before ((foreign).value ())).has_value ());
  CHECK_FALSE (
    (rewriter.split_before ((ret).value (), std::string_view ("bad\0name", 8))).has_value ());
  CHECK (e.function->revisions () == revision);
  CHECK (dump (e.context) == original);
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE (
  "native merging rejects labels terminators scope changes and foreign ownership atomically") {
  Example e;
  InstructionRewriter rewriter (*e.function);
  Example other;
  auto next = mirnext::ir::detail::access (*(e.function)).create_block ("next");
  REQUIRE ((next).has_value ());
  SUBCASE ("label anchor") {
    REQUIRE (
      (mirnext::ir::detail::access (*(e.function)).create_label ((next).value ())).has_value ());
  }
  SUBCASE ("terminator") {
    REQUIRE ((InstructionBuilder (*e.entry).ret (Operand (42))).has_value ());
  }
  SUBCASE ("scope boundary") {
    auto scope = mirnext::ir::detail::access (*(e.function)).create_stack_scope ();
    REQUIRE ((scope).has_value ());
    REQUIRE ((rewriter.set_stack_scope ((next).value (), &(scope).value ())).has_value ());
  }
  REQUIRE ((InstructionBuilder ((next).value ()).ret (Operand (42))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto original = dump (e.context);
  auto revision = e.function->revisions ();
  CHECK_FALSE ((rewriter.merge_fallthrough (*e.entry)).has_value ());
  CHECK_FALSE ((rewriter.merge_fallthrough ((next).value ())).has_value ());
  CHECK_FALSE ((rewriter.merge_fallthrough (*other.entry)).has_value ());
  CHECK (e.function->revisions () == revision);
  CHECK (dump (e.context) == original);
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native label cleanup retains branches switch cases addresses and both data anchors") {
  Example e;
  auto target = mirnext::ir::detail::access (*(e.function)).create_block ("target");
  REQUIRE ((target).has_value ());
  auto unused
    = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "unused");
  REQUIRE ((unused).has_value ());
  auto direct
    = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "direct");
  REQUIRE ((direct).has_value ());
  auto indirect
    = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "indirect");
  REQUIRE ((indirect).has_value ());
  auto data = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "data");
  REQUIRE ((data).has_value ());
  auto base = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "base");
  REQUIRE ((base).has_value ());
  auto case_alias
    = mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "case");
  REQUIRE ((case_alias).has_value ());
  auto switch_block = mirnext::ir::detail::access (*(e.function)).create_block ("switch");
  REQUIRE ((switch_block).has_value ());
  InstructionBuilder builder (*e.entry);
  REQUIRE ((builder.address_of ((indirect).value ())).has_value ());
  REQUIRE ((builder.jump ((direct).value ())).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (42))).has_value ());
  REQUIRE ((InstructionBuilder ((switch_block).value ())
              .emit (Opcode::Switch, {Operand (0), Operand ((case_alias).value ()),
                                      Operand ((case_alias).value ())}))
             .has_value ());
  auto ending = mirnext::ir::detail::access (*(e.function)).create_block ("ending");
  REQUIRE ((ending).has_value ());
  REQUIRE ((InstructionBuilder ((ending).value ()).ret (Operand (0))).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (*(e.module))
       .create_data ("difference",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(data).value (),
                                                                           &(base).value (), 8)))
      .has_value ());
  InstructionRewriter rewriter (*e.function);
  auto revision = e.function->revisions ();
  auto erased = rewriter.erase_unused_labels ();
  REQUIRE ((erased).has_value ());
  CHECK ((erased).value () == 1);
  CHECK (e.function->find_label ("unused").get () == nullptr);
  CHECK (target.value ().labels ().size () == 5);
  CHECK (e.function->find_label ("direct").get () == &(direct).value ());
  CHECK (e.function->find_label ("indirect").get () == &(indirect).value ());
  CHECK (e.function->find_label ("data").get () == &(data).value ());
  CHECK (e.function->find_label ("base").get () == &(base).value ());
  CHECK (e.function->find_label ("case").get () == &(case_alias).value ());
  REQUIRE ((e.context.verify ()).has_value ());
  CHECK (e.function->revisions ().control_flow > revision.control_flow);
  revision = e.function->revisions ();
  erased = rewriter.erase_unused_labels ();
  REQUIRE ((erased).has_value ());
  CHECK ((erased).value () == 0);
  CHECK (e.function->revisions () == revision);
  REQUIRE ((mirnext::ir::detail::access (*(e.function)).create_label ((target).value (), "unused"))
             .has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native named block indexing survives clones insertions and removed names") {
  Example e;
  for (unsigned n = 0; n < 4096; ++n) {
    auto name = "block" + std::to_string (n);
    auto block = mirnext::ir::detail::access (*(e.function)).create_block_after (*e.entry, name);
    REQUIRE ((block).has_value ());
    CHECK (e.function->find_block (name).get () == &(block).value ());
  }
  auto &last = *std::prev (e.function->blocks ().end ());
  REQUIRE ((InstructionBuilder (last).ret (Operand (42))).has_value ());
  auto copied = clone (e.context);
  REQUIRE ((copied).has_value ());
  auto &f = *copied.value ().modules ().begin ()->functions ().begin ();
  CHECK (f.find_block ("entry").get () != e.entry);
  CHECK (f.find_block ("missing").get () == nullptr);
  CHECK (f.find_block ("").get () == nullptr);
  for (auto &block : f.blocks ()) CHECK (f.find_block (block.name ()).get () == &block);
  CHECK (dump ((copied).value ()) == dump (e.context));
  REQUIRE ((copied.value ().verify ()).has_value ());
}
TEST_CASE ("native structural edits cover every public opcode with stable instruction storage") {
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = test::public_instruction_example (descriptor);
    auto &function = *context->modules ().begin ()->functions ().begin ();
    InstructionRewriter rewriter (function);
    std::vector<Instruction *> instructions;
    for (auto &block : function.blocks ())
      for (auto &instruction : block.instructions ()) instructions.push_back (&instruction);
    for (auto instruction : instructions) {
      auto &source = const_cast<BasicBlock &> (instruction->block ());
      auto operands = instruction->operands ().data ();
      auto split = rewriter.split_before (*instruction);
      REQUIRE ((split).has_value ());
      CHECK (instruction->operands ().data () == operands);
      CHECK (&instruction->block () == &(split).value ());
      REQUIRE ((rewriter.merge_fallthrough (source)).has_value ());
      CHECK (&instruction->block () == &source);
      CHECK (instruction->operands ().data () == operands);
    }
    REQUIRE ((context->verify ()).has_value ());
  }
}
TEST_CASE (
  "native structural fixtures clone and roundtrip text and binary without mutating source") {
  for (unsigned n = 0; n <= static_cast<unsigned> (test::RewriteExample::Scope); ++n) {
    CAPTURE (n);
    auto context = test::rewrite_example (static_cast<test::RewriteExample> (n));
    auto original = dump (*context);
    auto copy = clone (*context);
    REQUIRE ((copy).has_value ());
    CHECK (dump ((copy).value ()) == original);
    auto text = serialization::write_text (*context);
    REQUIRE ((text).has_value ());
    auto parsed = serialization::parse_text ((text).value ());
    REQUIRE ((parsed).has_value ());
    auto second = serialization::write_text ((parsed).value ());
    REQUIRE ((second).has_value ());
    CHECK ((second).value () == (text).value ());
    auto bytes = serialization::write_binary (*context);
    REQUIRE ((bytes).has_value ());
    auto decoded = serialization::read_binary ((bytes).value ());
    REQUIRE ((decoded).has_value ());
    auto reencoded = serialization::write_binary ((decoded).value ());
    REQUIRE ((reencoded).has_value ());
    CHECK ((reencoded).value () == (bytes).value ());
    CHECK (dump (*context) == original);
  }
}
