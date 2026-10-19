/* Licensed under the MIT License. Stack scope lowering tests. */
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
Function &function (Context &context) {
  return *context.modules ().begin ()->functions ().begin ();
}
}  // namespace
TEST_CASE ("scope DSL returns load stack values before automatic restoration") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  auto entry = (*fn)->create_block ();
  REQUIRE (entry);
  IRBuilder builder (**entry);
  auto outer = builder.begin_stack_scope ("outer");
  REQUIRE (outer);
  auto inner = builder.begin_stack_scope ("inner");
  REQUIRE (inner);
  auto block = const_cast<BasicBlock *> (builder.insert_block ());
  auto slot = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE (slot);
  REQUIRE (builder.store (Memory{Type::i64 (), *slot}, Operand (std::int64_t{42})));
  auto returned = builder.ret (Operand (Memory{Type::i64 (), *slot}));
  REQUIRE (returned);
  REQUIRE (builder.end_stack_scope (**inner));
  REQUIRE (builder.end_stack_scope (**outer));
  CHECK (builder.insert_block () == nullptr);
  auto before = dump (context);
  auto copied = clone (context);
  REQUIRE (copied);
  CHECK (dump (**copied) == before);
  auto result = materialize_stack_scopes (context);
  REQUIRE (result);
  CHECK (result->saves == 2);
  CHECK (result->restores == 2);
  CHECK (opcodes (*block)
         == std::vector{Opcode::Alloca, Opcode::Mov, Opcode::Mov, Opcode::BEnd, Opcode::BEnd,
                        Opcode::Ret});
  CHECK ((*returned)->operands ()[0].get<const Register *> () != nullptr);
  CHECK (&(*returned)->block () == block);
  REQUIRE (context.verify ());
  CHECK_FALSE (needs_materialization (context));
  auto stable = dump (context);
  auto again = materialize_stack_scopes (context);
  REQUIRE (again);
  CHECK (again->saves == 0);
  CHECK (dump (context) == stable);
  auto cloned = clone (context);
  REQUIRE (cloned);
  CHECK (dump (**cloned) == stable);
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
    INFO ((text ? "" : text.error ().message));
    REQUIRE (text);
    auto bytes = serialization::write_binary (*context);
    INFO ((bytes ? "" : bytes.error ().message));
    REQUIRE (bytes);
    CHECK (dump (*context) == before);
    CHECK (fn.revisions () == revisions);
    CHECK (instruction == &*fn.blocks ().begin ()->instructions ().begin ());
    CHECK (needs_materialization (*context));
    auto lowered = materialize (*context);
    INFO ((lowered ? "" : lowered.error ().message));
    REQUIRE (lowered);
    REQUIRE ((*lowered)->verify ());
    CHECK_FALSE (needs_materialization (**lowered));
    auto decoded = serialization::read_binary (*bytes);
    REQUIRE (decoded);
    CHECK_FALSE (needs_materialization (**decoded));
    auto encoded = serialization::write_binary (**decoded);
    REQUIRE (encoded);
    CHECK (*encoded == *bytes);
    auto parsed = serialization::parse_text (*text);
    REQUIRE (parsed);
    auto formatted = serialization::write_text (**parsed);
    REQUIRE (formatted);
    CHECK (*formatted == *text);
  }
}
TEST_CASE (
  "same scope loop edges retain their identity and conditional exits restore only their edge") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Branch);
  auto &fn = function (*context);
  auto again = fn.find_label ("again"), inner = fn.find_label ("inner"),
       done = fn.find_label ("done");
  REQUIRE (again);
  REQUIRE (inner);
  REQUIRE (done);
  auto branch = &*std::prev (const_cast<BasicBlock &> (inner->block ()).instructions ().end ());
  auto loop = &*std::prev (const_cast<BasicBlock &> (again->block ()).instructions ().end ());
  auto result = materialize_stack_scopes (fn);
  REQUIRE (result);
  CHECK (*branch->operands ()[0].get<const Label *> () == again);
  CHECK (*loop->operands ()[0].get<const Label *> () == inner);
  auto inner_codes = opcodes (inner->block ());
  CHECK (std::count (inner_codes.begin (), inner_codes.end (), Opcode::BEnd) == 0);
  auto blocks = fn.blocks ();
  auto source = std::find_if (blocks.begin (), blocks.end (),
                              [&] (auto &b) { return &b == &inner->block (); });
  REQUIRE (source != blocks.end ());
  auto &cleanup = *std::next (source);
  CHECK (opcodes (cleanup) == std::vector{Opcode::BEnd, Opcode::Jmp});
  CHECK (*cleanup.terminator ()->operands ()[0].get<const Label *> () == done);
  REQUIRE (fn.verify ());
}
TEST_CASE ("switch exits share cleanup blocks and preserve repeated destinations") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Switch);
  auto &fn = function (*context);
  auto inner = fn.find_label ("inner"), done = fn.find_label ("done"),
       again = fn.find_label ("again");
  REQUIRE (inner);
  REQUIRE (done);
  REQUIRE (again);
  auto &instruction
    = *std::prev (const_cast<BasicBlock &> (inner->block ()).instructions ().end ());
  const Operand operands[]{instruction.operands ()[0], Operand (*done), Operand (*again),
                           Operand (*done)};
  REQUIRE (IRRewriter (fn).replace (instruction, Opcode::Switch, operands));
  auto result = materialize_stack_scopes (fn);
  REQUIRE (result);
  CHECK (instruction.operands ()[1] == instruction.operands ()[3]);
  CHECK (*instruction.operands ()[2].get<const Label *> () == again);
  auto cleanup = *instruction.operands ()[1].get<const Label *> ();
  CHECK (cleanup != done);
  CHECK (opcodes (cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::Jmp});
}
TEST_CASE ("scoped function entry is saved once even with a backedge to its first block") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (fn);
  auto scope = (*fn)->create_stack_scope ();
  REQUIRE (scope);
  auto entry = (*fn)->create_block ("entry", *scope);
  REQUIRE (entry);
  auto exit = (*fn)->create_block ("exit", *scope);
  REQUIRE (exit);
  auto label = (*fn)->create_label (**entry);
  REQUIRE (label);
  IRBuilder builder (**entry);
  REQUIRE (builder.allocate_stack (Operand (std::int64_t{16})));
  REQUIRE (builder.branch_if (Operand (*(*fn)->arguments ()[0]), **label));
  REQUIRE (IRBuilder (**exit).ret (Operand (std::int64_t{42})));
  auto result = materialize_stack_scopes (**fn);
  REQUIRE (result);
  CHECK (result->saves == 1);
  CHECK (result->restores == 1);
  CHECK (opcodes (*(*fn)->blocks ().begin ()) == std::vector{Opcode::BStart, Opcode::Jmp});
  CHECK (opcodes (**entry) == std::vector{Opcode::Alloca, Opcode::Bt});
}
TEST_CASE ("mixed indirect targets evaluate memory once and leave stack cleanup after dispatch") {
  auto context = test::stack_scope_example (test::ScopeTransfer::Indirect);
  auto &fn = function (*context);
  auto dispatch = fn.find_label ("dispatch");
  REQUIRE (dispatch);
  auto source = &dispatch->block ();
  auto result = materialize_stack_scopes (fn);
  REQUIRE (result);
  CHECK (result->indirect_dispatches == 1);
  auto codes = opcodes (*source);
  CHECK (codes == std::vector{Opcode::Mov, Opcode::LAddr, Opcode::Beq});
  auto &first = *source->instructions ().begin ();
  CHECK (first.operands ()[1].get<Memory> () != nullptr);
  auto destination = *first.operands ()[0].get<const Register *> ();
  std::size_t reads = 0;
  for (auto &block : fn.blocks ())
    for (auto &i : block.instructions ())
      for (auto &operand : i.operands ())
        if (auto reg = operand.get<const Register *> (); reg && *reg == destination) ++reads;
  CHECK (reads > 2);
  REQUIRE (fn.verify ());
}
TEST_CASE ("same scope indirect targets still retain cleanup for an unknown destination") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  auto scope = (*fn)->create_stack_scope ();
  REQUIRE (scope);
  auto entry = (*fn)->create_block ({}, *scope);
  REQUIRE (entry);
  auto target = (*fn)->create_block ({}, *scope);
  REQUIRE (target);
  auto label = (*fn)->create_label (**target);
  REQUIRE (label);
  IRBuilder builder (**entry);
  auto address = builder.address_of (**label);
  REQUIRE (address);
  REQUIRE (builder.emit (Opcode::JmpIndirect, {Operand (**address)}));
  REQUIRE (IRBuilder (**target).ret (Operand (std::int64_t{42})));
  auto lowered = materialize_stack_scopes (**fn);
  REQUIRE (lowered);
  CHECK (lowered->indirect_dispatches == 1);
  CHECK ((*entry)->terminator ()->opcode () == Opcode::Beq);
  auto blocks = (*fn)->blocks ();
  auto source
    = std::find_if (blocks.begin (), blocks.end (), [&] (auto &b) { return &b == *entry; });
  REQUIRE (source != blocks.end ());
  CHECK (opcodes (*std::next (source)) == std::vector{Opcode::BEnd, Opcode::JmpIndirect});
  CHECK (*(*entry)->terminator ()->operands ()[0].get<const Label *> () == *label);
}
TEST_CASE ("scope errors retain first builder diagnostic and rejected edits are atomic") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {});
  REQUIRE (fn);
  auto entry = (*fn)->create_block ();
  REQUIRE (entry);
  IRBuilder builder (**entry);
  auto a = builder.begin_stack_scope ("a");
  REQUIRE (a);
  auto b = builder.begin_stack_scope ("b");
  REQUIRE (b);
  auto before = (*fn)->instruction_count ();
  CHECK_FALSE (builder.end_stack_scope (**a));
  auto first = (*module)->error ();
  REQUIRE (first);
  CHECK_FALSE (builder.ret ());
  CHECK ((*module)->error ()->message == first->message);
  CHECK ((*fn)->instruction_count () == before);
  Context other;
  auto om = other.create_module ("other");
  REQUIRE (om);
  auto of = (*om)->create_function ("g", {});
  REQUIRE (of);
  auto os = (*of)->create_stack_scope ();
  REQUIRE (os);
  auto block = (*of)->create_block ();
  REQUIRE (block);
  auto revision = (*of)->revisions ();
  CHECK_FALSE (IRRewriter (**of).set_stack_scope (**block, *a));
  CHECK ((*of)->revisions () == revision);
  CHECK_FALSE ((*of)->create_block ("foreign", *a));
}
TEST_CASE ("sibling transitions restore then save and homogeneous indirect exits avoid dispatch") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  auto outer = (*fn)->create_stack_scope ();
  REQUIRE (outer);
  auto a = (*fn)->create_stack_scope (*outer);
  REQUIRE (a);
  auto b = (*fn)->create_stack_scope (*outer);
  REQUIRE (b);
  auto entry = (*fn)->create_block ("entry", *a);
  REQUIRE (entry);
  auto sibling = (*fn)->create_block ("sibling", *b);
  REQUIRE (sibling);
  auto exit = (*fn)->create_block ("exit");
  REQUIRE (exit);
  auto exit2 = (*fn)->create_block ("exit2");
  REQUIRE (exit2);
  auto sibling_label = (*fn)->create_label (**sibling);
  REQUIRE (sibling_label);
  auto exit_label = (*fn)->create_label (**exit);
  REQUIRE (exit_label);
  auto exit2_label = (*fn)->create_label (**exit2);
  REQUIRE (exit2_label);
  REQUIRE (IRBuilder (**entry).jump (**sibling_label));
  IRBuilder builder (**sibling);
  auto destination = builder.address_of (**exit_label);
  REQUIRE (destination);
  auto jumped = builder.emit (Opcode::JmpIndirect, {Operand (**destination)});
  REQUIRE (jumped);
  REQUIRE (IRBuilder (**exit).ret (Operand (std::int64_t{42})));
  REQUIRE (IRBuilder (**exit2).ret (Operand (std::int64_t{42})));
  auto result = materialize_stack_scopes (**fn);
  REQUIRE (result);
  auto entry_jump = (*entry)->terminator ();
  REQUIRE (entry_jump);
  auto cleanup = *entry_jump->operands ()[0].get<const Label *> ();
  CHECK (opcodes (cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::BStart, Opcode::Jmp});
  CHECK (result->indirect_dispatches == 1);  // The sibling label is also a valid in-scope target.

  Context homogeneous;
  auto hm = homogeneous.create_module ("m");
  REQUIRE (hm);
  auto hf = (*hm)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (hf);
  auto scope = (*hf)->create_stack_scope ();
  REQUIRE (scope);
  auto source = (*hf)->create_block ({}, *scope);
  REQUIRE (source);
  auto target = (*hf)->create_block ();
  REQUIRE (target);
  auto label = (*hf)->create_label (**target);
  REQUIRE (label);
  IRBuilder source_builder (**source);
  auto slot = source_builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE (slot);
  auto address = source_builder.address_of (**label);
  REQUIRE (address);
  REQUIRE (source_builder.store (Memory{Type::pointer (), *slot}, Operand (**address)));
  auto transfer
    = source_builder.emit (Opcode::JmpIndirect, {Operand (Memory{Type::pointer (), *slot})});
  REQUIRE (transfer);
  REQUIRE (IRBuilder (**target).ret (Operand (std::int64_t{42})));
  auto lowered = materialize_stack_scopes (**hf);
  REQUIRE (lowered);
  CHECK (lowered->indirect_dispatches == 0);
  CHECK (opcodes (**source)
         == std::vector{Opcode::Alloca, Opcode::LAddr, Opcode::Mov, Opcode::Mov, Opcode::BEnd,
                        Opcode::JmpIndirect});
  CHECK ((*transfer)->opcode () == Opcode::JmpIndirect);
  CHECK ((*transfer)->operands ()[0].get<const Register *> () != nullptr);
}
TEST_CASE (
  "value DSL chooses precise address opcodes and rejects invalid values before publishing "
  "registers") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  auto entry = (*fn)->create_block ();
  REQUIRE (entry);
  IRBuilder builder (**entry);
  auto data = (*module)->create_data ("global",
                                      InitializedData{Type::i64 (), {Operand (std::int64_t{42})}});
  REQUIRE (data);
  for (auto [type, opcode] :
       {std::pair{Type (Type::Kind::U8), Opcode::Addr8},
        std::pair{Type (Type::Kind::I16), Opcode::Addr16},
        std::pair{Type (Type::Kind::U32), Opcode::Addr32}, std::pair{Type::i64 (), Opcode::Addr}}) {
    auto variable = (*fn)->create_register (type);
    REQUIRE (variable);
    auto address = builder.address_of (**variable);
    REQUIRE (address);
    CHECK (std::prev ((*entry)->instructions ().end ())->opcode () == opcode);
    CHECK ((*address)->type () == Type::pointer ());
  }
  auto address = builder.address_of ((*data)->symbol (), 8);
  REQUIRE (address);
  CHECK (std::prev ((*entry)->instructions ().end ())->opcode () == Opcode::Mov);
  auto converted = builder.unary (Opcode::I2D, Operand (std::int64_t{42}));
  REQUIRE (converted);
  CHECK ((*converted)->type () == Type (Type::Kind::F64));
  auto slot = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE (slot);
  REQUIRE (builder.store (Memory{Type (Type::Kind::F64), *slot}, Operand (**converted)));
  auto loaded = builder.load (Memory{Type (Type::Kind::F64), *slot});
  REQUIRE (loaded);
  CHECK (std::prev ((*entry)->instructions ().end ())->opcode () == Opcode::DMov);
  REQUIRE (builder.ret (Operand (std::int64_t{42})));
  REQUIRE (context.verify ());
  auto bad = (*module)->create_function ("bad", {});
  REQUIRE (bad);
  auto bad_entry = (*bad)->create_block ();
  REQUIRE (bad_entry);
  IRBuilder invalid (**bad_entry);
  CHECK_FALSE (invalid.allocate_stack (Operand (1.0)));
  CHECK ((*bad)->registers ().begin () == (*bad)->registers ().end ());
  CHECK ((*bad)->instruction_count () == 0);
}
TEST_CASE (
  "explicit stack operations retain order and overflow branches clean up after testing flags") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  auto entry = (*fn)->create_block ();
  REQUIRE (entry);
  IRBuilder builder (**entry);
  auto scope = builder.begin_stack_scope ();
  REQUIRE (scope);
  auto explicit_save = builder.stack_save ("explicit");
  REQUIRE (explicit_save);
  auto restored = builder.stack_restore (**explicit_save);
  REQUIRE (restored);
  auto value = builder.binary (Opcode::Addo, Operand (INT64_MAX), Operand (std::int64_t{1}));
  REQUIRE (value);
  auto fallback = (*fn)->create_block ("fallback");
  REQUIRE (fallback);
  auto target = (*fn)->create_block ("target");
  REQUIRE (target);
  auto label = (*fn)->create_label (**target);
  REQUIRE (label);
  auto branch = builder.emit (Opcode::Bo, {Operand (**label)});
  REQUIRE (branch);
  REQUIRE (IRBuilder (**fallback).ret (Operand (std::int64_t{0})));
  REQUIRE (IRBuilder (**target).ret (Operand (std::int64_t{42})));
  auto lowered = materialize_stack_scopes (**fn);
  REQUIRE (lowered);
  auto body = &(*branch)->block ();
  CHECK (opcodes (*body) == std::vector{Opcode::BStart, Opcode::BEnd, Opcode::Addo, Opcode::Bo});
  CHECK ((*restored)->operands ()[0] == Operand (**explicit_save));
  REQUIRE ((*fn)->verify ());
  auto target_cleanup = *(*branch)->operands ()[0].get<const Label *> ();
  CHECK (opcodes (target_cleanup->block ()) == std::vector{Opcode::BEnd, Opcode::Jmp});

  Context invalid;
  auto im = invalid.create_module ("invalid");
  REQUIRE (im);
  auto iff = (*im)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (iff);
  auto ib = (*iff)->create_block ();
  REQUIRE (ib);
  IRBuilder bad (**ib);
  auto is = bad.begin_stack_scope ();
  REQUIRE (is);
  REQUIRE (bad.binary (Opcode::Addo, Operand (INT64_MAX), Operand (std::int64_t{1})));
  REQUIRE (bad.end_stack_scope (**is));
  auto ir = (*iff)->create_block ();
  REQUIRE (ir);
  auto il = (*iff)->create_label (**ir);
  REQUIRE (il);
  REQUIRE (bad.emit (Opcode::Bo, {Operand (**il)}));
  REQUIRE (IRBuilder (**ir).ret (Operand (std::int64_t{42})));
  auto snapshot = dump (invalid);
  CHECK_FALSE (materialize_stack_scopes (invalid));
  CHECK (dump (invalid) == snapshot);
}
TEST_CASE ("scope lowering and cloning are iterative for deeply nested block scopes") {
  Context context;
  auto module = context.create_module ("m");
  REQUIRE (module);
  auto fn = (*module)->create_function ("f", {{Type::i64 ()}, {}});
  REQUIRE (fn);
  const StackScope *parent = nullptr;
  constexpr std::size_t depth = 4096;
  for (std::size_t n = 0; n < depth; ++n) {
    auto scope = (*fn)->create_stack_scope (parent);
    REQUIRE (scope);
    parent = *scope;
    auto block = (*fn)->create_block ({}, parent);
    REQUIRE (block);
  }
  REQUIRE (IRBuilder (*std::prev ((*fn)->blocks ().end ())).ret (Operand (std::int64_t{42})));
  auto copy = clone (context);
  REQUIRE (copy);
  auto lowered = materialize_stack_scopes (**copy);
  REQUIRE (lowered);
  CHECK (lowered->saves == depth);
  CHECK (lowered->restores == depth);
  REQUIRE ((*copy)->verify ());
}
TEST_CASE ("cloned scopes and label relocations survive destruction of the source") {
  std::unique_ptr<Context> independent;
  {
    auto source = test::stack_scope_example (test::ScopeTransfer::Indirect);
    auto &fn = function (*source);
    auto label = fn.find_label ("inner");
    REQUIRE (label);
    REQUIRE (source->modules ().begin ()->create_data ("label-address", LabelReference{label}));
    auto copied = clone (*source);
    REQUIRE (copied);
    independent = std::move (*copied);
  }
  REQUIRE (independent->verify ());
  auto lowered = materialize_stack_scopes (*independent);
  REQUIRE (lowered);
  REQUIRE (independent->verify ());
  auto &fn = function (*independent);
  auto data = fn.module ().find_symbol ("label-address");
  REQUIRE (data);
  auto reference = std::get_if<LabelReference> (&data->data ()->initializer ());
  REQUIRE (reference);
  CHECK (&reference->label->function () == &fn);
  CHECK (reference->label == fn.find_label ("inner"));
}
