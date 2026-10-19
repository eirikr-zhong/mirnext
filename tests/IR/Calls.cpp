/* Licensed under the MIT License. Native call construction and protocol validation. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "Calls.hpp"
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Context context;
  Module *module;
  Function *function;
  BasicBlock *block;
  Example () {
    auto m = context.create_module ("calls");
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
    REQUIRE (copied);
    CHECK (dump (**copied) == source);
    auto text = serialization::write_text (*context);
    INFO ((text ? "" : text.error ().message));
    REQUIRE (text);
    auto parsed = serialization::parse_text (*text);
    INFO ((parsed ? "" : parsed.error ().message));
    REQUIRE (parsed);
    auto normalized = serialization::write_text (**parsed);
    REQUIRE (normalized);
    CHECK (*normalized == *text);
    auto bytes = serialization::write_binary (*context);
    REQUIRE (bytes);
    auto decoded = serialization::read_binary (*bytes);
    INFO ((decoded ? "" : decoded.error ().message));
    REQUIRE (decoded);
    CHECK (dump (*context) == source);
    CHECK (needs_materialization (*context));
    CHECK_FALSE (needs_materialization (**decoded));
  }
}
TEST_CASE ("native invalid calls publish neither registers nor instructions") {
  Example e;
  IRBuilder builder (*e.block);
  auto target = e.module->import ("target", Signature{{Type::i64 (), Type (Type::Kind::F64)},
                                                      {Type::block (16)}});
  REQUIRE (target);
  auto address = e.function->create_register (Type::pointer ());
  REQUIRE (address);
  auto before = e.function->revisions ();
  auto registers = register_count (*e.function);
  auto symbols = symbol_count (*e.module);
  auto failed = builder.call (**target, {BlockArgument (**address, 8)});
  REQUIRE_FALSE (failed);
  CHECK (failed.error ().message.find ("block argument size") != std::string::npos);
  CHECK (register_count (*e.function) == registers);
  CHECK (symbol_count (*e.module) == symbols);
  CHECK (e.block->size () == 0);
  CHECK (e.function->revisions () == before);
  auto repeated = builder.call (**target, {BlockArgument (**address, 16)});
  REQUIRE_FALSE (repeated);
  CHECK (repeated.error ().message == failed.error ().message);
}
TEST_CASE ("native call DSL rejects ownership types and counts before publishing") {
  Example e;
  IRBuilder builder (*e.block);
  auto target = e.module->import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (target);
  const auto before = e.function->revisions ();
  SUBCASE ("missing argument") { CHECK_FALSE (builder.call (**target)); }
  SUBCASE ("extra argument") {
    CHECK_FALSE (builder.call (**target, {std::int64_t{1}, std::int64_t{2}}));
  }
  SUBCASE ("wrong scalar type") { CHECK_FALSE (builder.call (**target, {1.0})); }
  SUBCASE ("BlockArgument cannot masquerade as scalar") {
    auto reg = e.function->create_register (Type::pointer ());
    REQUIRE (reg);
    auto count = register_count (*e.function);
    CHECK_FALSE (builder.call (**target, {BlockArgument (**reg, Type::i64 ())}));
    CHECK (register_count (*e.function) == count);
    return;
  }
  SUBCASE ("foreign signature symbol") {
    auto other = e.context.create_module ("other");
    REQUIRE (other);
    auto foreign = (*other)->import ("target", *(*target)->signature ());
    REQUIRE (foreign);
    CHECK_FALSE (builder.call (**foreign, {std::int64_t{1}}));
  }
  SUBCASE ("foreign argument register") {
    auto other = e.module->create_function ("other", {});
    REQUIRE (other);
    auto reg = (*other)->create_register (Type::i64 ());
    REQUIRE (reg);
    CHECK_FALSE (builder.call (**target, {**reg}));
  }
  SUBCASE ("untyped import") {
    auto untyped = e.module->import ("untyped");
    REQUIRE (untyped);
    CHECK_FALSE (builder.call (**untyped));
  }
  SUBCASE ("noncallable prototype") {
    auto proto = e.module->prototype ("proto", *(*target)->signature ());
    REQUIRE (proto);
    CHECK_FALSE (builder.call (**proto, {std::int64_t{1}}));
  }
  SUBCASE ("closed insertion point") {
    builder.clear_insert_point ();
    CHECK_FALSE (builder.call (**target, {std::int64_t{1}}));
  }
  CHECK (e.function->revisions () == before);
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
}
TEST_CASE ("native block arguments retain ABI class size address and displacement") {
  Example e;
  IRBuilder builder (*e.block);
  auto address = builder.allocate_stack (Operand (std::int64_t{32}));
  REQUIRE (address);
  for (unsigned n = 0; n <= 4; ++n) {
    auto type = Type::block (16, static_cast<Type::BlockClass> (n));
    auto target = e.module->import ("block" + std::to_string (n), Signature{{}, {type}});
    REQUIRE (target);
    auto call = builder.call (**target, {BlockArgument (**address, type, 8)});
    REQUIRE (call);
    auto memory = call->instruction ().operands ()[2].get<Memory> ();
    REQUIRE (memory);
    CHECK (memory->type == type);
    CHECK (memory->base == *address);
    CHECK (memory->displacement == 8);
    CHECK (call->results ().empty ());
    CHECK_FALSE (call->value ());
  }
  REQUIRE (builder.ret (Operand (std::int64_t{42})));
  REQUIRE (e.context.verify ());
  auto binary = serialization::write_binary (e.context);
  REQUIRE (binary);
  auto decoded = serialization::read_binary (*binary);
  REQUIRE (decoded);
  CHECK ((*decoded)->verify ());
}
TEST_CASE ("native variadic calls require promotion and prohibit unnamed return blocks") {
  Example e;
  IRBuilder builder (*e.block);
  auto target = e.module->import ("variadic", Signature{{Type::i64 ()}, {Type::i64 ()}, true});
  REQUIRE (target);
  SUBCASE ("float") { CHECK_FALSE (builder.call (**target, {std::int64_t{0}, 1.0f})); }
  SUBCASE ("return block") {
    auto address = e.function->create_register (Type::pointer ());
    REQUIRE (address);
    auto count = register_count (*e.function);
    CHECK_FALSE (
      builder.call (**target, {std::int64_t{0}, BlockArgument::return_buffer (**address, 16)}));
    CHECK (register_count (*e.function) == count);
  }
  SUBCASE ("empty block") {
    auto address = e.function->create_register (Type::pointer ());
    REQUIRE (address);
    CHECK_FALSE (builder.call (**target, {std::int64_t{0}, BlockArgument (**address, 0)}));
  }
  SUBCASE ("promoted double") {
    auto call = builder.call (**target, {std::int64_t{0}, 1.0});
    REQUIRE (call);
    CHECK (call->results ().size () == 1);
    return;
  }
  CHECK (e.block->size () == 0);
}
TEST_CASE (
  "native forward and imported call results retain identity across definitions and clones") {
  Context context;
  auto module = context.create_module ("definitions");
  REQUIRE (module);
  Signature signature{{Type::i64 ()}, {Type::i64 ()}};
  auto declared = (*module)->declare_function ("callee", signature);
  REQUIRE (declared);
  auto caller = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  REQUIRE (caller);
  auto block = (*caller)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  auto call = builder.call (**declared, {std::int64_t{42}});
  REQUIRE (call);
  auto value = call->value ();
  REQUIRE (value);
  REQUIRE (builder.ret (Operand (**value)));
  auto defined = (*module)->define_function (**declared);
  REQUIRE (defined);
  auto body = (*defined)->create_block ();
  REQUIRE (body);
  REQUIRE (IRBuilder (**body).ret (Operand (*(*defined)->arguments ()[0])));
  REQUIRE ((*module)->export_symbol (**declared));
  CHECK (call->instruction ().operands ()[1].get<SymbolReference> ()->symbol == *declared);
  auto other = context.create_module ("consumer");
  REQUIRE (other);
  auto imported = (*other)->import ("callee", signature);
  REQUIRE (imported);
  auto consumer = (*other)->create_function ("consumer", {{Type::i64 ()}, {}});
  REQUIRE (consumer);
  auto entry = (*consumer)->create_block ();
  REQUIRE (entry);
  IRBuilder consuming (**entry);
  auto result = consuming.call (**imported, {std::int64_t{42}});
  REQUIRE (result);
  auto scalar = result->value ();
  REQUIRE (scalar);
  REQUIRE (consuming.ret (Operand (**scalar)));
  REQUIRE (context.verify ());
  auto binary = serialization::write_binary (context);
  REQUIRE (binary);
  auto decoded = serialization::read_binary (*binary);
  REQUIRE (decoded);
  auto copied = clone (context);
  REQUIRE (copied);
  CHECK (dump (**copied) == dump (context));
}
TEST_CASE ("native jump calls use a separate void convention") {
  Context context;
  auto module = context.create_module ("jump");
  REQUIRE (module);
  auto target = (*module)->create_function ("target", {});
  REQUIRE (target);
  auto body = (*target)->create_block ();
  REQUIRE (body);
  REQUIRE (IRBuilder (**body).ret_to (Operand (std::int64_t{0})));
  auto caller = (*module)->create_function ("caller", {});
  REQUIRE (caller);
  auto entry = (*caller)->create_block ();
  REQUIRE (entry);
  IRBuilder builder (**entry);
  auto call = builder.jump_call ((*target)->symbol ());
  REQUIRE (call);
  CHECK ((*call)->opcode () == Opcode::JCall);
  REQUIRE (context.verify ());
  SUBCASE ("ordinary call convention rejected") {
    REQUIRE (IRRewriter (**caller).replace (**call, Opcode::Call, (*call)->operands ()));
    REQUIRE (builder.ret ());
    CHECK_FALSE (context.verify ());
  }
  SUBCASE ("nonvoid jump signature rejected") {
    auto invalid = (*module)->import ("invalid", Signature{{Type::i64 ()}, {}});
    REQUIRE (invalid);
    auto other = (*caller)->create_block ();
    REQUIRE (other);
    REQUIRE (builder.set_insert_point (**other));
    auto failed = builder.jump_call (**invalid);
    REQUIRE_FALSE (failed);
    CHECK ((*other)->size () == 0);
  }
}
TEST_CASE ("native va_arg helpers return values and checked address operations") {
  Context context;
  auto module = context.create_module ("va");
  REQUIRE (module);
  auto function
    = (*module)->create_function ("sum", {{Type::i64 ()}, {Type::i64 (), Type::block (16)}, true});
  REQUIRE (function);
  auto block = (*function)->create_block ();
  REQUIRE (block);
  IRBuilder builder (**block);
  auto list = builder.allocate_stack (Operand (std::int64_t{64}));
  REQUIRE (list);
  REQUIRE (builder.start_varargs (Operand (**list)));
  auto value = builder.next_vararg (Type (Type::Kind::F64), Operand (**list), "value");
  REQUIRE (value);
  CHECK ((*value)->type () == Type (Type::Kind::F64));
  auto address = builder.next_vararg_address (Type::i64 (), Operand (**list));
  REQUIRE (address);
  CHECK ((*address)->type () == Type::pointer ());
  auto destination = builder.allocate_stack (Operand (std::int64_t{16}));
  REQUIRE (destination);
  for (unsigned n = 0; n <= 4; ++n)
    REQUIRE (builder.next_vararg_block (**destination, Operand (**list),
                                        Type::block (16, static_cast<Type::BlockClass> (n))));
  REQUIRE (
    builder.next_vararg_block (*(*function)->arguments ()[1], Operand (**list), Type::block (16)));
  REQUIRE (builder.end_varargs (Operand (**list)));
  REQUIRE (builder.ret (Operand (std::int64_t{0})));
  REQUIRE (context.verify ());
  auto binary = serialization::write_binary (context);
  REQUIRE (binary);
  CHECK (serialization::read_binary (*binary));
}
TEST_CASE ("native invalid variadic helpers are atomic and retain the first diagnostic") {
  Example e;
  IRBuilder builder (*e.block);
  auto list = e.function->create_register (Type::pointer (), "list");
  REQUIRE (list);
  auto before = e.function->revisions ();
  auto count = register_count (*e.function);
  SUBCASE ("start requires varargs") { CHECK_FALSE (builder.start_varargs (Operand (**list))); }
  SUBCASE ("block scalar type") {
    CHECK_FALSE (builder.next_vararg_block (**list, Operand (**list), Type::i64 ()));
  }
  SUBCASE ("block return buffer") {
    CHECK_FALSE (builder.next_vararg_block (**list, Operand (**list), Type::return_block (16)));
  }
  SUBCASE ("block empty") {
    CHECK_FALSE (builder.next_vararg_block (**list, Operand (**list), Type::block (0)));
  }
  SUBCASE ("invalid ABI class") {
    CHECK_FALSE (builder.next_vararg_block (**list, Operand (**list),
                                            Type::block (16, static_cast<Type::BlockClass> (5))));
  }
  SUBCASE ("block size exceeds protocol range") {
    CHECK_FALSE (builder.next_vararg_block (**list, Operand (**list),
                                            Type::block (static_cast<std::size_t> (UINT64_MAX))));
  }
  SUBCASE ("va_arg block") {
    CHECK_FALSE (builder.next_vararg (Type::block (16), Operand (**list)));
  }
  SUBCASE ("va_arg undef") {
    CHECK_FALSE (builder.next_vararg (Type (Type::Kind::Undef), Operand (**list)));
  }
  SUBCASE ("va_arg duplicate name") {
    CHECK_FALSE (builder.next_vararg (Type::i64 (), Operand (**list), "list"));
  }
  SUBCASE ("va_arg foreign list") {
    auto other = e.module->create_function ("other", {});
    REQUIRE (other);
    auto foreign = (*other)->create_register (Type::pointer ());
    REQUIRE (foreign);
    CHECK_FALSE (builder.next_vararg (Type::i64 (), Operand (**foreign)));
  }
  CHECK (register_count (*e.function) == count);
  CHECK (e.block->size () == 0);
  CHECK (e.function->revisions () == before);
}
TEST_CASE ("native indirect memory targets use CALL with typed multiple results") {
  Example e;
  IRBuilder builder (*e.block);
  auto prototype
    = e.module->prototype ("signature", Signature{{Type::i64 (), Type (Type::Kind::F64)},
                                                  {Type (Type::Kind::F64)}});
  REQUIRE (prototype);
  auto pointer = e.function->create_register (Type::pointer ());
  REQUIRE (pointer);
  auto call
    = builder.call_indirect (**prototype, Operand (Memory{Type::pointer (), *pointer}), {1.0});
  REQUIRE (call);
  CHECK (call->instruction ().opcode () == Opcode::Call);
  REQUIRE (call->results ().size () == 2);
  CHECK (call->results ()[0]->type () == Type::i64 ());
  CHECK (call->results ()[1]->type () == Type (Type::Kind::F64));
  CHECK (call->instruction ().operands ()[1].get<Memory> ());
  CHECK (output_operand (*e.function, call->instruction (), 2));
  CHECK (output_operand (*e.function, call->instruction (), 3));
  CHECK_FALSE (output_operand (*e.function, call->instruction (), 4));
  REQUIRE (builder.ret (Operand (*call->results ()[0])));
  REQUIRE (e.context.verify ());
  auto text = serialization::write_text (e.context);
  REQUIRE (text);
  CHECK (serialization::parse_text (*text));
  auto binary = serialization::write_binary (e.context);
  REQUIRE (binary);
  CHECK (serialization::read_binary (*binary));
}
TEST_CASE ("native call result handles remain stable through bulk construction and edits") {
  Example e;
  IRBuilder builder (*e.block);
  auto target = e.module->import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (target);
  auto first = builder.call (**target, {std::int64_t{42}});
  REQUIRE (first);
  auto first_value = first->value ();
  REQUIRE (first_value);
  auto instruction = &first->instruction ();
  auto value = *first_value;
  for (unsigned n = 0; n < 4096; ++n) REQUIRE (builder.call (**target, {*value}));
  CHECK (&first->instruction () == instruction);
  CHECK (*first->value () == value);
  CHECK (register_count (*e.function) == 4097);
  CHECK (e.function->instruction_count () == 4097);
  REQUIRE (builder.ret (Operand (*value)));
  REQUIRE (e.context.verify ());
  auto copied = clone (e.context);
  REQUIRE (copied);
  CHECK (dump (**copied) == dump (e.context));
  auto bytes = serialization::write_binary (e.context);
  REQUIRE (bytes);
  auto decoded = serialization::read_binary (*bytes);
  REQUIRE (decoded);
  CHECK ((*decoded)->modules ().begin ()->functions ().begin ()->instruction_count () == 4098);
}
TEST_CASE ("native mismatched indirect signatures fail without publishing results") {
  Example e;
  IRBuilder builder (*e.block);
  auto signature = e.module->prototype ("signature", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (signature);
  auto target = e.module->import ("target", Signature{{Type (Type::Kind::F64)}, {Type::i64 ()}});
  REQUIRE (target);
  auto before = e.function->revisions ();
  auto call = builder.call_indirect (**signature, Operand (**target), {std::int64_t{42}});
  REQUIRE_FALSE (call);
  CHECK (call.error ().message.find ("callee signature mismatch") != std::string::npos);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
  CHECK (e.block->size () == 0);
}
TEST_CASE ("native integral literals preserve sign width and boolean values") {
  Example e;
  IRBuilder builder (*e.block);
  auto target
    = e.module->import ("target", Signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}});
  REQUIRE (target);
  auto call = builder.call (**target, {20, 22});
  REQUIRE (call);
  CHECK (*call->instruction ().operands ()[3].get<std::int64_t> () == 20);
  CHECK (*call->instruction ().operands ()[4].get<std::int64_t> () == 22);
  CHECK (*Operand (-1).get<std::int64_t> () == -1);
  CHECK (*Operand (UINT64_MAX).get<std::uint64_t> () == UINT64_MAX);
  CHECK (*Operand (true).get<std::int64_t> () == 1);
  CHECK (*Operand (false).get<std::int64_t> () == 0);
  CHECK (*Operand (static_cast<unsigned short> (65535)).get<std::uint64_t> () == 65535);
  auto result = call->value ();
  REQUIRE (result);
  REQUIRE (builder.ret (Operand (**result)));
  REQUIRE (e.context.verify ());
}
TEST_CASE ("native single Operand argument lists select direct calls unambiguously") {
  Example e;
  IRBuilder builder (*e.block);
  auto target = e.module->import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (target);
  auto direct = builder.call (**target, {Operand (42)});
  REQUIRE (direct);
  const auto operands = direct->instruction ().operands ();
  REQUIRE (operands.size () == 4);
  REQUIRE (operands[1].get<SymbolReference> ());
  CHECK (operands[1].get<SymbolReference> ()->symbol == *target);
  REQUIRE (operands[3].get<std::int64_t> ());
  CHECK (*operands[3].get<std::int64_t> () == 42);
  auto value = direct->value ();
  REQUIRE (value);
  auto passed = builder.call (**target, {Operand (**value)});
  REQUIRE (passed);
  auto address = builder.address_of (**target);
  REQUIRE (address);
  auto indirect = builder.call_indirect (**target, Operand (**address), {Operand (**value)});
  REQUIRE (indirect);
  CHECK (indirect->instruction ().operands ()[1].get<const Register *> ());
  auto no_arguments = e.module->import ("no_arguments", Signature{{Type::i64 ()}, {}});
  REQUIRE (no_arguments);
  auto empty_address = builder.address_of (**no_arguments);
  REQUIRE (empty_address);
  auto empty = builder.call_indirect (**no_arguments, Operand (**empty_address));
  REQUIRE (empty);
  CHECK (empty->instruction ().operands ().size () == 3);
  auto result = indirect->value ();
  REQUIRE (result);
  REQUIRE (builder.ret (Operand (**result)));
  REQUIRE (e.context.verify ());
  auto text = serialization::write_text (e.context);
  REQUIRE (text);
  CHECK (serialization::parse_text (*text));
  auto binary = serialization::write_binary (e.context);
  REQUIRE (binary);
  CHECK (serialization::read_binary (*binary));
}
