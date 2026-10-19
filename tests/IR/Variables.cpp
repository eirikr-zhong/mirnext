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
  Context context;
  Module *module;
  Function *function;
  BasicBlock *block;
  Example () {
    auto m = context.create_module ("variables");
    REQUIRE (m);
    module = *m;
    auto f = module->create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE (f);
    function = *f;
    auto b = function->create_block ();
    REQUIRE (b);
    block = *b;
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
                    test::VariableExample::Expression, test::VariableExample::CrossModule,
                    test::VariableExample::Narrow}) {
    CAPTURE (static_cast<unsigned> (mode));
    auto context = test::variable_example (mode);
    auto before = dump (*context);
    auto copied = clone (*context);
    INFO ((copied ? "" : copied.error ().message));
    REQUIRE (copied);
    CHECK (dump (**copied) == before);
    auto text = serialization::write_text (*context);
    INFO ((text ? "" : text.error ().message));
    REQUIRE (text);
    auto parsed = serialization::parse_text (*text);
    REQUIRE (parsed);
    auto normalized = serialization::write_text (**parsed);
    REQUIRE (normalized);
    CHECK (*normalized == *text);
    auto binary = serialization::write_binary (*context);
    REQUIRE (binary);
    auto decoded = serialization::read_binary (*binary);
    REQUIRE (decoded);
    CHECK (dump (*context) == before);
    CHECK ((*decoded)->verify ());
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
      = e.module->create_variable ("v" + std::to_string (static_cast<unsigned> (type.kind ())),
                                   type);
    REQUIRE (variable);
    auto storage = std::get_if<ZeroData> (&(*variable)->symbol ().data ()->initializer ());
    REQUIRE (storage);
    CHECK (storage->size == size);
  }
  REQUIRE (IRBuilder (*e.block).ret (42));
  REQUIRE (e.context.verify ());
}
TEST_CASE ("native variable definitions preserve ordered data items and stable bindings") {
  Context context;
  auto module = context.create_module ("data");
  REQUIRE (module);
  auto head = (*module)->create_variable ("head", Type::i64 (), 20);
  REQUIRE (head);
  auto tail = (*module)->create_data ({}, InitializedData{Type::i64 (), {22}});
  REQUIRE (tail);
  auto zero = (*module)->create_variable ("zero", Type::i64 ());
  REQUIRE (zero);
  REQUIRE ((*module)->items ().size () == 3);
  CHECK (std::get<const GlobalData *> ((*module)->items ()[0]) == (*head)->symbol ().data ());
  CHECK (std::get<const GlobalData *> ((*module)->items ()[1]) == *tail);
  CHECK (std::get<const GlobalData *> ((*module)->items ()[2]) == (*zero)->symbol ().data ());
  auto rebound = (*module)->bind_variable ((*head)->symbol (), Type::i64 ());
  REQUIRE (rebound);
  CHECK (*rebound == *head);
  CHECK ((*module)->items ().size () == 3);
  auto copied = clone (context);
  REQUIRE (copied);
  CHECK (dump (**copied) == dump (context));
}
TEST_CASE ("native variable failures retain the first error and publish no storage") {
  Example e;
  const auto before = e.module->items ().size ();
  const auto revision = e.module->data_revision ();
  SUBCASE ("block type") { CHECK_FALSE (e.module->create_variable ("bad", Type::block (16))); }
  SUBCASE ("undef type") {
    CHECK_FALSE (e.module->create_variable ("bad", Type (Type::Kind::Undef)));
  }
  SUBCASE ("invalid type") {
    CHECK_FALSE (e.module->create_variable ("bad", Type (static_cast<Type::Kind> (99))));
  }
  SUBCASE ("empty name") { CHECK_FALSE (e.module->create_variable ({}, Type::i64 ())); }
  SUBCASE ("wrong literal type") {
    CHECK_FALSE (e.module->create_variable ("bad", Type::i64 (), 1.0));
  }
  SUBCASE ("dynamic initializer") {
    auto reg = e.function->create_register (Type::i64 ());
    REQUIRE (reg);
    CHECK_FALSE (e.module->create_variable ("bad", Type::i64 (), Operand (**reg)));
  }
  SUBCASE ("wrong boolean initializer") {
    CHECK_FALSE (e.module->create_variable ("bad", Type (Type::Kind::Bool), 1.0));
  }
  SUBCASE ("duplicate name") { CHECK_FALSE (e.module->create_variable ("entry", Type::i64 ())); }
  CHECK (e.module->items ().size () == before);
  CHECK (e.module->data_revision () == revision);
  CHECK (e.module->find_symbol ("bad") == nullptr);
  REQUIRE (e.module->error ());
  auto again = e.module->create_variable ("later", Type::i64 (), 42);
  REQUIRE_FALSE (again);
  CHECK (again.error ().message == e.module->error ()->message);
}
TEST_CASE ("native variable access failures are atomic") {
  Example e;
  IRBuilder builder (*e.block);
  auto variable = e.module->create_variable ("value", Type::i64 (), 42);
  REQUIRE (variable);
  auto revision = e.function->revisions ();
  SUBCASE ("wrong store type") { CHECK_FALSE (builder.store (**variable, 1.0)); }
  SUBCASE ("foreign variable") {
    auto other = e.context.create_module ("other");
    REQUIRE (other);
    auto foreign = (*other)->create_variable ("value", Type::i64 (), 42);
    REQUIRE (foreign);
    CHECK_FALSE (builder.load (**foreign));
  }
  SUBCASE ("foreign store value") {
    auto other = e.module->create_function ("other", {});
    REQUIRE (other);
    auto foreign = (*other)->create_register (Type::i64 ());
    REQUIRE (foreign);
    CHECK_FALSE (builder.store (**variable, Operand (**foreign)));
  }
  SUBCASE ("invalid result name") {
    CHECK_FALSE (builder.load (**variable, std::string_view ("a\0b", 3)));
  }
  SUBCASE ("closed insertion point") {
    builder.clear_insert_point ();
    CHECK_FALSE (builder.load (**variable));
  }
  SUBCASE ("duplicate result name") {
    REQUIRE (e.function->create_register (Type::i64 (), "existing"));
    auto count = register_count (*e.function);
    auto before = e.function->revisions ();
    CHECK_FALSE (builder.load (**variable, "existing"));
    CHECK (register_count (*e.function) == count);
    CHECK (e.function->revisions () == before);
    CHECK (e.block->size () == 0);
    return;
  }
  SUBCASE ("terminated block") {
    REQUIRE (builder.ret (42));
    auto before = e.function->revisions ();
    CHECK_FALSE (builder.load (**variable));
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
  IRBuilder builder (*e.block);
  auto constant = e.module->create_variable ("constant", Type::i64 (), 42, false);
  REQUIRE (constant);
  auto value = builder.load (**constant);
  REQUIRE (value);
  auto count = register_count (*e.function);
  auto instructions = e.block->size ();
  auto revision = e.function->revisions ();
  CHECK_FALSE (builder.store (**constant, 0));
  CHECK (register_count (*e.function) == count);
  CHECK (e.block->size () == instructions);
  CHECK (e.function->revisions () == revision);
}
TEST_CASE ("native boolean variable initialization and access normalize integer values") {
  Example e;
  auto variable = e.module->create_variable ("flag", Type (Type::Kind::Bool), -7);
  REQUIRE (variable);
  CHECK (*std::get<InitializedData> ((*variable)->symbol ().data ()->initializer ())
            .values[0]
            .get<std::int64_t> ()
         == 1);
  IRBuilder builder (*e.block);
  REQUIRE (builder.store (**variable, UINT64_MAX));
  CHECK (e.block->size () == 2);
  CHECK (register_count (*e.function) == 1);
  auto dynamic = builder.binary (Opcode::Add, 40, 2);
  REQUIRE (dynamic);
  REQUIRE (builder.store (**variable, Operand (**dynamic)));
  auto loaded = builder.load (**variable, "flag");
  REQUIRE (loaded);
  CHECK ((*loaded)->type ().kind () == Type::Kind::Bool);
  REQUIRE (builder.ret (Operand (**loaded)));
  REQUIRE (e.context.verify ());
  unsigned comparisons = 0;
  for (auto &instruction : e.block->instructions ())
    if (instruction.opcode () == Opcode::Ne) ++comparisons;
  CHECK (comparisons == 2);
  auto bytes = serialization::write_binary (e.context);
  REQUIRE (bytes);
  CHECK (serialization::read_binary (*bytes));
}
TEST_CASE ("native bindings validate existing storage and recover typed views after parsing") {
  auto parsed = serialization::parse_text ("data: module\nvalue: i64 42\nzero: bss 8\nendmodule\n");
  REQUIRE (parsed);
  auto &module = *(*parsed)->modules ().begin ();
  auto symbol = module.find_symbol ("value");
  REQUIRE (symbol);
  auto bound = module.bind_variable (*symbol, Type::i64 ());
  REQUIRE (bound);
  CHECK ((*bound)->symbol ().data ());
  auto zero = module.bind_variable (*module.find_symbol ("zero"), Type::pointer ());
  REQUIRE (zero);
  CHECK ((*parsed)->verify ());
  SUBCASE ("wrong width") { CHECK_FALSE (module.bind_variable (*symbol, Type (Type::Kind::I32))); }
  SUBCASE ("same binding") {
    auto repeated = module.bind_variable (*symbol, Type::i64 ());
    REQUIRE (repeated);
    CHECK (*repeated == *bound);
  }
}
TEST_CASE ("native variable import checks ignore module creation order") {
  for (bool definition_first : {false, true})
    for (unsigned mode = 0; mode < 6; ++mode) {
      CAPTURE (definition_first);
      CAPTURE (mode);
      Context context;
      auto first = context.create_module ("first");
      REQUIRE (first);
      auto second = context.create_module ("second");
      REQUIRE (second);
      auto definition = definition_first ? *first : *second;
      auto consumer = definition_first ? *second : *first;
      auto variable = definition->create_variable ("value", Type::i64 (), 42, false);
      REQUIRE (variable);
      REQUIRE (definition->export_symbol ((*variable)->symbol ()));
      if (mode == 0) {
        REQUIRE (consumer->import_variable ("value", Type::i64 (), false));
        CHECK (context.verify ());
      } else if (mode == 1) {
        REQUIRE (consumer->import_variable ("value", Type::i64 ()));
        CHECK_FALSE (context.verify ());
      } else if (mode == 2) {
        REQUIRE (consumer->import_variable ("value", Type (Type::Kind::F64), false));
        CHECK_FALSE (context.verify ());
      } else if (mode == 3) {
        auto raw = definition->create_data ("raw", InitializedData{Type::i64 (), {42}});
        REQUIRE (raw);
        REQUIRE (definition->export_symbol ((*raw)->symbol ()));
        REQUIRE (consumer->import_variable ("raw", Type::i64 ()));
        CHECK (context.verify ());
      } else if (mode == 4) {
        auto function = definition->create_function ("fn", {{Type::i64 ()}, {}});
        REQUIRE (function);
        auto block = (*function)->create_block ();
        REQUIRE (block);
        REQUIRE (IRBuilder (**block).ret (42));
        REQUIRE (definition->export_symbol ((*function)->symbol ()));
        REQUIRE (consumer->import_variable ("fn", Type::i64 ()));
        CHECK_FALSE (context.verify ());
      } else {
        REQUIRE (consumer->import_variable ("host", Type::i64 ()));
        CHECK (context.verify ());
      }
    }
}
TEST_CASE ("native cloned variables own their metadata independently of the source") {
  std::unique_ptr<Context> copied;
  {
    auto source = test::variable_example (test::VariableExample::CrossModule);
    auto clone_result = clone (*source);
    REQUIRE (clone_result);
    copied = std::move (*clone_result);
  }
  auto &consumer = *copied->modules ().begin ();
  auto variable = consumer.find_variable ("value");
  REQUIRE (variable);
  auto function = consumer.create_function ("another", {{Type::i64 ()}, {}});
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  REQUIRE (builder.store (*variable, 42));
  auto result = builder.load (*variable);
  REQUIRE (result);
  REQUIRE (builder.ret (Operand (**result)));
  CHECK (copied->verify ());
  CHECK (serialization::write_binary (*copied));
}
TEST_CASE ("native variable symbols cannot be called as functions") {
  Example e;
  IRBuilder builder (*e.block);
  auto variable = e.module->import_variable ("value", Type::i64 ());
  REQUIRE (variable);
  auto signature = e.module->prototype ("signature", {{Type::i64 ()}, {}});
  REQUIRE (signature);
  auto failed = builder.call_indirect (**signature, Operand ((*variable)->symbol ()));
  REQUIRE_FALSE (failed);
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
}
TEST_CASE ("native binding rejects incompatible storage without changing module data") {
  Example e;
  const Symbol *symbol = nullptr;
  SUBCASE ("array") {
    auto data = e.module->create_data ("value", InitializedData{Type::i64 (), {20, 22}});
    REQUIRE (data);
    symbol = &(*data)->symbol ();
  }
  SUBCASE ("wrong BSS size") {
    auto data = e.module->create_data ("value", ZeroData{4});
    REQUIRE (data);
    symbol = &(*data)->symbol ();
  }
  SUBCASE ("readonly storage") {
    auto data = e.module->create_data ("value", InitializedData{Type::i64 (), {42}}, false);
    REQUIRE (data);
    symbol = &(*data)->symbol ();
  }
  SUBCASE ("function") { symbol = &e.function->symbol (); }
  SUBCASE ("function import") {
    auto imported = e.module->import ("value", Signature{{Type::i64 ()}, {}});
    REQUIRE (imported);
    symbol = *imported;
  }
  SUBCASE ("foreign owner") {
    auto other = e.context.create_module ("other");
    REQUIRE (other);
    auto data = (*other)->create_data ("value", ZeroData{8});
    REQUIRE (data);
    symbol = &(*data)->symbol ();
  }
  REQUIRE (symbol);
  auto revision = e.module->data_revision ();
  auto items = e.module->items ().size ();
  CHECK_FALSE (e.module->bind_variable (*symbol, Type::i64 ()));
  CHECK_FALSE (symbol->variable ());
  CHECK (e.module->items ().size () == items);
  CHECK (e.module->data_revision () == revision);
}
TEST_CASE ("native expression variables reject illegal initializers before publishing storage") {
  Example e;
  IRBuilder builder (*e.block);
  SUBCASE ("memory access") {
    auto value = builder.load (Memory{Type::i64 ()});
    REQUIRE (value);
    REQUIRE (builder.ret (Operand (**value)));
  }
  SUBCASE ("call") {
    auto imported = e.module->import ("target", Signature{{Type::i64 ()}, {}});
    REQUIRE (imported);
    auto call = builder.call (**imported);
    REQUIRE (call);
    auto value = call->value ();
    REQUIRE (value);
    REQUIRE (builder.ret (Operand (**value)));
  }
  SUBCASE ("foreign function") {
    auto other = e.context.create_module ("other");
    REQUIRE (other);
    auto function = (*other)->create_function ("expression", {{Type::i64 ()}, {}});
    REQUIRE (function);
    auto block = (*function)->create_block ();
    REQUIRE (block);
    REQUIRE (IRBuilder (**block).ret (42));
    auto items = e.module->items ().size ();
    CHECK_FALSE (e.module->create_variable ("bad", **function));
    CHECK (e.module->items ().size () == items);
    CHECK (e.module->find_symbol ("bad") == nullptr);
    return;
  }
  auto items = e.module->items ().size ();
  auto revision = e.module->data_revision ();
  CHECK_FALSE (e.module->create_variable ("bad", *e.function));
  CHECK (e.module->items ().size () == items);
  CHECK (e.module->data_revision () == revision);
  CHECK (e.module->find_symbol ("bad") == nullptr);
}
TEST_CASE ("native variable access selects exact memory types and move opcodes for every scalar") {
  Example e;
  IRBuilder builder (*e.block);
  for (unsigned n = 0; n <= static_cast<unsigned> (Type::Kind::Pointer); ++n) {
    auto type = Type (static_cast<Type::Kind> (n));
    Operand initial = type.kind () == Type::Kind::F32          ? Operand (42.0f)
                      : type.kind () == Type::Kind::F64        ? Operand (42.0)
                      : type.kind () == Type::Kind::LongDouble ? Operand (42.0L)
                                                               : Operand (42);
    auto variable = e.module->create_variable ("v" + std::to_string (n), type, initial);
    REQUIRE (variable);
    auto loaded = builder.load (**variable);
    REQUIRE (loaded);
    CHECK ((*loaded)->type () == type);
    auto stored = builder.store (**variable, Operand (**loaded));
    REQUIRE (stored);
    auto opcode = type.kind () == Type::Kind::F32          ? Opcode::FMov
                  : type.kind () == Type::Kind::F64        ? Opcode::DMov
                  : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
                                                           : Opcode::Mov;
    CHECK ((*stored)->opcode () == opcode);
    REQUIRE ((*stored)->operands ()[0].get<Memory> ());
    CHECK ((*stored)->operands ()[0].get<Memory> ()->type == type);
  }
  REQUIRE (builder.ret (42));
  REQUIRE (e.context.verify ());
  auto bytes = serialization::write_binary (e.context);
  REQUIRE (bytes);
  CHECK (serialization::read_binary (*bytes));
}
