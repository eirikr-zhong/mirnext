#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. Variable storage, ownership, permissions and atomic construction.
 */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "Variables.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *block;
  Example () {
    auto m = mirnext::ir::detail::access (context).create_module ("variables");
    REQUIRE ((m).has_value ());
    module = &(m).value ();
    auto f
      = mirnext::ir::detail::access (*(module)).create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    function = &(f).value ();
    auto b = mirnext::ir::detail::access (*(function)).create_block ();
    REQUIRE ((b).has_value ());
    block = &(b).value ();
  }
};
std::size_t register_count (const Function &function) {
  return static_cast<std::size_t> (std::ranges::distance (function.registers ()));
}
}  // namespace
TEST_CASE ("native variable DSL clones and formats all storage and access forms") {
  for (auto mode : {test::VariableExample::Initialized, test::VariableExample::Zero,
                    test::VariableExample::Constant, test::VariableExample::Boolean,
                    test::VariableExample::Floating, test::VariableExample::Pointer,
                    test::VariableExample::ExpressionInitializer,
                    test::VariableExample::CrossModule, test::VariableExample::Narrow}) {
    CAPTURE (static_cast<unsigned> (mode));
    auto context = test::variable_example (mode);
    auto before = dump (*context);
    auto copied = clone (*context);
    INFO (((copied).has_value () ? "" : copied.error ().message));
    REQUIRE ((copied).has_value ());
    CHECK (dump ((copied).value ()) == before);
    auto text = serialization::write_text (*context);
    INFO (((text).has_value () ? "" : text.error ().message));
    REQUIRE ((text).has_value ());
    auto parsed = serialization::parse_text ((text).value ());
    REQUIRE ((parsed).has_value ());
    auto normalized = serialization::write_text ((parsed).value ());
    REQUIRE ((normalized).has_value ());
    CHECK ((normalized).value () == (text).value ());
    auto binary = serialization::write_binary (*context);
    REQUIRE ((binary).has_value ());
    auto decoded = serialization::read_binary ((binary).value ());
    REQUIRE ((decoded).has_value ());
    CHECK (dump (*context) == before);
    CHECK ((decoded.value ().verify ()).has_value ());
  }
}
TEST_CASE ("native scalar variables use the MIR storage widths") {
  Example e;
  for (auto [type, size] : {std::pair{Type (Type::Kind::Bool), std::size_t{8}},
                            {Type (Type::Kind::I8), 1},
                            {Type (Type::Kind::U8), 1},
                            {Type (Type::Kind::I16), 2},
                            {Type (Type::Kind::U16), 2},
                            {Type (Type::Kind::I32), 4},
                            {Type (Type::Kind::U32), 4},
                            {Type::i64 (), 8},
                            {Type (Type::Kind::U64), 8},
                            {Type (Type::Kind::F32), 4},
                            {Type (Type::Kind::F64), 8},
                            {Type (Type::Kind::LongDouble), sizeof (long double)},
                            {Type::pointer (), 8}}) {
    auto variable
      = mirnext::ir::detail::access (*(e.module))
          .create_variable ("v" + std::to_string (static_cast<unsigned> (type.kind ())), type);
    REQUIRE ((variable).has_value ());
    auto storage
      = std::get_if<ZeroData> (&variable.value ().symbol ().data ().get ()->initializer ());
    REQUIRE (storage);
    CHECK (storage->size == size);
  }
  REQUIRE ((InstructionBuilder (*e.block).ret (42)).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native variable definitions preserve ordered data items and stable bindings") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("data");
  REQUIRE ((module).has_value ());
  auto head
    = mirnext::ir::detail::access (((module)).value ()).create_variable ("head", Type::i64 (), 20);
  REQUIRE ((head).has_value ());
  auto tail = mirnext::ir::detail::access (((module)).value ())
                .create_data ({}, InitializedData{Type::i64 (), {22}});
  REQUIRE ((tail).has_value ());
  auto zero
    = mirnext::ir::detail::access (((module)).value ()).create_variable ("zero", Type::i64 ());
  REQUIRE ((zero).has_value ());
  REQUIRE (module.value ().items ().size () == 3);
  CHECK (module.value ().items ()[0].get<GlobalData> ().get ()
         == head.value ().symbol ().data ().get ());
  CHECK (module.value ().items ()[1].get<GlobalData> ().get () == &(tail).value ());
  CHECK (module.value ().items ()[2].get<GlobalData> ().get ()
         == zero.value ().symbol ().data ().get ());
  auto rebound = mirnext::ir::detail::access (((module)).value ())
                   .bind_variable (head.value ().symbol (), Type::i64 ());
  REQUIRE ((rebound).has_value ());
  CHECK (&(rebound).value () == &(head).value ());
  CHECK (module.value ().items ().size () == 3);
  auto copied = clone (context);
  REQUIRE ((copied).has_value ());
  CHECK (dump ((copied).value ()) == dump (context));
}
TEST_CASE ("native variable failures retain the first error and publish no storage") {
  Example e;
  const auto before = e.module->items ().size ();
  const auto revision = e.module->data_revision ();
  SUBCASE ("block type") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (*(e.module)).create_variable ("bad", Type::block (16)))
        .has_value ());
  }
  SUBCASE ("undef type") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (*(e.module)).create_variable ("bad", Type (Type::Kind::Undef)))
        .has_value ());
  }
  SUBCASE ("invalid type") {
    CHECK_FALSE ((mirnext::ir::detail::access (*(e.module))
                    .create_variable ("bad", Type (static_cast<Type::Kind> (99))))
                   .has_value ());
  }
  SUBCASE ("empty name") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (*(e.module)).create_variable ({}, Type::i64 ())).has_value ());
  }
  SUBCASE ("wrong literal type") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (*(e.module)).create_variable ("bad", Type::i64 (), 1.0))
        .has_value ());
  }
  SUBCASE ("dynamic initializer") {
    auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
    REQUIRE ((reg).has_value ());
    CHECK_FALSE ((mirnext::ir::detail::access (*(e.module))
                    .create_variable ("bad", Type::i64 (), Operand ((reg).value ())))
                   .has_value ());
  }
  SUBCASE ("wrong boolean initializer") {
    CHECK_FALSE ((mirnext::ir::detail::access (*(e.module))
                    .create_variable ("bad", Type (Type::Kind::Bool), 1.0))
                   .has_value ());
  }
  SUBCASE ("duplicate name") {
    CHECK_FALSE ((mirnext::ir::detail::access (*(e.module)).create_variable ("entry", Type::i64 ()))
                   .has_value ());
  }
  CHECK (e.module->items ().size () == before);
  CHECK (e.module->data_revision () == revision);
  CHECK (e.module->find_symbol ("bad").get () == nullptr);
  REQUIRE ((e.module->error ()).has_value ());
  auto again
    = mirnext::ir::detail::access (*(e.module)).create_variable ("later", Type::i64 (), 42);
  REQUIRE_FALSE ((again).has_value ());
  CHECK (again.error ().message == e.module->error ().value ().message);
}
TEST_CASE ("native variable access failures are atomic") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto variable
    = mirnext::ir::detail::access (*(e.module)).create_variable ("value", Type::i64 (), 42);
  REQUIRE ((variable).has_value ());
  auto revision = e.function->revisions ();
  SUBCASE ("wrong store type") {
    CHECK_FALSE ((builder.store ((variable).value (), 1.0)).has_value ());
  }
  SUBCASE ("foreign variable") {
    auto other = mirnext::ir::detail::access (e.context).create_module ("other");
    REQUIRE ((other).has_value ());
    auto foreign = mirnext::ir::detail::access (((other)).value ())
                     .create_variable ("value", Type::i64 (), 42);
    REQUIRE ((foreign).has_value ());
    CHECK_FALSE ((builder.load ((foreign).value ())).has_value ());
  }
  SUBCASE ("foreign store value") {
    auto other = mirnext::ir::detail::access (*(e.module)).create_function ("other", {});
    REQUIRE ((other).has_value ());
    auto foreign = mirnext::ir::detail::access (((other)).value ()).create_register (Type::i64 ());
    REQUIRE ((foreign).has_value ());
    CHECK_FALSE ((builder.store ((variable).value (), Operand ((foreign).value ()))).has_value ());
  }
  SUBCASE ("invalid result name") {
    CHECK_FALSE ((builder.load ((variable).value (), std::string_view ("a\0b", 3))).has_value ());
  }
  SUBCASE ("closed insertion point") {
    builder.clear_insert_point ();
    CHECK_FALSE ((builder.load ((variable).value ())).has_value ());
  }
  SUBCASE ("duplicate result name") {
    REQUIRE (
      (mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "existing"))
        .has_value ());
    auto count = register_count (*e.function);
    auto before = e.function->revisions ();
    CHECK_FALSE ((builder.load ((variable).value (), "existing")).has_value ());
    CHECK (register_count (*e.function) == count);
    CHECK (e.function->revisions () == before);
    CHECK (e.block->size () == 0);
    return;
  }
  SUBCASE ("terminated block") {
    REQUIRE ((builder.ret (42)).has_value ());
    auto before = e.function->revisions ();
    CHECK_FALSE ((builder.load ((variable).value ())).has_value ());
    CHECK (e.function->revisions () == before);
    CHECK (e.block->size () == 1);
    CHECK (register_count (*e.function) == 0);
    return;
  }
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == revision);
}
TEST_CASE ("native readonly variable bindings reject stores before emitting addresses") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto constant = mirnext::ir::detail::access (*(e.module))
                    .create_variable ("constant", Type::i64 (), 42, false);
  REQUIRE ((constant).has_value ());
  auto value = builder.load ((constant).value ());
  REQUIRE ((value).has_value ());
  auto count = register_count (*e.function);
  auto instructions = e.block->size ();
  auto revision = e.function->revisions ();
  CHECK_FALSE ((builder.store ((constant).value (), 0)).has_value ());
  CHECK (register_count (*e.function) == count);
  CHECK (e.block->size () == instructions);
  CHECK (e.function->revisions () == revision);
}
TEST_CASE ("native boolean variable initialization and access normalize integer values") {
  Example e;
  auto variable = mirnext::ir::detail::access (*(e.module))
                    .create_variable ("flag", Type (Type::Kind::Bool), -7);
  REQUIRE ((variable).has_value ());
  CHECK ((std::get<InitializedData> (variable.value ().symbol ().data ().value ().initializer ())
            .values[0]
            .get<std::int64_t> ())
           .value ()
         == 1);
  InstructionBuilder builder (*e.block);
  REQUIRE ((builder.store ((variable).value (), UINT64_MAX)).has_value ());
  CHECK (e.block->size () == 2);
  CHECK (register_count (*e.function) == 1);
  auto dynamic = builder.binary (Opcode::Add, 40, 2);
  REQUIRE ((dynamic).has_value ());
  REQUIRE ((builder.store ((variable).value (), Operand ((dynamic).value ()))).has_value ());
  auto loaded = builder.load ((variable).value (), "flag");
  REQUIRE ((loaded).has_value ());
  CHECK (loaded.value ().type ().kind () == Type::Kind::Bool);
  REQUIRE ((builder.ret (Operand ((loaded).value ()))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  unsigned comparisons = 0;
  for (auto &instruction : e.block->instructions ())
    if (instruction.opcode () == Opcode::Ne) ++comparisons;
  CHECK (comparisons == 2);
  auto bytes = serialization::write_binary (e.context);
  REQUIRE ((bytes).has_value ());
  CHECK ((serialization::read_binary ((bytes).value ())).has_value ());
}
TEST_CASE ("native bindings validate existing storage and recover typed views after parsing") {
  auto parsed = serialization::parse_text ("data: module\nvalue: i64 42\nzero: bss 8\nendmodule\n");
  REQUIRE ((parsed).has_value ());
  auto &module = *parsed.value ().modules ().begin ();
  auto symbol = module.find_symbol ("value").get ();
  REQUIRE (symbol);
  auto bound = mirnext::ir::detail::access (module).bind_variable (*symbol, Type::i64 ());
  REQUIRE ((bound).has_value ());
  CHECK ((bound.value ().symbol ().data ()).has_value ());
  auto zero
    = mirnext::ir::detail::access (module).bind_variable (*module.find_symbol ("zero").get (),
                                                          Type::pointer ());
  REQUIRE ((zero).has_value ());
  CHECK ((parsed.value ().verify ()).has_value ());
  SUBCASE ("wrong width") {
    CHECK_FALSE (
      (mirnext::ir::detail::access (module).bind_variable (*symbol, Type (Type::Kind::I32)))
        .has_value ());
  }
  SUBCASE ("same binding") {
    auto repeated = mirnext::ir::detail::access (module).bind_variable (*symbol, Type::i64 ());
    REQUIRE ((repeated).has_value ());
    CHECK (&(repeated).value () == &(bound).value ());
  }
}
TEST_CASE ("native variable import checks ignore module creation order") {
  for (bool definition_first : {false, true})
    for (unsigned mode = 0; mode < 6; ++mode) {
      CAPTURE (definition_first);
      CAPTURE (mode);
      Program context;
      auto first = mirnext::ir::detail::access (context).create_module ("first");
      REQUIRE ((first).has_value ());
      auto second = mirnext::ir::detail::access (context).create_module ("second");
      REQUIRE ((second).has_value ());
      auto definition = definition_first ? &(first).value () : &(second).value ();
      auto consumer = definition_first ? &(second).value () : &(first).value ();
      auto variable = mirnext::ir::detail::access (*(definition))
                        .create_variable ("value", Type::i64 (), 42, false);
      REQUIRE ((variable).has_value ());
      REQUIRE (
        (mirnext::ir::detail::access (*(definition)).export_symbol (variable.value ().symbol ()))
          .has_value ());
      if (mode == 0) {
        REQUIRE (
          (mirnext::ir::detail::access (*(consumer)).import_variable ("value", Type::i64 (), false))
            .has_value ());
        CHECK ((context.verify ()).has_value ());
      } else if (mode == 1) {
        REQUIRE ((mirnext::ir::detail::access (*(consumer)).import_variable ("value", Type::i64 ()))
                   .has_value ());
        CHECK_FALSE ((context.verify ()).has_value ());
      } else if (mode == 2) {
        REQUIRE ((mirnext::ir::detail::access (*(consumer))
                    .import_variable ("value", Type (Type::Kind::F64), false))
                   .has_value ());
        CHECK_FALSE ((context.verify ()).has_value ());
      } else if (mode == 3) {
        auto raw = mirnext::ir::detail::access (*(definition))
                     .create_data ("raw", InitializedData{Type::i64 (), {42}});
        REQUIRE ((raw).has_value ());
        REQUIRE (
          (mirnext::ir::detail::access (*(definition)).export_symbol (raw.value ().symbol ()))
            .has_value ());
        REQUIRE ((mirnext::ir::detail::access (*(consumer)).import_variable ("raw", Type::i64 ()))
                   .has_value ());
        CHECK ((context.verify ()).has_value ());
      } else if (mode == 4) {
        auto function = mirnext::ir::detail::access (*(definition))
                          .create_function ("fn", {{Type::i64 ()}, {}});
        REQUIRE ((function).has_value ());
        auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
        REQUIRE ((block).has_value ());
        REQUIRE ((InstructionBuilder ((block).value ()).ret (42)).has_value ());
        REQUIRE (
          (mirnext::ir::detail::access (*(definition)).export_symbol (function.value ().symbol ()))
            .has_value ());
        REQUIRE ((mirnext::ir::detail::access (*(consumer)).import_variable ("fn", Type::i64 ()))
                   .has_value ());
        CHECK_FALSE ((context.verify ()).has_value ());
      } else {
        REQUIRE ((mirnext::ir::detail::access (*(consumer)).import_variable ("host", Type::i64 ()))
                   .has_value ());
        CHECK ((context.verify ()).has_value ());
      }
    }
}
TEST_CASE ("native cloned variables own their metadata independently of the source") {
  std::unique_ptr<Program> copied;
  {
    auto source = test::variable_example (test::VariableExample::CrossModule);
    auto clone_result = clone (*source);
    REQUIRE ((clone_result).has_value ());
    copied = std::make_unique<Program> (std::move ((clone_result).value ()));
  }
  auto &consumer = *copied->modules ().begin ();
  auto variable = consumer.find_variable ("value").get ();
  REQUIRE (variable);
  auto function
    = mirnext::ir::detail::access (consumer).create_function ("another", {{Type::i64 ()}, {}});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  REQUIRE ((builder.store (*variable, 42)).has_value ());
  auto result = builder.load (*variable);
  REQUIRE ((result).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  CHECK ((copied->verify ()).has_value ());
  CHECK ((serialization::write_binary (*copied)).has_value ());
}
TEST_CASE ("native variable symbols cannot be called as functions") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto variable = mirnext::ir::detail::access (*(e.module)).import_variable ("value", Type::i64 ());
  REQUIRE ((variable).has_value ());
  auto signature
    = mirnext::ir::detail::access (*(e.module)).prototype ("signature", {{Type::i64 ()}, {}});
  REQUIRE ((signature).has_value ());
  auto failed = builder.call_indirect ((signature).value (), Operand (variable.value ().symbol ()));
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
}
TEST_CASE ("native binding rejects incompatible storage without changing module data") {
  Example e;
  const Symbol *symbol = nullptr;
  SUBCASE ("array") {
    auto data = mirnext::ir::detail::access (*(e.module))
                  .create_data ("value", InitializedData{Type::i64 (), {20, 22}});
    REQUIRE ((data).has_value ());
    symbol = &data.value ().symbol ();
  }
  SUBCASE ("wrong BSS size") {
    auto data = mirnext::ir::detail::access (*(e.module)).create_data ("value", ZeroData{4});
    REQUIRE ((data).has_value ());
    symbol = &data.value ().symbol ();
  }
  SUBCASE ("readonly storage") {
    auto data = mirnext::ir::detail::access (*(e.module))
                  .create_data ("value", InitializedData{Type::i64 (), {42}}, false);
    REQUIRE ((data).has_value ());
    symbol = &data.value ().symbol ();
  }
  SUBCASE ("function") { symbol = &e.function->symbol (); }
  SUBCASE ("function import") {
    auto imported
      = mirnext::ir::detail::access (*(e.module)).import ("value", Signature{{Type::i64 ()}, {}});
    REQUIRE ((imported).has_value ());
    symbol = &(imported).value ();
  }
  SUBCASE ("foreign owner") {
    auto other = mirnext::ir::detail::access (e.context).create_module ("other");
    REQUIRE ((other).has_value ());
    auto data = mirnext::ir::detail::access (((other)).value ()).create_data ("value", ZeroData{8});
    REQUIRE ((data).has_value ());
    symbol = &data.value ().symbol ();
  }
  REQUIRE (symbol);
  auto revision = e.module->data_revision ();
  auto items = e.module->items ().size ();
  CHECK_FALSE (
    (mirnext::ir::detail::access (*(e.module)).bind_variable (*symbol, Type::i64 ())).has_value ());
  CHECK_FALSE ((symbol->variable ()).has_value ());
  CHECK (e.module->items ().size () == items);
  CHECK (e.module->data_revision () == revision);
}
TEST_CASE ("native expression variables reject illegal initializers before publishing storage") {
  Example e;
  InstructionBuilder builder (*e.block);
  SUBCASE ("memory access") {
    auto value = builder.load (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ()));
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
  }
  SUBCASE ("call") {
    auto imported
      = mirnext::ir::detail::access (*(e.module)).import ("target", Signature{{Type::i64 ()}, {}});
    REQUIRE ((imported).has_value ());
    auto call = builder.call ((imported).value ());
    REQUIRE ((call).has_value ());
    auto value = call.value ().value ();
    REQUIRE ((value).has_value ());
    REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
  }
  SUBCASE ("foreign function") {
    auto other = mirnext::ir::detail::access (e.context).create_module ("other");
    REQUIRE ((other).has_value ());
    auto function = mirnext::ir::detail::access (((other)).value ())
                      .create_function ("expression", {{Type::i64 ()}, {}});
    REQUIRE ((function).has_value ());
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    REQUIRE ((block).has_value ());
    REQUIRE ((InstructionBuilder ((block).value ()).ret (42)).has_value ());
    auto items = e.module->items ().size ();
    CHECK_FALSE (
      (mirnext::ir::detail::access (*(e.module)).create_variable ("bad", (function).value ()))
        .has_value ());
    CHECK (e.module->items ().size () == items);
    CHECK (e.module->find_symbol ("bad").get () == nullptr);
    return;
  }
  auto items = e.module->items ().size ();
  auto revision = e.module->data_revision ();
  CHECK_FALSE (
    (mirnext::ir::detail::access (*(e.module)).create_variable ("bad", *e.function)).has_value ());
  CHECK (e.module->items ().size () == items);
  CHECK (e.module->data_revision () == revision);
  CHECK (e.module->find_symbol ("bad").get () == nullptr);
}
TEST_CASE ("native variable access selects exact memory types and move opcodes for every scalar") {
  Example e;
  InstructionBuilder builder (*e.block);
  for (unsigned n = 0; n <= static_cast<unsigned> (Type::Kind::Pointer); ++n) {
    auto type = Type (static_cast<Type::Kind> (n));
    Operand initial = type.kind () == Type::Kind::F32          ? Operand (42.0f)
                      : type.kind () == Type::Kind::F64        ? Operand (42.0)
                      : type.kind () == Type::Kind::LongDouble ? Operand (42.0L)
                                                               : Operand (42);
    auto variable = mirnext::ir::detail::access (*(e.module))
                      .create_variable ("v" + std::to_string (n), type, initial);
    REQUIRE ((variable).has_value ());
    auto loaded = builder.load ((variable).value ());
    REQUIRE ((loaded).has_value ());
    CHECK (loaded.value ().type () == type);
    auto stored = builder.store ((variable).value (), Operand ((loaded).value ()));
    REQUIRE ((stored).has_value ());
    auto opcode = type.kind () == Type::Kind::F32          ? Opcode::FMov
                  : type.kind () == Type::Kind::F64        ? Opcode::DMov
                  : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
                                                           : Opcode::Mov;
    CHECK (stored.value ().opcode () == opcode);
    REQUIRE ((stored.value ().operands ()[0].get<Memory> ()).has_value ());
    CHECK (stored.value ().operands ()[0].get<Memory> ().value ().type == type);
  }
  REQUIRE ((builder.ret (42)).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto bytes = serialization::write_binary (e.context);
  REQUIRE ((bytes).has_value ());
  CHECK ((serialization::read_binary ((bytes).value ())).has_value ());
}
