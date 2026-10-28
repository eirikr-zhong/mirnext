/* Licensed under the MIT License. Native call construction and protocol validation. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "Calls.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *block;
  Example () {
    auto m = mirnext::ir::detail::access (context).create_module ("calls");
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
std::size_t symbol_count (const Module &module) {
  return static_cast<std::size_t> (std::ranges::distance (module.symbols ()));
}
}  // namespace
TEST_CASE ("native call DSL formats and clones every ordinary call form") {
  for (auto mode : {test::CallExample::Direct, test::CallExample::Indirect,
                    test::CallExample::Block, test::CallExample::ReturnBlock,
                    test::CallExample::Variadic, test::CallExample::VariadicBlock,
                    test::CallExample::MultipleResults, test::CallExample::Inline}) {
    CAPTURE (static_cast<unsigned> (mode));
    auto context = test::call_example (mode);
    auto source = dump (*context);
    auto copied = clone (*context);
    REQUIRE ((copied).has_value ());
    CHECK (dump ((copied).value ()) == source);
    auto text = serialization::write_text (*context);
    INFO (((text).has_value () ? "" : text.error ().message));
    REQUIRE ((text).has_value ());
    auto parsed = serialization::parse_text ((text).value ());
    INFO (((parsed).has_value () ? "" : parsed.error ().message));
    REQUIRE ((parsed).has_value ());
    auto normalized = serialization::write_text ((parsed).value ());
    REQUIRE ((normalized).has_value ());
    CHECK ((normalized).value () == (text).value ());
    auto bytes = serialization::write_binary (*context);
    REQUIRE ((bytes).has_value ());
    auto decoded = serialization::read_binary ((bytes).value ());
    INFO (((decoded).has_value () ? "" : decoded.error ().message));
    REQUIRE ((decoded).has_value ());
    CHECK (dump (*context) == source);
    CHECK (needs_materialization (*context));
    CHECK_FALSE (needs_materialization ((decoded).value ()));
  }
}
TEST_CASE ("native invalid calls publish neither registers nor instructions") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target
    = mirnext::ir::detail::access (*(e.module))
        .import ("target", Signature{{Type::i64 (), Type (Type::Kind::F64)}, {Type::block (16)}});
  REQUIRE ((target).has_value ());
  auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
  REQUIRE ((address).has_value ());
  auto before = e.function->revisions ();
  auto registers = register_count (*e.function);
  auto symbols = symbol_count (*e.module);
  auto failed = builder.call ((target).value (),
                              {mirnext::ir::detail::BlockArgumentState ((address).value (), 8)});
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("block argument size") != std::string::npos);
  CHECK (register_count (*e.function) == registers);
  CHECK (symbol_count (*e.module) == symbols);
  CHECK (e.block->size () == 0);
  CHECK (e.function->revisions () == before);
  auto repeated = builder.call ((target).value (),
                                {mirnext::ir::detail::BlockArgumentState ((address).value (), 16)});
  REQUIRE_FALSE ((repeated).has_value ());
  CHECK (repeated.error ().message == failed.error ().message);
}
TEST_CASE ("native call DSL rejects ownership types and counts before publishing") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((target).has_value ());
  const auto before = e.function->revisions ();
  SUBCASE ("missing argument") { CHECK_FALSE ((builder.call ((target).value ())).has_value ()); }
  SUBCASE ("extra argument") {
    CHECK_FALSE (
      (builder.call ((target).value (), {std::int64_t{1}, std::int64_t{2}})).has_value ());
  }
  SUBCASE ("wrong scalar type") {
    CHECK_FALSE ((builder.call ((target).value (), {1.0})).has_value ());
  }
  SUBCASE ("mirnext::ir::detail::BlockArgumentState cannot masquerade as scalar") {
    auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
    REQUIRE ((reg).has_value ());
    auto count = register_count (*e.function);
    CHECK_FALSE (
      (builder.call ((target).value (),
                     {mirnext::ir::detail::BlockArgumentState ((reg).value (), Type::i64 ())}))
        .has_value ());
    CHECK (register_count (*e.function) == count);
    return;
  }
  SUBCASE ("foreign signature symbol") {
    auto other = mirnext::ir::detail::access (e.context).create_module ("other");
    REQUIRE ((other).has_value ());
    auto foreign = mirnext::ir::detail::access (((other)).value ())
                     .import ("target", *target.value ().signature ().get ());
    REQUIRE ((foreign).has_value ());
    CHECK_FALSE ((builder.call ((foreign).value (), {std::int64_t{1}})).has_value ());
  }
  SUBCASE ("foreign argument register") {
    auto other = mirnext::ir::detail::access (*(e.module)).create_function ("other", {});
    REQUIRE ((other).has_value ());
    auto reg = mirnext::ir::detail::access (((other)).value ()).create_register (Type::i64 ());
    REQUIRE ((reg).has_value ());
    CHECK_FALSE ((builder.call ((target).value (), {(reg).value ()})).has_value ());
  }
  SUBCASE ("untyped import") {
    auto untyped = mirnext::ir::detail::access (*(e.module)).import ("untyped");
    REQUIRE ((untyped).has_value ());
    CHECK_FALSE ((builder.call ((untyped).value ())).has_value ());
  }
  SUBCASE ("noncallable prototype") {
    auto proto = mirnext::ir::detail::access (*(e.module))
                   .prototype ("proto", *target.value ().signature ().get ());
    REQUIRE ((proto).has_value ());
    CHECK_FALSE ((builder.call ((proto).value (), {std::int64_t{1}})).has_value ());
  }
  SUBCASE ("closed insertion point") {
    builder.clear_insert_point ();
    CHECK_FALSE ((builder.call ((target).value (), {std::int64_t{1}})).has_value ());
  }
  CHECK (e.function->revisions () == before);
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
}
TEST_CASE ("native block arguments retain ABI class size address and displacement") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto address = builder.allocate_stack (Operand (std::int64_t{32}));
  REQUIRE ((address).has_value ());
  for (unsigned n = 0; n <= 4; ++n) {
    auto type = Type::block (16, static_cast<Type::BlockClass> (n));
    auto target = mirnext::ir::detail::access (*(e.module))
                    .import ("block" + std::to_string (n), Signature{{}, {type}});
    REQUIRE ((target).has_value ());
    auto call
      = builder.call ((target).value (),
                      {mirnext::ir::detail::BlockArgumentState ((address).value (), type, 8)});
    REQUIRE ((call).has_value ());
    auto memory = mirnext::ir::detail::operand_access (call.value ().instruction ().operands ()[2])
                    .get<Memory> ();
    REQUIRE (memory);
    CHECK (memory->type == type);
    CHECK (mirnext::ir::detail::NodeAccess::base (*(memory)) == &(address).value ());
    CHECK (memory->displacement == 8);
    CHECK (call.value ().results ().empty ());
    CHECK_FALSE ((call.value ().value ()).has_value ());
  }
  REQUIRE ((builder.ret (Operand (std::int64_t{42}))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto binary = serialization::write_binary (e.context);
  REQUIRE ((binary).has_value ());
  auto decoded = serialization::read_binary ((binary).value ());
  REQUIRE ((decoded).has_value ());
  CHECK ((decoded.value ().verify ()).has_value ());
}
TEST_CASE ("native variadic calls require promotion and prohibit unnamed return blocks") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("variadic", Signature{{Type::i64 ()}, {Type::i64 ()}, true});
  REQUIRE ((target).has_value ());
  SUBCASE ("float") {
    CHECK_FALSE ((builder.call ((target).value (), {std::int64_t{0}, 1.0f})).has_value ());
  }
  SUBCASE ("return block") {
    auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
    REQUIRE ((address).has_value ());
    auto count = register_count (*e.function);
    CHECK_FALSE (
      (builder.call ((target).value (),
                     {std::int64_t{0},
                      mirnext::ir::detail::BlockArgumentState::return_buffer ((address).value (),
                                                                              16)}))
        .has_value ());
    CHECK (register_count (*e.function) == count);
  }
  SUBCASE ("empty block") {
    auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
    REQUIRE ((address).has_value ());
    CHECK_FALSE ((builder.call ((target).value (),
                                {std::int64_t{0},
                                 mirnext::ir::detail::BlockArgumentState ((address).value (), 0)}))
                   .has_value ());
  }
  SUBCASE ("promoted double") {
    auto call = builder.call ((target).value (), {std::int64_t{0}, 1.0});
    REQUIRE ((call).has_value ());
    CHECK (call.value ().results ().size () == 1);
    return;
  }
  CHECK (e.block->size () == 0);
}
TEST_CASE (
  "native forward and imported call results retain identity across definitions and clones") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("definitions");
  REQUIRE ((module).has_value ());
  Signature signature{{Type::i64 ()}, {Type::i64 ()}};
  auto declared
    = mirnext::ir::detail::access (((module)).value ()).declare_function ("callee", signature);
  REQUIRE ((declared).has_value ());
  auto caller = mirnext::ir::detail::access (((module)).value ())
                  .create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE ((caller).has_value ());
  auto block = mirnext::ir::detail::access (((caller)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  auto call = builder.call ((declared).value (), {std::int64_t{42}});
  REQUIRE ((call).has_value ());
  auto value = call.value ().value ();
  REQUIRE ((value).has_value ());
  REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
  auto defined
    = mirnext::ir::detail::access (((module)).value ()).define_function ((declared).value ());
  REQUIRE ((defined).has_value ());
  auto body = mirnext::ir::detail::access (((defined)).value ()).create_block ();
  REQUIRE ((body).has_value ());
  REQUIRE ((InstructionBuilder ((body).value ())
              .ret (Operand (*mirnext::ir::detail::ReferenceAccess::pointers (
                defined.value ().arguments ())[0])))
             .has_value ());
  REQUIRE ((mirnext::ir::detail::access (((module)).value ()).export_symbol ((declared).value ()))
             .has_value ());
  CHECK (mirnext::ir::detail::NodeAccess::symbol (
           ((call.value ().instruction ().operands ()[1].get<SymbolReference> ())).value ())
         == &(declared).value ());
  auto other = mirnext::ir::detail::access (context).create_module ("consumer");
  REQUIRE ((other).has_value ());
  auto imported = mirnext::ir::detail::access (((other)).value ()).import ("callee", signature);
  REQUIRE ((imported).has_value ());
  auto consumer = mirnext::ir::detail::access (((other)).value ())
                    .create_function ("consumer", {{Type::i64 ()}, {}});
  REQUIRE ((consumer).has_value ());
  auto entry = mirnext::ir::detail::access (((consumer)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder consuming ((entry).value ());
  auto result = consuming.call ((imported).value (), {std::int64_t{42}});
  REQUIRE ((result).has_value ());
  auto scalar = result.value ().value ();
  REQUIRE ((scalar).has_value ());
  REQUIRE ((consuming.ret (Operand ((scalar).value ()))).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  auto binary = serialization::write_binary (context);
  REQUIRE ((binary).has_value ());
  auto decoded = serialization::read_binary ((binary).value ());
  REQUIRE ((decoded).has_value ());
  auto copied = clone (context);
  REQUIRE ((copied).has_value ());
  CHECK (dump ((copied).value ()) == dump (context));
}
TEST_CASE ("native jump calls use a separate void convention") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("jump");
  REQUIRE ((module).has_value ());
  auto target = mirnext::ir::detail::access (((module)).value ()).create_function ("target", {});
  REQUIRE ((target).has_value ());
  auto body = mirnext::ir::detail::access (((target)).value ()).create_block ();
  REQUIRE ((body).has_value ());
  REQUIRE ((InstructionBuilder ((body).value ()).ret_to (Operand (std::int64_t{0}))).has_value ());
  auto caller = mirnext::ir::detail::access (((module)).value ()).create_function ("caller", {});
  REQUIRE ((caller).has_value ());
  auto entry = mirnext::ir::detail::access (((caller)).value ()).create_block ();
  REQUIRE ((entry).has_value ());
  InstructionBuilder builder ((entry).value ());
  auto call = builder.jump_call (target.value ().symbol ());
  REQUIRE ((call).has_value ());
  CHECK (call.value ().opcode () == Opcode::JCall);
  REQUIRE ((context.verify ()).has_value ());
  SUBCASE ("ordinary call convention rejected") {
    REQUIRE ((InstructionRewriter ((caller).value ())
                .replace ((call).value (), Opcode::Call, call.value ().operands ()))
               .has_value ());
    REQUIRE ((builder.ret ()).has_value ());
    CHECK_FALSE ((context.verify ()).has_value ());
  }
  SUBCASE ("nonvoid jump signature rejected") {
    auto invalid = mirnext::ir::detail::access (((module)).value ())
                     .import ("invalid", Signature{{Type::i64 ()}, {}});
    REQUIRE ((invalid).has_value ());
    auto other = mirnext::ir::detail::access (((caller)).value ()).create_block ();
    REQUIRE ((other).has_value ());
    REQUIRE ((builder.set_insert_point ((other).value ())).has_value ());
    auto failed = builder.jump_call ((invalid).value ());
    REQUIRE_FALSE ((failed).has_value ());
    CHECK (other.value ().size () == 0);
  }
}
TEST_CASE ("native va_arg helpers return values and checked address operations") {
  Program context;
  auto module = mirnext::ir::detail::access (context).create_module ("va");
  REQUIRE ((module).has_value ());
  auto function
    = mirnext::ir::detail::access (((module)).value ())
        .create_function ("sum", {{Type::i64 ()}, {Type::i64 (), Type::block (16)}, true});
  REQUIRE ((function).has_value ());
  auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
  REQUIRE ((block).has_value ());
  InstructionBuilder builder ((block).value ());
  auto list = builder.allocate_stack (Operand (std::int64_t{64}));
  REQUIRE ((list).has_value ());
  REQUIRE ((builder.start_varargs (Operand ((list).value ()))).has_value ());
  auto value = builder.next_vararg (Type (Type::Kind::F64), Operand ((list).value ()), "value");
  REQUIRE ((value).has_value ());
  CHECK (value.value ().type () == Type (Type::Kind::F64));
  auto address = builder.next_vararg_address (Type::i64 (), Operand ((list).value ()));
  REQUIRE ((address).has_value ());
  CHECK (address.value ().type () == Type::pointer ());
  auto destination = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE ((destination).has_value ());
  for (unsigned n = 0; n <= 4; ++n)
    REQUIRE ((builder.next_vararg_block ((destination).value (), Operand ((list).value ()),
                                         Type::block (16, static_cast<Type::BlockClass> (n))))
               .has_value ());
  REQUIRE ((builder.next_vararg_block (*mirnext::ir::detail::ReferenceAccess::pointers (
                                         function.value ().arguments ())[1],
                                       Operand ((list).value ()), Type::block (16)))
             .has_value ());
  REQUIRE ((builder.end_varargs (Operand ((list).value ()))).has_value ());
  REQUIRE ((builder.ret (Operand (std::int64_t{0}))).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  auto binary = serialization::write_binary (context);
  REQUIRE ((binary).has_value ());
  CHECK ((serialization::read_binary ((binary).value ())).has_value ());
}
TEST_CASE ("native invalid variadic helpers are atomic and retain the first diagnostic") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto list
    = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer (), "list");
  REQUIRE ((list).has_value ());
  auto before = e.function->revisions ();
  auto count = register_count (*e.function);
  SUBCASE ("start requires varargs") {
    CHECK_FALSE ((builder.start_varargs (Operand ((list).value ()))).has_value ());
  }
  SUBCASE ("block scalar type") {
    CHECK_FALSE (
      (builder.next_vararg_block ((list).value (), Operand ((list).value ()), Type::i64 ()))
        .has_value ());
  }
  SUBCASE ("block return buffer") {
    CHECK_FALSE ((builder.next_vararg_block ((list).value (), Operand ((list).value ()),
                                             Type::return_block (16)))
                   .has_value ());
  }
  SUBCASE ("block empty") {
    CHECK_FALSE (
      (builder.next_vararg_block ((list).value (), Operand ((list).value ()), Type::block (0)))
        .has_value ());
  }
  SUBCASE ("invalid ABI class") {
    CHECK_FALSE ((builder.next_vararg_block ((list).value (), Operand ((list).value ()),
                                             Type::block (16, static_cast<Type::BlockClass> (5))))
                   .has_value ());
  }
  SUBCASE ("block size exceeds protocol range") {
    CHECK_FALSE ((builder.next_vararg_block ((list).value (), Operand ((list).value ()),
                                             Type::block (static_cast<std::size_t> (UINT64_MAX))))
                   .has_value ());
  }
  SUBCASE ("va_arg block") {
    CHECK_FALSE ((builder.next_vararg (Type::block (16), Operand ((list).value ()))).has_value ());
  }
  SUBCASE ("va_arg undef") {
    CHECK_FALSE (
      (builder.next_vararg (Type (Type::Kind::Undef), Operand ((list).value ()))).has_value ());
  }
  SUBCASE ("va_arg duplicate name") {
    CHECK_FALSE (
      (builder.next_vararg (Type::i64 (), Operand ((list).value ()), "list")).has_value ());
  }
  SUBCASE ("va_arg foreign list") {
    auto other = mirnext::ir::detail::access (*(e.module)).create_function ("other", {});
    REQUIRE ((other).has_value ());
    auto foreign
      = mirnext::ir::detail::access (((other)).value ()).create_register (Type::pointer ());
    REQUIRE ((foreign).has_value ());
    CHECK_FALSE ((builder.next_vararg (Type::i64 (), Operand ((foreign).value ()))).has_value ());
  }
  CHECK (register_count (*e.function) == count);
  CHECK (e.block->size () == 0);
  CHECK (e.function->revisions () == before);
}
TEST_CASE ("native indirect memory targets use CALL with typed multiple results") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto prototype = mirnext::ir::detail::access (*(e.module))
                     .prototype ("signature", Signature{{Type::i64 (), Type (Type::Kind::F64)},
                                                        {Type (Type::Kind::F64)}});
  REQUIRE ((prototype).has_value ());
  auto pointer = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
  REQUIRE ((pointer).has_value ());
  auto call
    = builder.call_indirect ((prototype).value (),
                             Operand (
                               mirnext::ir::detail::NodeAccess::make_Memory (Type::pointer (),
                                                                             &(pointer).value ())),
                             {1.0});
  REQUIRE ((call).has_value ());
  CHECK (call.value ().instruction ().opcode () == Opcode::Call);
  REQUIRE (call.value ().results ().size () == 2);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (call.value ().results ())[0]->type ()
         == Type::i64 ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (call.value ().results ())[1]->type ()
         == Type (Type::Kind::F64));
  CHECK ((call.value ().instruction ().operands ()[1].get<Memory> ()).has_value ());
  CHECK (output_operand (*e.function, call.value ().instruction (), 2));
  CHECK (output_operand (*e.function, call.value ().instruction (), 3));
  CHECK_FALSE (output_operand (*e.function, call.value ().instruction (), 4));
  REQUIRE ((builder.ret (Operand (
              *mirnext::ir::detail::ReferenceAccess::pointers (call.value ().results ())[0])))
             .has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto text = serialization::write_text (e.context);
  REQUIRE ((text).has_value ());
  CHECK ((serialization::parse_text ((text).value ())).has_value ());
  auto binary = serialization::write_binary (e.context);
  REQUIRE ((binary).has_value ());
  CHECK ((serialization::read_binary ((binary).value ())).has_value ());
}
TEST_CASE ("native call result handles remain stable through bulk construction and edits") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((target).has_value ());
  auto first = builder.call ((target).value (), {std::int64_t{42}});
  REQUIRE ((first).has_value ());
  auto first_value = first.value ().value ();
  REQUIRE ((first_value).has_value ());
  auto instruction = &first.value ().instruction ();
  auto value = &(first_value).value ();
  for (unsigned n = 0; n < 4096; ++n)
    REQUIRE ((builder.call ((target).value (), {*value})).has_value ());
  CHECK (&first.value ().instruction () == instruction);
  CHECK (&(first.value ().value ()).value () == value);
  CHECK (register_count (*e.function) == 4097);
  CHECK (e.function->instruction_count () == 4097);
  REQUIRE ((builder.ret (Operand (*value))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto copied = clone (e.context);
  REQUIRE ((copied).has_value ());
  CHECK (dump ((copied).value ()) == dump (e.context));
  auto bytes = serialization::write_binary (e.context);
  REQUIRE ((bytes).has_value ());
  auto decoded = serialization::read_binary ((bytes).value ());
  REQUIRE ((decoded).has_value ());
  CHECK (decoded.value ().modules ().begin ()->functions ().begin ()->instruction_count () == 4098);
}
TEST_CASE ("native mismatched indirect signatures fail without publishing results") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto signature = mirnext::ir::detail::access (*(e.module))
                     .prototype ("signature", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((signature).has_value ());
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type (Type::Kind::F64)}, {Type::i64 ()}});
  REQUIRE ((target).has_value ());
  auto before = e.function->revisions ();
  auto call
    = builder.call_indirect ((signature).value (), Operand ((target).value ()), {std::int64_t{42}});
  REQUIRE_FALSE ((call).has_value ());
  CHECK (call.error ().message.find ("callee signature mismatch") != std::string::npos);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
  CHECK (e.block->size () == 0);
}
TEST_CASE ("native integral literals preserve sign width and boolean values") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}});
  REQUIRE ((target).has_value ());
  auto call = builder.call ((target).value (), {20, 22});
  REQUIRE ((call).has_value ());
  CHECK ((call.value ().instruction ().operands ()[3].get<std::int64_t> ()).value () == 20);
  CHECK ((call.value ().instruction ().operands ()[4].get<std::int64_t> ()).value () == 22);
  CHECK ((Operand (-1).get<std::int64_t> ()).value () == -1);
  CHECK ((Operand (UINT64_MAX).get<std::uint64_t> ()).value () == UINT64_MAX);
  CHECK ((Operand (true).get<std::int64_t> ()).value () == 1);
  CHECK ((Operand (false).get<std::int64_t> ()).value () == 0);
  CHECK ((Operand (static_cast<unsigned short> (65535)).get<std::uint64_t> ()).value () == 65535);
  auto result = call.value ().value ();
  REQUIRE ((result).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native single Operand argument lists select direct calls unambiguously") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((target).has_value ());
  auto direct = builder.call ((target).value (), {Operand (42)});
  REQUIRE ((direct).has_value ());
  const auto operands = direct.value ().instruction ().operands ();
  REQUIRE (operands.size () == 4);
  REQUIRE ((operands[1].get<SymbolReference> ()).has_value ());
  CHECK (mirnext::ir::detail::NodeAccess::symbol (((operands[1].get<SymbolReference> ())).value ())
         == &(target).value ());
  REQUIRE ((operands[3].get<std::int64_t> ()).has_value ());
  CHECK ((operands[3].get<std::int64_t> ()).value () == 42);
  auto value = direct.value ().value ();
  REQUIRE ((value).has_value ());
  auto passed = builder.call ((target).value (), {Operand ((value).value ())});
  REQUIRE ((passed).has_value ());
  auto address = builder.address_of ((target).value ());
  REQUIRE ((address).has_value ());
  auto indirect = builder.call_indirect ((target).value (), Operand ((address).value ()),
                                         {Operand ((value).value ())});
  REQUIRE ((indirect).has_value ());
  CHECK (mirnext::ir::detail::operand_access (indirect.value ().instruction ().operands ()[1])
           .get<const Register *> ());
  auto no_arguments = mirnext::ir::detail::access (*(e.module))
                        .import ("no_arguments", Signature{{Type::i64 ()}, {}});
  REQUIRE ((no_arguments).has_value ());
  auto empty_address = builder.address_of ((no_arguments).value ());
  REQUIRE ((empty_address).has_value ());
  auto empty = builder.call_indirect ((no_arguments).value (), Operand ((empty_address).value ()));
  REQUIRE ((empty).has_value ());
  CHECK (empty.value ().instruction ().operands ().size () == 3);
  auto result = indirect.value ().value ();
  REQUIRE ((result).has_value ());
  REQUIRE ((builder.ret (Operand ((result).value ()))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto text = serialization::write_text (e.context);
  REQUIRE ((text).has_value ());
  CHECK ((serialization::parse_text ((text).value ())).has_value ());
  auto binary = serialization::write_binary (e.context);
  REQUIRE ((binary).has_value ());
  CHECK ((serialization::read_binary ((binary).value ())).has_value ());
}
