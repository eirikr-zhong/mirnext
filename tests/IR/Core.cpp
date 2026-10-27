/* Licensed under the MIT License. Register IR construction and validation tests. */
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
  Context context;
  Module *module;
  Function *function;
  BasicBlock *entry;
  Example () {
    auto m = context.create_module ("example");
    REQUIRE (m);
    module = *m;
    auto f = module->create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE (f);
    function = *f;
    auto b = function->create_block ("entry");
    REQUIRE (b);
    entry = *b;
  }
};
Operand integer (std::int64_t value) { return Operand (value); }
}  // namespace
TEST_CASE ("native expression data rejects memory and call effects including later rewrites") {
  Example e;
  auto value = e.function->create_register (Type::i64 ());
  REQUIRE (value);
  IRBuilder builder (*e.entry);
  SUBCASE ("memory") {
    REQUIRE (builder.assign (**value, Operand (Memory{Type::i64 ()})));
    REQUIRE (builder.ret (Operand (**value)));
    auto count = e.module->items ().size ();
    auto failed = e.module->create_data ("expression", ExpressionData{e.function});
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("must not access memory") != std::string::npos);
    CHECK (e.module->items ().size () == count);
    CHECK (e.module->find_symbol ("expression") == nullptr);
  }
  SUBCASE ("call") {
    auto imported = e.module->import ("foreign", e.function->signature ());
    REQUIRE (imported);
    REQUIRE (
      builder.emit (Opcode::Call, {Operand (**imported), Operand (**imported), Operand (**value)}));
    REQUIRE (builder.ret (Operand (**value)));
    auto failed = e.module->create_data ("expression", ExpressionData{e.function});
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("must not call functions") != std::string::npos);
  }
  SUBCASE ("mutation after data creation") {
    auto assignment = builder.assign (**value, integer (42));
    REQUIRE (assignment);
    REQUIRE (builder.ret (Operand (**value)));
    REQUIRE (e.module->create_data ("expression", ExpressionData{e.function}));
    REQUIRE (e.context.verify ());
    REQUIRE (
      IRRewriter (*e.function).replace_operand (**assignment, 1, Operand (Memory{Type::i64 ()})));
    auto failed = e.context.verify ();
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("must not access memory") != std::string::npos);
  }
}
TEST_CASE ("native context validates exported names imported signatures and untyped calls") {
  Example e;
  IRBuilder exported (*e.entry);
  REQUIRE (exported.ret (integer (42)));
  REQUIRE (e.module->export_symbol (e.function->symbol ()));
  auto other = e.context.create_module ("consumer");
  REQUIRE (other);
  SUBCASE ("matching import and independent clone") {
    auto imported = (*other)->import ("entry", e.function->signature ());
    REQUIRE (imported);
    auto function = (*other)->create_function ("caller", e.function->signature ());
    REQUIRE (function);
    auto block = (*function)->create_block ();
    REQUIRE (block);
    auto result = (*function)->create_register (Type::i64 ());
    REQUIRE (result);
    IRBuilder builder (**block);
    REQUIRE (builder.emit (Opcode::Call,
                           {Operand (**imported), Operand (**imported), Operand (**result)}));
    REQUIRE (builder.ret (Operand (**result)));
    REQUIRE (e.context.verify ());
    auto copied = clone (e.context);
    REQUIRE (copied);
    CHECK (dump (**copied) == dump (e.context));
  }
  SUBCASE ("duplicate export") {
    auto function = (*other)->create_function ("entry", e.function->signature ());
    REQUIRE (function);
    auto block = (*function)->create_block ();
    REQUIRE (block);
    IRBuilder builder (**block);
    REQUIRE (builder.ret (integer (0)));
    REQUIRE ((*other)->export_symbol ((*function)->symbol ()));
    auto failed = e.context.verify ();
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().code == ErrorCode::DuplicateName);
  }
  SUBCASE ("import mismatch") {
    REQUIRE ((*other)->import ("entry", Signature{{Type (Type::Kind::F64)}, {}}));
    auto failed = e.context.verify ();
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("consumer/entry: import signature") != std::string::npos);
  }
  SUBCASE ("untyped import call mismatch") {
    auto imported = (*other)->import ("entry");
    REQUIRE (imported);
    Signature signature{{Type (Type::Kind::F64)}, {}};
    auto prototype = (*other)->prototype ("call_type", signature);
    REQUIRE (prototype);
    auto function = (*other)->create_function ("caller", e.function->signature ());
    REQUIRE (function);
    auto block = (*function)->create_block ();
    REQUIRE (block);
    auto result = (*function)->create_register (Type (Type::Kind::F64));
    REQUIRE (result);
    IRBuilder builder (**block);
    REQUIRE (builder.emit (Opcode::Call,
                           {Operand (**prototype), Operand (**imported), Operand (**result)}));
    REQUIRE (builder.ret (integer (0)));
    REQUIRE ((*other)->verify ());
    auto failed = e.context.verify ();
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("consumer/caller: cross-module call signature")
           != std::string::npos);
  }
  SUBCASE ("external host import has no required in-context definition") {
    REQUIRE ((*other)->import ("host", e.function->signature ()));
    REQUIRE (e.context.verify ());
  }
}
TEST_CASE ("native verifier enforces direct and cross module jump calling conventions") {
  for (bool cross_module : {false, true})
    for (bool jump_return : {false, true})
      for (bool jump_call : {false, true}) {
        CAPTURE (cross_module);
        CAPTURE (jump_return);
        CAPTURE (jump_call);
        Context context;
        auto module = context.create_module ("target");
        REQUIRE (module);
        auto callee = (*module)->create_function ("callee", {});
        REQUIRE (callee);
        auto body = (*callee)->create_block ();
        REQUIRE (body);
        IRBuilder target (**body);
        if (jump_return)
          REQUIRE (target.emit (Opcode::JRet, {integer (0)}));
        else
          REQUIRE (target.ret ());
        auto caller_module = *module;
        const Symbol *symbol = &(*callee)->symbol ();
        if (cross_module) {
          REQUIRE ((*module)->export_symbol (*symbol));
          auto other = context.create_module ("caller");
          REQUIRE (other);
          caller_module = *other;
          auto imported = caller_module->import ("callee", Signature{});
          REQUIRE (imported);
          symbol = *imported;
        }
        auto caller = caller_module->create_function ("caller", {});
        REQUIRE (caller);
        auto block = (*caller)->create_block ();
        REQUIRE (block);
        IRBuilder builder (**block);
        REQUIRE (builder.emit (jump_call ? Opcode::JCall : Opcode::Call,
                               {Operand (*symbol), Operand (*symbol)}));
        if (!jump_call) REQUIRE (builder.ret ());
        auto checked = context.verify ();
        CHECK (static_cast<bool> (checked) == (jump_call == jump_return));
        if (!checked)
          CHECK (checked.error ().message.find ("call convention") != std::string::npos);
      }
}
TEST_CASE ("native IR has stable basic blocks registers labels and instruction identities") {
  Example e;
  IRBuilder builder (*e.entry);
  auto a = e.function->create_register (Type::i64 (), "a");
  REQUIRE (a);
  auto first = builder.assign (**a, integer (40));
  REQUIRE (first);
  auto result = builder.binary (Opcode::Add, Operand (**a), integer (2), "result");
  REQUIRE (result);
  REQUIRE (builder.ret (Operand (**result)));
  REQUIRE (e.context.verify ());
  auto identity = *first;
  for (unsigned n = 0; n < 2000; ++n) REQUIRE (e.function->create_register (Type::i64 ()));
  CHECK (&identity->function () == e.function);
  CHECK (identity->opcode () == Opcode::Mov);
  CHECK (e.function->find_register ("a") == *a);
  auto text = dump (e.context);
  CHECK (text.find ("add") != text.npos);
  CHECK (text.find ("ret") != text.npos);
  auto revisions = e.function->revisions ();
  auto count = e.function->instruction_count ();
  for (unsigned n = 0; n < 4; ++n) {
    CHECK (dump (e.context) == text);
    CHECK (e.context.verify ());
  }
  CHECK (e.function->revisions () == revisions);
  CHECK (e.function->instruction_count () == count);
  static_assert (
    std::is_same_v<decltype (*std::as_const (*e.function).blocks ().begin ()), const BasicBlock &>);
  static_assert (std::is_same_v<decltype (*std::as_const (*e.entry).instructions ().begin ()),
                                const Instruction &>);
}
TEST_CASE ("native forward declaration and definition share a symbol identity") {
  Context context;
  auto module = context.create_module ("module");
  REQUIRE (module);
  auto forward = (*module)->forward ("entry");
  REQUIRE (forward);
  auto declaration = (*module)->declare_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (declaration);
  CHECK (*forward == *declaration);
  CHECK_FALSE (context.verify ());
  auto function = (*module)->define_function (**declaration);
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  REQUIRE (builder.ret (integer (42)));
  REQUIRE ((*module)->export_symbol (**forward));
  REQUIRE (context.verify ());
  CHECK ((*module)->find_symbol ("entry") == *forward);
  CHECK ((*forward)->function () == *function);
  CHECK ((*module)->items ().size () == 4);
}
TEST_CASE (
  "native hardware bindings preserve one identity and reject incompatible scalar classes") {
  Example e;
  auto first = e.function->create_register (Type::i64 (), "a", "r5");
  REQUIRE (first);
  auto second = e.function->create_register (Type (Type::Kind::U32), "b", "r5");
  REQUIRE (second);
  CHECK (*first == *second);
  CHECK (e.function->find_register ("b") == *first);
  CHECK_FALSE (e.function->create_register (Type (Type::Kind::F64), "wrong", "r5"));
  CHECK_FALSE (e.function->create_register (Type::block (16), "block"));
  IRBuilder builder (*e.entry);
  REQUIRE (builder.assign (**first, integer (42)));
  REQUIRE (builder.assign (**second, integer (0)));
  REQUIRE (builder.ret (Operand (**first)));
  REQUIRE (e.context.verify ());
  auto copied = clone (e.context);
  REQUIRE (copied);
  REQUIRE ((*copied)->verify ());
  auto &module = *(*copied)->modules ().begin ();
  auto &function = *module.functions ().begin ();
  CHECK (function.find_register ("a") == function.find_register ("b"));
  CHECK (function.find_register ("a") != *first);
}
TEST_CASE ("hardware addresses require an explicit ordinary register copy") {
  for (auto opcode : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    CAPTURE (opcode);
    Example e;
    auto bound = e.function->create_register (Type::i64 (), "bound", "r5");
    REQUIRE (bound);
    auto local = e.function->create_register (Type::i64 (), "copy");
    REQUIRE (local);
    auto pointer = e.function->create_register (Type::pointer (), "pointer");
    REQUIRE (pointer);
    IRBuilder builder (*e.entry);
    REQUIRE (builder.assign (**local, Operand (**bound)));
    auto address = builder.emit (opcode, {Operand (**pointer), Operand (**local)});
    REQUIRE (address);
    REQUIRE (builder.ret (Operand (**local)));
    REQUIRE (e.context.verify ());
    auto before = dump (e.context);
    auto revision = e.function->revisions ();
    IRRewriter rewriter (*e.function);
    auto failed = rewriter.replace_operand (**address, 1, Operand (**bound));
    REQUIRE_FALSE (failed);
    CHECK (failed.error ().message.find ("copy it to an ordinary local") != std::string::npos);
    CHECK_FALSE (rewriter.materialize_register_storage (**bound));
    CHECK_FALSE ((*bound)->is_memory_backed ());
    CHECK (e.function->revisions () == revision);
    CHECK (dump (e.context) == before);
    REQUIRE (e.context.verify ());
    // A fresh builder checks invalid emit before publishing any instruction.
    Example invalid;
    auto r = invalid.function->create_register (Type::i64 (), "bound", "r5");
    REQUIRE (r);
    auto p = invalid.function->create_register (Type::pointer ());
    REQUIRE (p);
    IRBuilder bad (*invalid.entry);
    auto revisions = invalid.function->revisions ();
    CHECK_FALSE (bad.emit (opcode, {Operand (**p), Operand (**r)}));
    CHECK (invalid.function->instruction_count () == 0);
    CHECK (invalid.function->revisions () == revisions);
  }
  Example e;
  auto bound = e.function->create_register (Type::i64 (), "bound", "r5");
  REQUIRE (bound);
  IRBuilder builder (*e.entry);
  auto before = e.function->revisions ();
  auto count = std::distance (e.function->registers ().begin (), e.function->registers ().end ());
  CHECK_FALSE (builder.address_of (**bound));
  CHECK (e.function->revisions () == before);
  CHECK (std::distance (e.function->registers ().begin (), e.function->registers ().end ())
         == count);
  CHECK (e.function->instruction_count () == 0);
}
TEST_CASE ("native builder reports the first error and does not publish failed operations") {
  Example e;
  IRBuilder builder (*e.entry);
  auto before = e.function->revisions ();
  auto failed = builder.binary (Opcode::Add, Operand (1.0), integer (2));
  CHECK_FALSE (failed);
  CHECK (e.function->instruction_count () == 0);
  CHECK (e.function->revisions () == before);
  auto repeated = builder.ret (integer (42));
  CHECK_FALSE (repeated);
  REQUIRE (e.module->error ());
  CHECK (repeated.error ().message == failed.error ().message);
  CHECK_FALSE (e.context.verify ());
}
TEST_CASE ("native rewriter edits are atomic and moving instructions preserves identity") {
  Example e;
  auto exit = e.function->create_block ("exit");
  REQUIRE (exit);
  auto label = e.function->create_label (**exit, "exit");
  REQUIRE (label);
  auto alias = e.function->create_label (**exit, "alias");
  REQUIRE (alias);
  auto reg = e.function->create_register (Type::i64 (), "a");
  REQUIRE (reg);
  IRBuilder builder (*e.entry);
  auto instruction = builder.assign (**reg, integer (40));
  REQUIRE (instruction);
  auto branch = builder.jump (**label);
  REQUIRE (branch);
  IRBuilder ending (**exit);
  auto ret = ending.ret (Operand (**reg));
  REQUIRE (ret);
  REQUIRE (e.context.verify ());
  IRRewriter rewriter (*e.function);
  auto revisions = e.function->revisions ();
  CHECK_FALSE (rewriter.replace_operand (**instruction, 1, Operand (1.0)));
  CHECK (e.function->revisions () == revisions);
  REQUIRE (rewriter.replace_operand (**instruction, 1, integer (42)));
  CHECK (e.function->revisions ().control_flow == revisions.control_flow);
  auto address = *instruction;
  REQUIRE (rewriter.move_before (**instruction, **exit, *ret));
  CHECK (&address->block () == *exit);
  CHECK (e.entry->size () == 1);
  CHECK ((*exit)->size () == 2);
  CHECK ((*exit)->labels ().size () == 2);
  REQUIRE (e.context.verify ());
}
TEST_CASE ("native cloning remaps symbols labels data and references before source destruction") {
  std::unique_ptr<Context> copied;
  std::string expected;
  {
    Example e;
    auto label = e.function->create_label (*e.entry, "entry");
    REQUIRE (label);
    IRBuilder builder (*e.entry);
    REQUIRE (builder.ret (integer (42)));
    auto data
      = e.module->create_data ("value", InitializedData{Type::i64 (), {integer (42)}}, false);
    REQUIRE (data);
    REQUIRE (e.module->create_data ("pointer", SymbolReference{&(*data)->symbol (), 8}));
    REQUIRE (e.module->create_data ("labels", LabelReference{*label, *label, 4}));
    REQUIRE (e.module->create_data ("expression", ExpressionData{e.function}));
    REQUIRE (e.module->create_data ("zero", ZeroData{32}));
    REQUIRE (e.module->export_symbol (e.function->symbol ()));
    REQUIRE (e.module->create_data ("", InitializedData{Type::i64 (), {integer (17)}}));
    REQUIRE (e.module->create_data ("", ZeroData{8}));
    REQUIRE (e.context.verify ());
    expected = dump (e.context);
    auto clone_result = clone (e.context);
    REQUIRE (clone_result);
    copied = std::move (*clone_result);
  }
  REQUIRE (copied->verify ());
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
    REQUIRE (copied);
    CHECK (dump (**copied) == output);
    ++covered;
  }
  CHECK (covered == instruction_descriptors ().size ());
}
TEST_CASE ("native IR checks block ABI sizes classes and variadic promotions before insertion") {
  for (unsigned classification = 0; classification < 5; ++classification) {
    Example e;
    auto type = Type::block (16, static_cast<Type::BlockClass> (classification));
    Signature signature{{Type::i64 ()}, {type}, true};
    auto prototype = e.module->prototype ("signature", signature);
    REQUIRE (prototype);
    auto imported = e.module->import ("callee", signature);
    REQUIRE (imported);
    auto result = e.function->create_register (Type::i64 ());
    REQUIRE (result);
    auto address = e.function->create_register (Type::pointer ());
    REQUIRE (address);
    Memory block{type, *address};
    std::vector<Operand> operands{Operand (**prototype), Operand (**imported), Operand (**result),
                                  Operand (block), Operand (1.0)};
    REQUIRE (validate_instruction (*e.function, Opcode::Call, operands));
    auto bad = operands;
    auto memory = block;
    memory.type = Type::block (8, static_cast<Type::BlockClass> (classification));
    bad[3] = Operand (memory);
    CHECK_FALSE (validate_instruction (*e.function, Opcode::Call, bad));
    bad = operands;
    memory.type = Type::block (16, static_cast<Type::BlockClass> ((classification + 1) % 5));
    bad[3] = Operand (memory);
    CHECK_FALSE (validate_instruction (*e.function, Opcode::Call, bad));
    bad = operands;
    bad[4] = Operand (1.0f);
    CHECK_FALSE (validate_instruction (*e.function, Opcode::Call, bad));
    bad = operands;
    bad.pop_back ();
    bad.pop_back ();
    CHECK_FALSE (validate_instruction (*e.function, Opcode::Call, bad));
    IRBuilder builder (*e.entry);
    REQUIRE (builder.emit (Opcode::Call, operands));
    REQUIRE (builder.ret (Operand (**result)));
    REQUIRE (e.context.verify ());
  }
}
TEST_CASE ("native overflow flags follow producer and register move rules") {
  for (auto producer : {Opcode::Addo, Opcode::Addos, Opcode::Subo, Opcode::Subos, Opcode::Mulo,
                        Opcode::Mulos, Opcode::UMulo, Opcode::UMulos}) {
    for (auto branch : {Opcode::Bo, Opcode::Bno, Opcode::UBo, Opcode::UBno}) {
      CAPTURE (producer);
      CAPTURE (branch);
      Example e;
      auto reg = e.function->create_register (Type::i64 ());
      REQUIRE (reg);
      auto exit = e.function->create_block ();
      REQUIRE (exit);
      auto label = e.function->create_label (**exit);
      REQUIRE (label);
      IRBuilder builder (*e.entry);
      REQUIRE (builder.emit (producer, {Operand (**reg), integer (1), integer (2)}));
      REQUIRE (builder.assign (**reg, Operand (**reg)));
      REQUIRE (builder.emit (Opcode::Mov, {Operand (Memory{Type::i64 (), *reg}), Operand (**reg)}));
      REQUIRE (builder.emit (branch, {Operand (**label)}));
      REQUIRE (IRBuilder (**exit).ret (integer (42)));
      bool unsigned_branch = branch == Opcode::UBo || branch == Opcode::UBno;
      bool invalid_signedness
        = (unsigned_branch && (producer == Opcode::Mulo || producer == Opcode::Mulos))
          || (!unsigned_branch && (producer == Opcode::UMulo || producer == Opcode::UMulos));
      CHECK (e.function->verify ().has_value () == !invalid_signedness);
    }
  }
  for (unsigned separator = 0; separator < 5; ++separator) {
    Example e;
    auto reg = e.function->create_register (Type::i64 ());
    REQUIRE (reg);
    IRBuilder builder (*e.entry);
    REQUIRE (builder.emit (Opcode::Addo, {Operand (**reg), integer (1), integer (2)}));
    if (separator <= 1)
      REQUIRE (
        builder.assign (**reg, separator ? Operand (Memory{Type::i64 (), *reg}) : integer (3)));
    else if (separator == 2)
      REQUIRE (builder.emit (Opcode::Add, {Operand (**reg), integer (1), integer (2)}));
    else {
      auto block = e.function->create_block ();
      REQUIRE (block);
      REQUIRE (builder.set_insert_point (**block));
      if (separator == 3) REQUIRE (e.function->create_label (**block));
    }
    auto exit = e.function->create_block ();
    REQUIRE (exit);
    auto label = e.function->create_label (**exit);
    REQUIRE (label);
    REQUIRE (builder.emit (Opcode::Bo, {Operand (**label)}));
    REQUIRE (IRBuilder (**exit).ret (integer (42)));
    CHECK (e.function->verify ().has_value () == (separator == 4));
  }
}
TEST_CASE ("native identifiers and signatures are interned and survive temporary input strings") {
  Context context;
  auto one = context.intern_signature ({{Type::i64 ()}, {Type::pointer ()}, true});
  REQUIRE (one);
  auto two = context.intern_signature ({{Type::i64 ()}, {Type::pointer ()}, true});
  REQUIRE (two);
  CHECK (*one == *two);
  auto text = context.intern_string ("identifier");
  CHECK (text.data () == context.intern_string ("identifier").data ());
  auto module = context.create_module (std::string (100, 'm'));
  REQUIRE (module);
  auto function = (*module)->create_function (std::string (100, 'f'), {{Type::i64 ()}, {}});
  REQUIRE (function);
  auto block = (*function)->create_block (std::string (100, 'b'));
  REQUIRE (block);
  auto reg
    = (*function)->create_register (Type::i64 (), std::string (100, 'r'), std::string (100, 'h'));
  REQUIRE (reg);
  CHECK ((*module)->name () == std::string (100, 'm'));
  CHECK ((*function)->symbol ().name () == std::string (100, 'f'));
  CHECK ((*function)->find_register (std::string (100, 'r')) == *reg);
  CHECK ((*reg)->hardware_binding () == std::string (100, 'h'));
  IRBuilder builder (**block);
  REQUIRE (builder.ret (integer (42)));
  REQUIRE (context.verify ());
  static_assert (!std::is_copy_constructible_v<Register>);
  static_assert (!std::is_move_constructible_v<Instruction>);
  static_assert (!std::is_move_constructible_v<Context>);
}
TEST_CASE ("native edits reject cross function and module ownership without changing IR") {
  Example e;
  auto other = e.module->create_function ("other", {{Type::i64 ()}, {}});
  REQUIRE (other);
  auto block = (*other)->create_block ();
  REQUIRE (block);
  auto label = (*other)->create_label (**block);
  REQUIRE (label);
  auto reg = (*other)->create_register (Type::i64 ());
  REQUIRE (reg);
  IRBuilder other_builder (**block);
  REQUIRE (other_builder.ret (integer (0)));
  auto own = e.function->create_register (Type::i64 ());
  REQUIRE (own);
  const Operand bad_register[]{Operand (**own), Operand (**reg)};
  CHECK_FALSE (validate_instruction (*e.function, Opcode::Mov, bad_register));
  const Operand bad_label[]{Operand (**label)};
  CHECK_FALSE (validate_instruction (*e.function, Opcode::Jmp, bad_label));
  CHECK_FALSE (e.function->create_label (**block));
  IRBuilder builder (*e.entry);
  auto ret = builder.ret (integer (42));
  REQUIRE (ret);
  auto before = e.function->revisions ();
  CHECK_FALSE (IRRewriter (*e.function).move_before (**ret, **block));
  CHECK (e.function->revisions () == before);
  REQUIRE (e.context.verify ());
}
TEST_CASE ("native types reject negative enum values and invalid block classifications") {
  CHECK_FALSE (Type (static_cast<Type::Kind> (-1)).valid ());
  CHECK_FALSE (Type::block (16, static_cast<Type::BlockClass> (-1)).valid ());
  CHECK_FALSE (Type::block (SIZE_MAX).valid ());
  CHECK_FALSE ((Signature{{Type::block (16)}, {}}.verify ()));
  CHECK_FALSE ((Signature{{}, {Type::block (0)}}.verify ()));
  CHECK_FALSE ((Signature{{Type::i64 ()}, {Type::return_block (16)}}.verify ()));
}
