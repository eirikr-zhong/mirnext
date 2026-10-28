/* Licensed under the MIT License. Stack scope lowering tests. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "StackScopes.hpp"
#include <algorithm>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
std::vector<Opcode> opcodes (const BasicBlock &block) {
  std::vector<Opcode> result;
  for (auto &i : block.instructions ()) result.push_back (i.opcode ());
  return result;
}
Function &function (Program &context) {
  return *context.modules ().begin ()->functions ().begin ();
}
}  // namespace
TEST_CASE ("scope DSL returns load stack values before automatic restoration") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto outer = builder.begin_stack_scope ("outer");
  REQUIRE ((outer).has_value ());
  auto inner = builder.begin_stack_scope ("inner");
  REQUIRE ((inner).has_value ());
  auto block = const_cast<BasicBlock *> (builder.insert_block ());
  auto slot = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE ((slot).has_value ());
  REQUIRE (
    (builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(slot).value ()),
                    Operand (std::int64_t{42})))
      .has_value ());
  auto returned = builder.ret (
    Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(slot).value ())));
  REQUIRE ((returned).has_value ());
  REQUIRE ((builder.end_stack_scope ((inner).value ())).has_value ());
  REQUIRE ((builder.end_stack_scope ((outer).value ())).has_value ());
  CHECK (builder.insert_block () == nullptr);
  auto before = dump (context);
  auto copied = clone (context);
  REQUIRE ((copied).has_value ());
  CHECK (dump ((copied).value ()) == before);
  auto result = materialize_stack_scopes (context);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().saves == 2);
  CHECK (result.value ().restores == 2);
  CHECK (opcodes (*block)
         == std::vector{Opcode::Alloca, Opcode::Mov, Opcode::Mov, Opcode::BEnd, Opcode::BEnd,
                        Opcode::Ret});
  CHECK (
    mirnext::ir::detail::operand_access (returned.value ().operands ()[0]).get<const Register *> ()
    != nullptr);
  CHECK (&returned.value ().block () == block);
  REQUIRE ((context.verify ()).has_value ());
  CHECK_FALSE (needs_materialization (context));
  auto stable = dump (context);
  auto again = materialize_stack_scopes (context);
  REQUIRE ((again).has_value ());
  CHECK (again.value ().saves == 0);
  CHECK (dump (context) == stable);
  auto cloned = clone (context);
  REQUIRE ((cloned).has_value ());
  CHECK (dump ((cloned).value ()) == stable);
}
TEST_CASE (
  "materialization is explicit and format writing leaves source nodes and revisions intact") {
  for (auto mode :
       {test::ScopeTransfer::Branch, test::ScopeTransfer::Switch, test::ScopeTransfer::Indirect}) {
    auto context = test::stack_scope_example (mode);
    auto &fn = function (*context);
    auto before = dump (*context);
    auto revisions = fn.revisions ();
    auto instruction = &*fn.blocks ().begin ()->instructions ().begin ();
    auto text = serialization::write_text (*context);
    INFO (((text).has_value () ? "" : text.error ().message));
    REQUIRE ((text).has_value ());
    auto bytes = serialization::write_binary (*context);
    INFO (((bytes).has_value () ? "" : bytes.error ().message));
    REQUIRE ((bytes).has_value ());
    CHECK (dump (*context) == before);
    CHECK (fn.revisions () == revisions);
    CHECK (instruction == &*fn.blocks ().begin ()->instructions ().begin ());
    CHECK (needs_materialization (*context));
    auto lowered = materialize (*context);
    INFO (((lowered).has_value () ? "" : lowered.error ().message));
    REQUIRE ((lowered).has_value ());
    REQUIRE ((lowered.value ().verify ()).has_value ());
    CHECK_FALSE (needs_materialization ((lowered).value ()));
    auto decoded = serialization::read_binary ((bytes).value ());
    REQUIRE ((decoded).has_value ());
    CHECK_FALSE (needs_materialization ((decoded).value ()));
    auto encoded = serialization::write_binary ((decoded).value ());
    REQUIRE ((encoded).has_value ());
    CHECK ((encoded).value () == (bytes).value ());
    auto parsed = serialization::parse_text ((text).value ());
    REQUIRE ((parsed).has_value ());
    auto formatted = serialization::write_text ((parsed).value ());
    REQUIRE ((formatted).has_value ());
    CHECK ((formatted).value () == (text).value ());
  }
}
TEST_CASE (
  "same scope loop edges retain their identity and conditional exits restore only their edge") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Branch);
  auto &fn = function (*context);
  auto again = fn.find_label ("again").get (), inner = fn.find_label ("inner").get (),
       done = fn.find_label ("done").get ();
  REQUIRE (again);
  REQUIRE (inner);
  REQUIRE (done);
  auto branch = &*std::prev (const_cast<BasicBlock &> (inner->block ()).instructions ().end ());
  auto loop = &*std::prev (const_cast<BasicBlock &> (again->block ()).instructions ().end ());
  auto result = materialize_stack_scopes (fn);
  REQUIRE ((result).has_value ());
  CHECK (*mirnext::ir::detail::operand_access (branch->operands ()[0]).get<const Label *> ()
         == again);
  CHECK (*mirnext::ir::detail::operand_access (loop->operands ()[0]).get<const Label *> ()
         == inner);
  auto inner_codes = opcodes (inner->block ());
  CHECK (std::count (inner_codes.begin (), inner_codes.end (), Opcode::BEnd) == 0);
  auto blocks = fn.blocks ();
  auto source = std::find_if (blocks.begin (), blocks.end (),
                              [&] (auto &b) { return &b == &inner->block (); });
  REQUIRE (source != blocks.end ());
  auto &cleanup = *std::next (source);
  CHECK (opcodes (cleanup) == std::vector{Opcode::BEnd, Opcode::Jmp});
  CHECK (*mirnext::ir::detail::operand_access (cleanup.terminator ().value ().operands ()[0])
            .get<const Label *> ()
         == done);
  REQUIRE ((fn.verify ()).has_value ());
}
TEST_CASE ("switch exits share cleanup blocks and preserve repeated destinations") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Switch);
  auto &fn = function (*context);
  auto inner = fn.find_label ("inner").get (), done = fn.find_label ("done").get (),
       again = fn.find_label ("again").get ();
  REQUIRE (inner);
  REQUIRE (done);
  REQUIRE (again);
  auto &instruction
    = *std::prev (const_cast<BasicBlock &> (inner->block ()).instructions ().end ());
  const Operand operands[]{instruction.operands ()[0], Operand (*done), Operand (*again),
                           Operand (*done)};
  REQUIRE ((InstructionRewriter (fn).replace (instruction, Opcode::Switch, operands)).has_value ());
  auto result = materialize_stack_scopes (fn);
  REQUIRE ((result).has_value ());
  CHECK (instruction.operands ()[1] == instruction.operands ()[3]);
  CHECK (*mirnext::ir::detail::operand_access (instruction.operands ()[2]).get<const Label *> ()
         == again);
  auto cleanup
    = *mirnext::ir::detail::operand_access (instruction.operands ()[1]).get<const Label *> ();
  CHECK (cleanup != done);
  CHECK (opcodes (cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::Jmp});
}
TEST_CASE ("scoped function entry is saved once even with a backedge to its first block") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn = mirnext::ir::detail::access (((module)).value ())
              .create_function ("f", {{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((fn).has_value ());
  auto scope = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope ();
  REQUIRE ((scope).has_value ());
  auto entry
    = mirnext::ir::detail::access (((fn)).value ()).create_block ("entry", &(scope).value ());
  REQUIRE ((entry).has_value ());
  auto exit
    = mirnext::ir::detail::access (((fn)).value ()).create_block ("exit", &(scope).value ());
  REQUIRE ((exit).has_value ());
  auto label = mirnext::ir::detail::access (((fn)).value ()).create_label ((entry).value ());
  REQUIRE ((label).has_value ());
  InstructionBuilder builder ((entry).value ());
  REQUIRE ((builder.allocate_stack (Operand (std::int64_t{16}))).has_value ());
  REQUIRE ((builder.branch_if (Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                                 fn.value ().arguments ())[0]),
                               (label).value ()))
             .has_value ());
  REQUIRE ((InstructionBuilder ((exit).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto result = materialize_stack_scopes ((fn).value ());
  REQUIRE ((result).has_value ());
  CHECK (result.value ().saves == 1);
  CHECK (result.value ().restores == 1);
  CHECK (opcodes (*fn.value ().blocks ().begin ()) == std::vector{Opcode::BStart, Opcode::Jmp});
  CHECK (opcodes ((entry).value ()) == std::vector{Opcode::Alloca, Opcode::Bt});
}
TEST_CASE ("mixed indirect targets evaluate memory once and leave stack cleanup after dispatch") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Indirect);
  auto &fn = function (*context);
  auto dispatch = fn.find_label ("dispatch").get ();
  REQUIRE (dispatch);
  auto source = &dispatch->block ();
  auto result = materialize_stack_scopes (fn);
  REQUIRE ((result).has_value ());
  CHECK (result.value ().indirect_dispatches == 1);
  auto codes = opcodes (*source);
  CHECK (codes == std::vector{Opcode::Mov, Opcode::LAddr, Opcode::Beq});
  auto &first = *source->instructions ().begin ();
  CHECK (first.operands ()[1].get<Memory> ().has_value ());
  auto destination
    = *mirnext::ir::detail::operand_access (first.operands ()[0]).get<const Register *> ();
  std::size_t reads = 0;
  for (auto &block : fn.blocks ())
    for (auto &i : block.instructions ())
      for (auto &operand : i.operands ())
        if (auto reg = mirnext::ir::detail::operand_access (operand).get<const Register *> ();
            reg && *reg == destination)
          ++reads;
  CHECK (reads > 2);
  REQUIRE ((fn.verify ()).has_value ());
}
TEST_CASE ("same scope indirect targets still retain cleanup for an unknown destination") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  auto scope = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope ();
  REQUIRE ((scope).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ({}, &(scope).value ());
  REQUIRE ((entry).has_value ());
  auto target = mirnext::ir::detail::access (((fn)).value ()).create_block ({}, &(scope).value ());
  REQUIRE ((target).has_value ());
  auto label = mirnext::ir::detail::access (((fn)).value ()).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto address = builder.address_of ((label).value ());
  REQUIRE ((address).has_value ());
  REQUIRE ((builder.emit (Opcode::JmpIndirect, {Operand ((address).value ())})).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto lowered = materialize_stack_scopes ((fn).value ());
  REQUIRE ((lowered).has_value ());
  CHECK (lowered.value ().indirect_dispatches == 1);
  CHECK (entry.value ().terminator ().value ().opcode () == Opcode::Beq);
  auto blocks = fn.value ().blocks ();
  auto source = std::find_if (blocks.begin (), blocks.end (),
                              [&] (auto &b) { return &b == &(entry).value (); });
  REQUIRE (source != blocks.end ());
  CHECK (opcodes (*std::next (source)) == std::vector{Opcode::BEnd, Opcode::JmpIndirect});
  CHECK (*mirnext::ir::detail::operand_access (entry.value ().terminator ().value ().operands ()[0])
            .get<const Label *> ()
         == &(label).value ());
}
TEST_CASE ("scope errors retain first builder diagnostic and rejected edits are atomic") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {});
  REQUIRE ((fn).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto a = builder.begin_stack_scope ("a");
  REQUIRE ((a).has_value ());
  auto b = builder.begin_stack_scope ("b");
  REQUIRE ((b).has_value ());
  auto before = fn.value ().instruction_count ();
  CHECK_FALSE ((builder.end_stack_scope ((a).value ())).has_value ());
  auto first = module.value ().error ();
  REQUIRE ((first).has_value ());
  CHECK_FALSE ((builder.ret ()).has_value ());
  CHECK (module.value ().error ().value ().message == first.value ().message);
  CHECK (fn.value ().instruction_count () == before);
  Program other;
  auto om = mirnext::ir::detail::access (other).create_module ("other");
  REQUIRE ((om).has_value ());
  auto of = mirnext::ir::detail::access (((om)).value ()).create_function ("g", {});
  REQUIRE ((of).has_value ());
  auto os = mirnext::ir::detail::access (((of)).value ()).create_stack_scope ();
  REQUIRE ((os).has_value ());
  auto block = mirnext::ir::detail::access (((of)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto revision = of.value ().revisions ();
  CHECK_FALSE (
    (InstructionRewriter ((of).value ()).set_stack_scope ((block).value (), &(a).value ()))
      .has_value ());
  CHECK (of.value ().revisions () == revision);
  CHECK_FALSE (
    (mirnext::ir::detail::access (((of)).value ()).create_block ("foreign", &(a).value ()))
      .has_value ());
}
TEST_CASE ("sibling transitions restore then save and homogeneous indirect exits avoid dispatch") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  auto outer = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope ();
  REQUIRE ((outer).has_value ());
  auto a = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope (&(outer).value ());
  REQUIRE ((a).has_value ());
  auto b = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope (&(outer).value ());
  REQUIRE ((b).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ("entry", &(a).value ());
  REQUIRE ((entry).has_value ());
  auto sibling
    = mirnext::ir::detail::access (((fn)).value ()).create_block ("sibling", &(b).value ());
  REQUIRE ((sibling).has_value ());
  auto exit = mirnext::ir::detail::access (((fn)).value ()).create_block ("exit");
  REQUIRE ((exit).has_value ());
  auto exit2 = mirnext::ir::detail::access (((fn)).value ()).create_block ("exit2");
  REQUIRE ((exit2).has_value ());
  auto sibling_label
    = mirnext::ir::detail::access (((fn)).value ()).create_label ((sibling).value ());
  REQUIRE ((sibling_label).has_value ());
  auto exit_label = mirnext::ir::detail::access (((fn)).value ()).create_label ((exit).value ());
  REQUIRE ((exit_label).has_value ());
  auto exit2_label = mirnext::ir::detail::access (((fn)).value ()).create_label ((exit2).value ());
  REQUIRE ((exit2_label).has_value ());
  REQUIRE ((InstructionBuilder ((entry).value ()).jump ((sibling_label).value ())).has_value ());
  InstructionBuilder builder ((sibling).value ());
  auto destination = builder.address_of ((exit_label).value ());
  REQUIRE ((destination).has_value ());
  auto jumped = builder.emit (Opcode::JmpIndirect, {Operand ((destination).value ())});
  REQUIRE ((jumped).has_value ());
  REQUIRE ((InstructionBuilder ((exit).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  REQUIRE ((InstructionBuilder ((exit2).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto result = materialize_stack_scopes ((fn).value ());
  REQUIRE ((result).has_value ());
  auto entry_jump = entry.value ().terminator ().get ();
  REQUIRE (entry_jump);
  auto cleanup
    = *mirnext::ir::detail::operand_access (entry_jump->operands ()[0]).get<const Label *> ();
  CHECK (opcodes (cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::BStart, Opcode::Jmp});
  CHECK (result.value ().indirect_dispatches
         == 1);  // The sibling label is also a valid in-scope target.

  Program homogeneous;
  auto hm = mirnext::ir::detail::access (homogeneous).create_module ("m");
  REQUIRE ((hm).has_value ());
  auto hf
    = mirnext::ir::detail::access (((hm)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((hf).has_value ());
  auto scope = mirnext::ir::detail::access (((hf)).value ()).create_stack_scope ();
  REQUIRE ((scope).has_value ());
  auto source = mirnext::ir::detail::access (((hf)).value ()).create_block ({}, &(scope).value ());
  REQUIRE ((source).has_value ());
  auto target = mirnext::ir::detail::access (((hf)).value ()).create_block ();
  REQUIRE ((target).has_value ());
  auto label = mirnext::ir::detail::access (((hf)).value ()).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  InstructionBuilder source_builder ((source).value ());
  auto slot = source_builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE ((slot).has_value ());
  auto address = source_builder.address_of ((label).value ());
  REQUIRE ((address).has_value ());
  REQUIRE ((source_builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (),
                                                                                &(slot).value ()),
                                  Operand ((address).value ())))
             .has_value ());
  auto transfer
    = source_builder.emit (Opcode::JmpIndirect,
                           {Operand (
                             mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (),
                                                                           &(slot).value ()))});
  REQUIRE ((transfer).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto lowered = materialize_stack_scopes ((hf).value ());
  REQUIRE ((lowered).has_value ());
  CHECK (lowered.value ().indirect_dispatches == 0);
  CHECK (opcodes ((source).value ())
         == std::vector{Opcode::Alloca, Opcode::LAddr, Opcode::Mov, Opcode::Mov, Opcode::BEnd,
                        Opcode::JmpIndirect});
  CHECK (transfer.value ().opcode () == Opcode::JmpIndirect);
  CHECK (
    mirnext::ir::detail::operand_access (transfer.value ().operands ()[0]).get<const Register *> ()
    != nullptr);
}
TEST_CASE (
  "value DSL chooses precise address opcodes and rejects invalid values before publishing "
  "registers") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto data
    = mirnext::ir::detail::access (((module)).value ())
        .create_data ("global", InitializedData{Type::i64 (), {Operand (std::int64_t{42})}});
  REQUIRE ((data).has_value ());
  for (auto [type, opcode] :
       {std::pair{Type (Type::Kind::U8), Opcode::Addr8},
        std::pair{Type (Type::Kind::I16), Opcode::Addr16},
        std::pair{Type (Type::Kind::U32), Opcode::Addr32}, std::pair{Type::i64 (), Opcode::Addr}}) {
    auto variable = mirnext::ir::detail::access (((fn)).value ()).create_register (type);
    REQUIRE ((variable).has_value ());
    auto address = builder.address_of ((variable).value ());
    REQUIRE ((address).has_value ());
    CHECK (std::prev (entry.value ().instructions ().end ())->opcode () == opcode);
    CHECK (address.value ().type () == Type::pointer ());
  }
  auto address = builder.address_of (data.value ().symbol (), 8);
  REQUIRE ((address).has_value ());
  CHECK (std::prev (entry.value ().instructions ().end ())->opcode () == Opcode::Mov);
  auto converted = builder.unary (Opcode::I2D, Operand (std::int64_t{42}));
  REQUIRE ((converted).has_value ());
  CHECK (converted.value ().type () == Type (Type::Kind::F64));
  auto slot = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE ((slot).has_value ());
  REQUIRE ((builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::F64),
                                                                         &(slot).value ()),
                           Operand ((converted).value ())))
             .has_value ());
  auto loaded = builder.load (
    mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::F64), &(slot).value ()));
  REQUIRE ((loaded).has_value ());
  CHECK (std::prev (entry.value ().instructions ().end ())->opcode () == Opcode::DMov);
  REQUIRE ((builder.ret (Operand (std::int64_t{42}))).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  auto bad = mirnext::ir::detail::access (((module)).value ()).create_function ("bad", {});
  REQUIRE ((bad).has_value ());
  auto bad_entry = mirnext::ir::detail::access (((bad)).value ()).create_block ();
  REQUIRE ((bad_entry).has_value ());
  InstructionBuilder invalid ((bad_entry).value ());
  CHECK_FALSE ((invalid.allocate_stack (Operand (1.0))).has_value ());
  CHECK (bad.value ().registers ().begin () == bad.value ().registers ().end ());
  CHECK (bad.value ().instruction_count () == 0);
}
TEST_CASE (
  "explicit stack operations retain order and overflow branches clean up after testing flags") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  auto entry = mirnext::ir::detail::access (((fn)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto scope = builder.begin_stack_scope ();
  REQUIRE ((scope).has_value ());
  auto explicit_save = builder.stack_save ("explicit");
  REQUIRE ((explicit_save).has_value ());
  auto restored = builder.stack_restore ((explicit_save).value ());
  REQUIRE ((restored).has_value ());
  auto value = builder.binary (Opcode::Addo, Operand (INT64_MAX), Operand (std::int64_t{1}));
  REQUIRE ((value).has_value ());
  auto fallback = mirnext::ir::detail::access (((fn)).value ()).create_block ("fallback");
  REQUIRE ((fallback).has_value ());
  auto target = mirnext::ir::detail::access (((fn)).value ()).create_block ("target");
  REQUIRE ((target).has_value ());
  auto label = mirnext::ir::detail::access (((fn)).value ()).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  auto branch = builder.emit (Opcode::Bo, {Operand ((label).value ())});
  REQUIRE ((branch).has_value ());
  REQUIRE ((InstructionBuilder ((fallback).value ()).ret (Operand (std::int64_t{0}))).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto lowered = materialize_stack_scopes ((fn).value ());
  REQUIRE ((lowered).has_value ());
  auto body = &branch.value ().block ();
  CHECK (opcodes (*body) == std::vector{Opcode::BStart, Opcode::BEnd, Opcode::Addo, Opcode::Bo});
  CHECK (restored.value ().operands ()[0] == Operand ((explicit_save).value ()));
  REQUIRE ((fn.value ().verify ()).has_value ());
  auto target_cleanup
    = *mirnext::ir::detail::operand_access (branch.value ().operands ()[0]).get<const Label *> ();
  CHECK (opcodes (target_cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::Jmp});

  Program invalid;
  auto im = mirnext::ir::detail::access (invalid).create_module ("invalid");
  REQUIRE ((im).has_value ());
  auto iff
    = mirnext::ir::detail::access (((im)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((iff).has_value ());
  auto ib = mirnext::ir::detail::access (((iff)).value ()).create_block ();
  REQUIRE ((ib).has_value ());
  InstructionBuilder bad ((ib).value ());
  auto is = bad.begin_stack_scope ();
  REQUIRE ((is).has_value ());
  REQUIRE (
    (bad.binary (Opcode::Addo, Operand (INT64_MAX), Operand (std::int64_t{1}))).has_value ());
  REQUIRE ((bad.end_stack_scope ((is).value ())).has_value ());
  auto ir = mirnext::ir::detail::access (((iff)).value ()).create_block ();
  REQUIRE ((ir).has_value ());
  auto il = mirnext::ir::detail::access (((iff)).value ()).create_label ((ir).value ());
  REQUIRE ((il).has_value ());
  REQUIRE ((bad.emit (Opcode::Bo, {Operand ((il).value ())})).has_value ());
  REQUIRE ((InstructionBuilder ((ir).value ()).ret (Operand (std::int64_t{42}))).has_value ());
  auto snapshot = dump (invalid);
  CHECK_FALSE ((materialize_stack_scopes (invalid)).has_value ());
  CHECK (dump (invalid) == snapshot);
}
TEST_CASE ("scope lowering and cloning are iterative for deeply nested block scopes") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("m");
  REQUIRE ((module).has_value ());
  auto fn
    = mirnext::ir::detail::access (((module)).value ()).create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE ((fn).has_value ());
  const StackScope *parent = nullptr;
  constexpr std::size_t depth = 4096;
  for (std::size_t n = 0; n < depth; ++n) {
    auto scope = mirnext::ir::detail::access (((fn)).value ()).create_stack_scope (parent);
    REQUIRE ((scope).has_value ());
    parent = &(scope).value ();
    auto block = mirnext::ir::detail::access (((fn)).value ()).create_block ({}, parent);
    REQUIRE ((block).has_value ());
  }
  REQUIRE ((InstructionBuilder (*std::prev (fn.value ().blocks ().end ()))
              .ret (Operand (std::int64_t{42})))
             .has_value ());
  auto copy = clone (context);
  REQUIRE ((copy).has_value ());
  auto lowered = materialize_stack_scopes ((copy).value ());
  REQUIRE ((lowered).has_value ());
  CHECK (lowered.value ().saves == depth);
  CHECK (lowered.value ().restores == depth);
  REQUIRE ((copy.value ().verify ()).has_value ());
}
TEST_CASE ("cloned scopes and label relocations survive destruction of the source") {
  std::unique_ptr<Program> independent;
  {
    auto source = test::stack_scope_example (test::ScopeTransfer::Indirect);
    auto &fn = function (*source);
    auto label = fn.find_label ("inner").get ();
    REQUIRE (label);
    REQUIRE ((mirnext::ir::detail::access (*source->modules ().begin ())
                .create_data ("label-address",
                              mirnext::ir::detail::NodeAccess::make_LabelReference (label)))
               .has_value ());
    auto copied = clone (*source);
    REQUIRE ((copied).has_value ());
    independent = std::make_unique<Program> (std::move ((copied).value ()));
  }
  REQUIRE ((independent->verify ()).has_value ());
  auto lowered = materialize_stack_scopes (*independent);
  REQUIRE ((lowered).has_value ());
  REQUIRE ((independent->verify ()).has_value ());
  auto &fn = function (*independent);
  auto data = fn.module ().find_symbol ("label-address").get ();
  REQUIRE (data);
  auto reference = std::get_if<LabelReference> (&data->data ().get ()->initializer ());
  REQUIRE (reference);
  CHECK (&mirnext::ir::detail::NodeAccess::label (*(reference))->function () == &fn);
  CHECK (mirnext::ir::detail::NodeAccess::label (*(reference)) == fn.find_label ("inner").get ());
}
