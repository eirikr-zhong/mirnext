/* Licensed under the MIT License. Register IR construction and validation tests. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/IR/IR.hpp"
#include "PublicInstructions.hpp"
#include <array>
#include <type_traits>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *entry;
  Example () {
    auto m = mirnext::ir::detail::access (context).create_module ("example");
    REQUIRE ((m).has_value ());
    module = &(m).value ();
    auto f
      = mirnext::ir::detail::access (*(module)).create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    function = &(f).value ();
    auto b = mirnext::ir::detail::access (*(function)).create_block ("entry");
    REQUIRE ((b).has_value ());
    entry = &(b).value ();
  }
};
Operand integer (std::int64_t value) { return Operand (value); }
}  // namespace
TEST_CASE ("native expression data rejects memory and call effects including later rewrites") {
  Example e;
  auto value = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((value).has_value ());
  InstructionBuilder builder (*e.entry);
  SUBCASE ("memory") {
    REQUIRE (
      (builder.assign ((value).value (),
                       Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ()))))
        .has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
    auto count = e.module->items ().size ();
    auto failed
      = mirnext::ir::detail::access (*(e.module))
          .create_data ("expression",
                        mirnext::ir::detail::NodeAccess::make_ExpressionData (e.function));
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("must not access memory") != std::string::npos);
    CHECK (e.module->items ().size () == count);
    CHECK (e.module->find_symbol ("expression").get () == nullptr);
  }
  SUBCASE ("call") {
    auto imported
      = mirnext::ir::detail::access (*(e.module)).import ("foreign", e.function->signature ());
    REQUIRE ((imported).has_value ());
    REQUIRE (
      (builder.emit (Opcode::Call, {Operand ((imported).value ()), Operand ((imported).value ()),
                                    Operand ((value).value ())}))
        .has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
    auto failed
      = mirnext::ir::detail::access (*(e.module))
          .create_data ("expression",
                        mirnext::ir::detail::NodeAccess::make_ExpressionData (e.function));
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("must not call functions") != std::string::npos);
  }
  SUBCASE ("mutation after data creation") {
    auto assignment = builder.assign ((value).value (), integer (42));
    REQUIRE ((assignment).has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
    REQUIRE ((mirnext::ir::detail::access (*(e.module))
                .create_data ("expression",
                              mirnext::ir::detail::NodeAccess::make_ExpressionData (e.function)))
               .has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    REQUIRE (
      (InstructionRewriter (*e.function)
         .replace_operand ((assignment).value (), 1,
                           Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ()))))
        .has_value ());
    auto failed = e.context.verify ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("must not access memory") != std::string::npos);
  }
}
TEST_CASE ("native context validates exported names imported signatures and untyped calls") {
  Example e;
  InstructionBuilder exported (*e.entry);
  REQUIRE ((exported.ret (integer (42))).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (*(e.module)).export_symbol (e.function->symbol ())).has_value ());
  auto other = mirnext::ir::detail::access (e.context).create_module ("consumer");
  REQUIRE ((other).has_value ());
  SUBCASE ("matching import and independent clone") {
    auto imported
      = mirnext::ir::detail::access (((other)).value ()).import ("entry", e.function->signature ());
    REQUIRE ((imported).has_value ());
    auto function = mirnext::ir::detail::access (((other)).value ())
                      .create_function ("caller", e.function->signature ());
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    auto result
      = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
    REQUIRE ((result).has_value ());
    InstructionBuilder builder ((block).value ());
    REQUIRE (
      (builder.emit (Opcode::Call, {Operand ((imported).value ()), Operand ((imported).value ()),
                                    Operand ((result).value ())}))
        .has_value ());
    REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    auto copied = clone (e.context);
    REQUIRE ((copied).has_value ());
    CHECK (dump ((copied).value ()) == dump (e.context));
  }
  SUBCASE ("duplicate export") {
    auto function = mirnext::ir::detail::access (((other)).value ())
                      .create_function ("entry", e.function->signature ());
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    InstructionBuilder builder ((block).value ());
    REQUIRE ((builder.ret (integer (0))).has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (((other)).value ()).export_symbol (function.value ().symbol ()))
        .has_value ());
    auto failed = e.context.verify ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().code == ErrorCode::DuplicateName);
  }
  SUBCASE ("import mismatch") {
    REQUIRE ((mirnext::ir::detail::access (((other)).value ())
                .import ("entry", Signature{{Type (Type::Kind::F64)}, {}}))
               .has_value ());
    auto failed = e.context.verify ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("consumer/entry: import signature") != std::string::npos);
  }
  SUBCASE ("untyped import call mismatch") {
    auto imported = mirnext::ir::detail::access (((other)).value ()).import ("entry");
    REQUIRE ((imported).has_value ());
    Signature signature{{Type (Type::Kind::F64)}, {}};
    auto prototype
      = mirnext::ir::detail::access (((other)).value ()).prototype ("call_type", signature);
    REQUIRE ((prototype).has_value ());
    auto function = mirnext::ir::detail::access (((other)).value ())
                      .create_function ("caller", e.function->signature ());
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    auto result = mirnext::ir::detail::access (((function)).value ())
                    .create_register (Type (Type::Kind::F64));
    REQUIRE ((result).has_value ());
    InstructionBuilder builder ((block).value ());
    REQUIRE (
      (builder.emit (Opcode::Call, {Operand ((prototype).value ()), Operand ((imported).value ()),
                                    Operand ((result).value ())}))
        .has_value ());
    REQUIRE ((builder.ret (integer (0))).has_value ());
    REQUIRE ((other.value ().verify ()).has_value ());
    auto failed = e.context.verify ();
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("consumer/caller: cross-module call signature")
           != std::string::npos);
  }
  SUBCASE ("external host import has no required in-context definition") {
    REQUIRE (
      (mirnext::ir::detail::access (((other)).value ()).import ("host", e.function->signature ()))
        .has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native verifier enforces direct and cross module jump calling conventions") {
  for (bool cross_module : {false, true})
    for (bool jump_return : {false, true})
      for (bool jump_call : {false, true}) {
        CAPTURE (cross_module);
        CAPTURE (jump_return);
        CAPTURE (jump_call);
        Program context;
        auto module = mirnext::ir::detail::access (context).create_module ("target");
        REQUIRE ((module).has_value ());
        auto callee
          = mirnext::ir::detail::access (((module)).value ()).create_function ("callee", {});
        REQUIRE ((callee).has_value ());
        auto body = mirnext::ir::detail::access (((callee)).value ()).create_block ();
        REQUIRE ((body).has_value ());
        InstructionBuilder target ((body).value ());
        if (jump_return)
          REQUIRE ((target.emit (Opcode::JRet, {integer (0)})).has_value ());
        else
          REQUIRE ((target.ret ()).has_value ());
        auto caller_module = &(module).value ();
        const Symbol *symbol = &callee.value ().symbol ();
        if (cross_module) {
          REQUIRE ((mirnext::ir::detail::access (((module)).value ()).export_symbol (*symbol))
                     .has_value ());
          auto other = mirnext::ir::detail::access (context).create_module ("caller");
          REQUIRE ((other).has_value ());
          caller_module = &(other).value ();
          auto imported
            = mirnext::ir::detail::access (*(caller_module)).import ("callee", Signature{});
          REQUIRE ((imported).has_value ());
          symbol = &(imported).value ();
        }
        auto caller = mirnext::ir::detail::access (*(caller_module)).create_function ("caller", {});
        REQUIRE ((caller).has_value ());
        auto block = mirnext::ir::detail::access (((caller)).value ()).create_block ();
        REQUIRE ((block).has_value ());
        InstructionBuilder builder ((block).value ());
        REQUIRE ((builder.emit (jump_call ? Opcode::JCall : Opcode::Call,
                                {Operand (*symbol), Operand (*symbol)}))
                   .has_value ());
        if (!jump_call) REQUIRE ((builder.ret ()).has_value ());
        auto checked = context.verify ();
        CHECK (static_cast<bool> ((checked).has_value ()) == (jump_call == jump_return));
        if (!(checked).has_value ())
          CHECK (checked.error ().message.find ("call convention") != std::string::npos);
      }
}
TEST_CASE ("native IR has stable basic blocks registers labels and instruction identities") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto a = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "a");
  REQUIRE ((a).has_value ());
  auto first = builder.assign ((a).value (), integer (40));
  REQUIRE ((first).has_value ());
  auto result = builder.binary (Opcode::Add, Operand ((a).value ()), integer (2), "result");
  REQUIRE ((result).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto identity = &(first).value ();
  for (unsigned n = 0; n < 2000; ++n)
    REQUIRE (
      (mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ())).has_value ());
  CHECK (&identity->function () == e.function);
  CHECK (identity->opcode () == Opcode::Mov);
  CHECK (e.function->find_register ("a").get () == &(a).value ());
  auto text = dump (e.context);
  CHECK (text.find ("add") != text.npos);
  CHECK (text.find ("ret") != text.npos);
  auto revisions = e.function->revisions ();
  auto count = e.function->instruction_count ();
  for (unsigned n = 0; n < 4; ++n) {
    CHECK (dump (e.context) == text);
    CHECK ((e.context.verify ()).has_value ());
  }
  CHECK (e.function->revisions () == revisions);
  CHECK (e.function->instruction_count () == count);
  static_assert (
    std::is_same_v<decltype (*std::as_const (*e.function).blocks ().begin ()), const BasicBlock &>);
  static_assert (std::is_same_v<decltype (*std::as_const (*e.entry).instructions ().begin ()),
                                const Instruction &>);
}
TEST_CASE ("native forward declaration and definition share a symbol identity") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((module).has_value ());
  auto forward = mirnext::ir::detail::access (((module)).value ()).forward ("entry");
  REQUIRE ((forward).has_value ());
  auto declaration = mirnext::ir::detail::access (((module)).value ())
                       .declare_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((declaration).has_value ());
  CHECK (&(forward).value () == &(declaration).value ());
  CHECK_FALSE ((context.verify ()).has_value ());
  auto function
    = mirnext::ir::detail::access (((module)).value ()).define_function ((declaration).value ());
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  REQUIRE ((builder.ret (integer (42))).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((module)).value ()).export_symbol ((forward).value ()))
             .has_value ());
  REQUIRE ((context.verify ()).has_value ());
  CHECK (module.value ().find_symbol ("entry").get () == &(forward).value ());
  CHECK (forward.value ().function ().get () == &(function).value ());
  CHECK (module.value ().items ().size () == 4);
}
TEST_CASE (
  "native hardware bindings preserve one identity and reject incompatible scalar classes") {
  Example e;
  auto first
    = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "a", "r5");
  REQUIRE ((first).has_value ());
  auto second = mirnext::ir::detail::access (*(e.function))
                  .create_register (Type (Type::Kind::U32), "b", "r5");
  REQUIRE ((second).has_value ());
  CHECK (&(first).value () == &(second).value ());
  CHECK (e.function->find_register ("b").get () == &(first).value ());
  CHECK_FALSE ((mirnext::ir::detail::access (*(e.function))
                  .create_register (Type (Type::Kind::F64), "wrong", "r5"))
                 .has_value ());
  CHECK_FALSE (
    (mirnext::ir::detail::access (*(e.function)).create_register (Type::block (16), "block"))
      .has_value ());
  InstructionBuilder builder (*e.entry);
  REQUIRE ((builder.assign ((first).value (), integer (42))).has_value ());
  REQUIRE ((builder.assign ((second).value (), integer (0))).has_value ());
  REQUIRE ((builder.ret (Operand ((first).value ()))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto copied = clone (e.context);
  REQUIRE ((copied).has_value ());
  REQUIRE ((copied.value ().verify ()).has_value ());
  auto &module = *copied.value ().modules ().begin ();
  auto &function = *module.functions ().begin ();
  CHECK (function.find_register ("a").get () == function.find_register ("b").get ());
  CHECK (function.find_register ("a").get () != &(first).value ());
}
TEST_CASE ("hardware addresses require an explicit ordinary register copy") {
  for (auto opcode : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    CAPTURE (opcode);
    Example e;
    auto bound
      = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "bound", "r5");
    REQUIRE ((bound).has_value ());
    auto local = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "copy");
    REQUIRE ((local).has_value ());
    auto pointer
      = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer (), "pointer");
    REQUIRE ((pointer).has_value ());
    InstructionBuilder builder (*e.entry);
    REQUIRE ((builder.assign ((local).value (), Operand ((bound).value ()))).has_value ());
    auto address
      = builder.emit (opcode, {Operand ((pointer).value ()), Operand ((local).value ())});
    REQUIRE ((address).has_value ());
    REQUIRE ((builder.ret (Operand ((local).value ()))).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    auto before = dump (e.context);
    auto revision = e.function->revisions ();
    InstructionRewriter rewriter (*e.function);
    auto failed = rewriter.replace_operand ((address).value (), 1, Operand ((bound).value ()));
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (failed.error ().message.find ("copy it to an ordinary local") != std::string::npos);
    CHECK_FALSE ((rewriter.materialize_register_storage ((bound).value ())).has_value ());
    CHECK_FALSE (bound.value ().is_memory_backed ());
    CHECK (e.function->revisions () == revision);
    CHECK (dump (e.context) == before);
    REQUIRE ((e.context.verify ()).has_value ());
    // A fresh builder checks invalid emit before publishing any instruction.
    Example invalid;
    auto r = mirnext::ir::detail::access (*(invalid.function))
               .create_register (Type::i64 (), "bound", "r5");
    REQUIRE ((r).has_value ());
    auto p = mirnext::ir::detail::access (*(invalid.function)).create_register (Type::pointer ());
    REQUIRE ((p).has_value ());
    InstructionBuilder bad (*invalid.entry);
    auto revisions = invalid.function->revisions ();
    CHECK_FALSE (
      (bad.emit (opcode, {Operand ((p).value ()), Operand ((r).value ())})).has_value ());
    CHECK (invalid.function->instruction_count () == 0);
    CHECK (invalid.function->revisions () == revisions);
  }
  Example e;
  auto bound
    = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "bound", "r5");
  REQUIRE ((bound).has_value ());
  InstructionBuilder builder (*e.entry);
  auto before = e.function->revisions ();
  auto count = std::distance (e.function->registers ().begin (), e.function->registers ().end ());
  CHECK_FALSE ((builder.address_of ((bound).value ())).has_value ());
  CHECK (e.function->revisions () == before);
  CHECK (std::distance (e.function->registers ().begin (), e.function->registers ().end ())
         == count);
  CHECK (e.function->instruction_count () == 0);
}
TEST_CASE ("native builder reports the first error and does not publish failed operations") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto before = e.function->revisions ();
  auto failed = builder.binary (Opcode::Add, Operand (1.0), integer (2));
  CHECK_FALSE ((failed).has_value ());
  CHECK (e.function->instruction_count () == 0);
  CHECK (e.function->revisions () == before);
  auto repeated = builder.ret (integer (42));
  CHECK_FALSE ((repeated).has_value ());
  REQUIRE ((e.module->error ()).has_value ());
  CHECK (repeated.error ().message == failed.error ().message);
  CHECK_FALSE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native rewriter edits are atomic and moving instructions preserves identity") {
  Example e;
  auto exit = mirnext::ir::detail::access (*(e.function)).create_block ("exit");
  REQUIRE ((exit).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value (), "exit");
  REQUIRE ((label).has_value ());
  auto alias = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value (), "alias");
  REQUIRE ((alias).has_value ());
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "a");
  REQUIRE ((reg).has_value ());
  InstructionBuilder builder (*e.entry);
  auto instruction = builder.assign ((reg).value (), integer (40));
  REQUIRE ((instruction).has_value ());
  auto branch = builder.jump ((label).value ());
  REQUIRE ((branch).has_value ());
  InstructionBuilder ending ((exit).value ());
  auto ret = ending.ret (Operand ((reg).value ()));
  REQUIRE ((ret).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  InstructionRewriter rewriter (*e.function);
  auto revisions = e.function->revisions ();
  CHECK_FALSE ((rewriter.replace_operand ((instruction).value (), 1, Operand (1.0))).has_value ());
  CHECK (e.function->revisions () == revisions);
  REQUIRE ((rewriter.replace_operand ((instruction).value (), 1, integer (42))).has_value ());
  CHECK (e.function->revisions ().control_flow == revisions.control_flow);
  auto address = &(instruction).value ();
  REQUIRE (
    (rewriter.move_before ((instruction).value (), (exit).value (), &(ret).value ())).has_value ());
  CHECK (&address->block () == &(exit).value ());
  CHECK (e.entry->size () == 1);
  CHECK (exit.value ().size () == 2);
  CHECK (exit.value ().labels ().size () == 2);
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native cloning remaps symbols labels data and references before source destruction") {
  std::unique_ptr<Program> copied;
  std::string expected;
  {
    Example e;
    auto label = mirnext::ir::detail::access (*(e.function)).create_label (*e.entry, "entry");
    REQUIRE ((label).has_value ());
    InstructionBuilder builder (*e.entry);
    REQUIRE ((builder.ret (integer (42))).has_value ());
    auto data = mirnext::ir::detail::access (*(e.module))
                  .create_data ("value", InitializedData{Type::i64 (), {integer (42)}}, false);
    REQUIRE ((data).has_value ());
    REQUIRE ((mirnext::ir::detail::access (*(e.module))
                .create_data ("pointer",
                              mirnext::ir::detail::NodeAccess::make_SymbolReference (&data.value ()
                                                                                        .symbol (),
                                                                                     8)))
               .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (*(e.module))
         .create_data ("labels",
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                             &(label).value (), 4)))
        .has_value ());
    REQUIRE ((mirnext::ir::detail::access (*(e.module))
                .create_data ("expression",
                              mirnext::ir::detail::NodeAccess::make_ExpressionData (e.function)))
               .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (*(e.module)).create_data ("zero", ZeroData{32})).has_value ());
    REQUIRE ((mirnext::ir::detail::access (*(e.module)).export_symbol (e.function->symbol ()))
               .has_value ());
    REQUIRE ((mirnext::ir::detail::access (*(e.module))
                .create_data ("", InitializedData{Type::i64 (), {integer (17)}}))
               .has_value ());
    REQUIRE (
      (mirnext::ir::detail::access (*(e.module)).create_data ("", ZeroData{8})).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    expected = dump (e.context);
    auto clone_result = clone (e.context);
    REQUIRE ((clone_result).has_value ());
    copied = std::make_unique<Program> (std::move ((clone_result).value ()));
  }
  REQUIRE ((copied->verify ()).has_value ());
  CHECK (dump (*copied) == expected);
}
TEST_CASE ("native IR constructs and dumps every public instruction through checked emission") {
  std::size_t covered = 0;
  for (auto &descriptor : instruction_descriptors ()) {
    CAPTURE (descriptor.name);
    auto context = mirnext::test::public_instruction_example (descriptor);
    auto output = dump (*context);
    if (descriptor.opcode != Opcode::Label)
      CHECK (output.find (descriptor.name) != std::string::npos);
    auto copied = clone (*context);
    REQUIRE ((copied).has_value ());
    CHECK (dump ((copied).value ()) == output);
    ++covered;
  }
  CHECK (covered == instruction_descriptors ().size ());
}
TEST_CASE ("native IR checks block ABI sizes classes and variadic promotions before insertion") {
  for (unsigned classification = 0; classification < 5; ++classification) {
    Example e;
    auto type = Type::block (16, static_cast<Type::BlockClass> (classification));
    Signature signature{{Type::i64 ()}, {type}, true};
    auto prototype = mirnext::ir::detail::access (*(e.module)).prototype ("signature", signature);
    REQUIRE ((prototype).has_value ());
    auto imported = mirnext::ir::detail::access (*(e.module)).import ("callee", signature);
    REQUIRE ((imported).has_value ());
    auto result = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
    REQUIRE ((result).has_value ());
    auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
    REQUIRE ((address).has_value ());
    Memory block = mirnext::ir::detail::NodeAccess::make_Memory (type, &(address).value ());
    std::vector<Operand> operands{Operand ((prototype).value ()), Operand ((imported).value ()),
                                  Operand ((result).value ()), Operand (block), Operand (1.0)};
    REQUIRE ((validate_instruction (*e.function, Opcode::Call, operands)).has_value ());
    auto bad = operands;
    auto memory = block;
    memory.type = Type::block (8, static_cast<Type::BlockClass> (classification));
    bad[3] = Operand (memory);
    CHECK_FALSE ((validate_instruction (*e.function, Opcode::Call, bad)).has_value ());
    bad = operands;
    memory.type = Type::block (16, static_cast<Type::BlockClass> ((classification + 1) % 5));
    bad[3] = Operand (memory);
    CHECK_FALSE ((validate_instruction (*e.function, Opcode::Call, bad)).has_value ());
    bad = operands;
    bad[4] = Operand (1.0f);
    CHECK_FALSE ((validate_instruction (*e.function, Opcode::Call, bad)).has_value ());
    bad = operands;
    bad.pop_back ();
    bad.pop_back ();
    CHECK_FALSE ((validate_instruction (*e.function, Opcode::Call, bad)).has_value ());
    InstructionBuilder builder (*e.entry);
    REQUIRE ((builder.emit (Opcode::Call, operands)).has_value ());
    REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native overflow flags follow producer and register move rules") {
  for (auto producer : {Opcode::Addo, Opcode::Addos, Opcode::Subo, Opcode::Subos, Opcode::Mulo,
                        Opcode::Mulos, Opcode::UMulo, Opcode::UMulos}) {
    for (auto branch : {Opcode::Bo, Opcode::Bno, Opcode::UBo, Opcode::UBno}) {
      CAPTURE (producer);
      CAPTURE (branch);
      Example e;
      auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
      REQUIRE ((reg).has_value ());
      auto exit = mirnext::ir::detail::access (*(e.function)).create_block ();
      REQUIRE ((exit).has_value ());
      auto label = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value ());
      REQUIRE ((label).has_value ());
      InstructionBuilder builder (*e.entry);
      REQUIRE ((builder.emit (producer, {Operand ((reg).value ()), integer (1), integer (2)}))
                 .has_value ());
      REQUIRE ((builder.assign ((reg).value (), Operand ((reg).value ()))).has_value ());
      REQUIRE (
        (builder.emit (Opcode::Mov,
                       {Operand (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                               &(reg).value ())),
                        Operand ((reg).value ())}))
          .has_value ());
      REQUIRE ((builder.emit (branch, {Operand ((label).value ())})).has_value ());
      REQUIRE ((InstructionBuilder ((exit).value ()).ret (integer (42))).has_value ());
      bool unsigned_branch = branch == Opcode::UBo || branch == Opcode::UBno;
      bool invalid_signedness
        = (unsigned_branch && (producer == Opcode::Mulo || producer == Opcode::Mulos))
          || (!unsigned_branch && (producer == Opcode::UMulo || producer == Opcode::UMulos));
      CHECK (e.function->verify ().has_value () == !invalid_signedness);
    }
  }
  for (unsigned separator = 0; separator < 5; ++separator) {
    Example e;
    auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
    REQUIRE ((reg).has_value ());
    InstructionBuilder builder (*e.entry);
    REQUIRE ((builder.emit (Opcode::Addo, {Operand ((reg).value ()), integer (1), integer (2)}))
               .has_value ());
    if (separator <= 1)
      REQUIRE (
        (builder.assign ((reg).value (),
                         separator
                           ? Operand (
                               mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                             &(reg).value ()))
                           : integer (3)))
          .has_value ());
    else if (separator == 2)
      REQUIRE ((builder.emit (Opcode::Add, {Operand ((reg).value ()), integer (1), integer (2)}))
                 .has_value ());
    else {
      auto block = mirnext::ir::detail::access (*(e.function)).create_block ();
      REQUIRE ((block).has_value ());
      REQUIRE ((builder.set_insert_point ((block).value ())).has_value ());
      if (separator == 3)
        REQUIRE ((mirnext::ir::detail::access (*(e.function)).create_label ((block).value ()))
                   .has_value ());
    }
    auto exit = mirnext::ir::detail::access (*(e.function)).create_block ();
    REQUIRE ((exit).has_value ());
    auto label = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value ());
    REQUIRE ((label).has_value ());
    REQUIRE ((builder.emit (Opcode::Bo, {Operand ((label).value ())})).has_value ());
    REQUIRE ((InstructionBuilder ((exit).value ()).ret (integer (42))).has_value ());
    CHECK (e.function->verify ().has_value () == (separator == 4));
  }
}
TEST_CASE ("native identifiers and signatures are interned and survive temporary input strings") {
  Program context;
  auto one = context.intern_signature ({{Type::i64 ()}, {Type::pointer ()}, true});
  REQUIRE ((one).has_value ());
  auto two = context.intern_signature ({{Type::i64 ()}, {Type::pointer ()}, true});
  REQUIRE ((two).has_value ());
  CHECK (&(one).value () == &(two).value ());
  auto text = context.intern_string ("identifier");
  CHECK (text.data () == context.intern_string ("identifier").data ());
  auto module = mirnext::ir::detail::access (context).create_module (std::string (100, 'm'));
  REQUIRE ((module).has_value ());
  auto function = mirnext::ir::detail::access (((module)).value ())
                    .create_function (std::string (100, 'f'), {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block
    = mirnext::ir::detail::access (((function)).value ()).create_block (std::string (100, 'b'));
  REQUIRE ((block).has_value ());
  auto reg = mirnext::ir::detail::access (((function)).value ())
               .create_register (Type::i64 (), std::string (100, 'r'), std::string (100, 'h'));
  REQUIRE ((reg).has_value ());
  CHECK (module.value ().name () == std::string (100, 'm'));
  CHECK (function.value ().symbol ().name () == std::string (100, 'f'));
  CHECK (function.value ().find_register (std::string (100, 'r')).get () == &(reg).value ());
  CHECK (reg.value ().hardware_binding () == std::string (100, 'h'));
  InstructionBuilder builder ((block).value ());
  REQUIRE ((builder.ret (integer (42))).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  static_assert (!std::is_copy_constructible_v<Register>);
  static_assert (!std::is_move_constructible_v<Instruction>);
  static_assert (std::is_move_constructible_v<Program>);
}
TEST_CASE ("native edits reject cross function and module ownership without changing IR") {
  Example e;
  auto other
    = mirnext::ir::detail::access (*(e.module)).create_function ("other", {{Type::i64 ()}, {}});
  REQUIRE ((other).has_value ());
  auto block = mirnext::ir::detail::access (((other)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  auto label = mirnext::ir::detail::access (((other)).value ()).create_label ((block).value ());
  REQUIRE ((label).has_value ());
  auto reg = mirnext::ir::detail::access (((other)).value ()).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  InstructionBuilder other_builder ((block).value ());
  REQUIRE ((other_builder.ret (integer (0))).has_value ());
  auto own = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((own).has_value ());
  const Operand bad_register[]{Operand ((own).value ()), Operand ((reg).value ())};
  CHECK_FALSE ((validate_instruction (*e.function, Opcode::Mov, bad_register)).has_value ());
  const Operand bad_label[]{Operand ((label).value ())};
  CHECK_FALSE ((validate_instruction (*e.function, Opcode::Jmp, bad_label)).has_value ());
  CHECK_FALSE (
    (mirnext::ir::detail::access (*(e.function)).create_label ((block).value ())).has_value ());
  InstructionBuilder builder (*e.entry);
  auto ret = builder.ret (integer (42));
  REQUIRE ((ret).has_value ());
  auto before = e.function->revisions ();
  CHECK_FALSE ((InstructionRewriter (*e.function).move_before ((ret).value (), (block).value ()))
                 .has_value ());
  CHECK (e.function->revisions () == before);
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native types reject negative enum values and invalid block classifications") {
  CHECK_FALSE (Type (static_cast<Type::Kind> (-1)).valid ());
  CHECK_FALSE (Type::block (16, static_cast<Type::BlockClass> (-1)).valid ());
  CHECK_FALSE (Type::block (SIZE_MAX).valid ());
  CHECK_FALSE (((Signature{{Type::block (16)}, {}}.verify ())).has_value ());
  CHECK_FALSE (((Signature{{}, {Type::block (0)}}.verify ())).has_value ());
  CHECK_FALSE (((Signature{{Type::i64 ()}, {Type::return_block (16)}}.verify ())).has_value ());
}
