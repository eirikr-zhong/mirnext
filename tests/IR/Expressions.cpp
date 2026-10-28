#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. Typed selection, failure atomicity and canonical expression
 * storage. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Serialization/Format.hpp"
#include "Expressions.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <limits>
using namespace mirnext;
using namespace mirnext::ir;
namespace {
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *block;
  Example (Signature signature = {}) {
    auto m = mirnext::ir::detail::access (context).create_module ("expressions");
    REQUIRE ((m).has_value ());
    module = &(m).value ();
    auto f
      = mirnext::ir::detail::access (*(module)).create_function ("entry", std::move (signature));
    REQUIRE ((f).has_value ());
    function = &(f).value ();
    auto b = mirnext::ir::detail::access (*(function)).create_block ();
    REQUIRE ((b).has_value ());
    block = &(b).value ();
  }
};
std::size_t register_count (const Function &f) {
  return static_cast<std::size_t> (std::ranges::distance (f.registers ()));
}
Opcode last_opcode (const BasicBlock &block) {
  return std::prev (block.instructions ().end ())->opcode ();
}
mirnext::ir::detail::ExpressionState number (InstructionBuilder &builder, Type type) {
  return type.kind () == Type::Kind::F32          ? builder.literal (42.0f)
         : type.kind () == Type::Kind::F64        ? builder.literal (42.0)
         : type.kind () == Type::Kind::LongDouble ? builder.literal (42.0L)
                                                  : builder.literal (type, 42);
}
}  // namespace
TEST_CASE ("native expression DSL clones and roundtrips executable fixtures") {
  for (unsigned n = 0; n <= static_cast<unsigned> (test::ExpressionExample::Initializer); ++n) {
    CAPTURE (n);
    auto context = test::expression_example (static_cast<test::ExpressionExample> (n));
    auto before = dump (*context);
    auto copy = clone (*context);
    REQUIRE ((copy).has_value ());
    CHECK (dump ((copy).value ()) == before);
    auto text = serialization::write_text (*context);
    REQUIRE ((text).has_value ());
    auto parsed = serialization::parse_text ((text).value ());
    REQUIRE ((parsed).has_value ());
    auto normalized = serialization::write_text ((parsed).value ());
    REQUIRE ((normalized).has_value ());
    CHECK ((normalized).value () == (text).value ());
    auto bytes = serialization::write_binary (*context);
    REQUIRE ((bytes).has_value ());
    auto decoded = serialization::read_binary ((bytes).value ());
    REQUIRE ((decoded).has_value ());
    CHECK (dump (*context) == before);
    CHECK ((decoded.value ().verify ()).has_value ());
  }
}
TEST_CASE ("native expressions select signedness widths and exact arithmetic opcodes") {
  struct Row {
    Type::Kind type;
    Opcode add, sub, mul, div;
  };
  for (auto row :
       {Row{Type::Kind::I8, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::Divs},
        {Type::Kind::U8, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::UDivs},
        {Type::Kind::I16, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::Divs},
        {Type::Kind::U16, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::UDivs},
        {Type::Kind::I32, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::Divs},
        {Type::Kind::U32, Opcode::Adds, Opcode::Subs, Opcode::Muls, Opcode::UDivs},
        {Type::Kind::I64, Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div},
        {Type::Kind::U64, Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::UDiv},
        {Type::Kind::F32, Opcode::FAdd, Opcode::FSub, Opcode::FMul, Opcode::FDiv},
        {Type::Kind::F64, Opcode::DAdd, Opcode::DSub, Opcode::DMul, Opcode::DDiv},
        {Type::Kind::LongDouble, Opcode::LDAdd, Opcode::LDSub, Opcode::LDMul, Opcode::LDDiv}}) {
    CAPTURE (static_cast<unsigned> (row.type));
    Example e;
    InstructionBuilder builder (*e.block);
    auto value = number (builder, Type (row.type));
    auto sum = value + 2;
    REQUIRE (sum.is_valid ());
    CHECK (sum.type () == Type (row.type));
    CHECK (last_opcode (*e.block) == row.add);
    auto sub = 2 - value;
    REQUIRE (sub.is_valid ());
    CHECK (last_opcode (*e.block) == row.sub);
    REQUIRE ((value * 2).is_valid ());
    CHECK (last_opcode (*e.block) == row.mul);
    REQUIRE ((value / 2).is_valid ());
    CHECK (last_opcode (*e.block) == row.div);
    CHECK (e.block->size () == 4);
    CHECK (register_count (*e.function) == 4);
    REQUIRE ((builder.ret ()).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    auto bytes = serialization::write_binary (e.context);
    REQUIRE ((bytes).has_value ());
    auto decoded = serialization::read_binary ((bytes).value ());
    REQUIRE ((decoded).has_value ());
    auto &body = *decoded.value ().modules ().begin ()->functions ().begin ()->blocks ().begin ();
    auto instruction = body.instructions ().begin ();
    CHECK (instruction++->opcode () == row.add);
    CHECK (instruction++->opcode () == row.sub);
    CHECK (instruction++->opcode () == row.mul);
    CHECK (instruction->opcode () == row.div);
  }
}
TEST_CASE ("native bit operations use unsigned shifts and modulo without implicit promotions") {
  struct Row {
    Type::Kind type;
    Opcode mod, right;
    bool small;
  };
  for (auto row : {Row{Type::Kind::I8, Opcode::Mods, Opcode::Rshs, true},
                   {Type::Kind::U8, Opcode::UMods, Opcode::URshs, true},
                   {Type::Kind::I16, Opcode::Mods, Opcode::Rshs, true},
                   {Type::Kind::U16, Opcode::UMods, Opcode::URshs, true},
                   {Type::Kind::I32, Opcode::Mods, Opcode::Rshs, true},
                   {Type::Kind::U32, Opcode::UMods, Opcode::URshs, true},
                   {Type::Kind::I64, Opcode::Mod, Opcode::Rsh, false},
                   {Type::Kind::U64, Opcode::UMod, Opcode::URsh, false}}) {
    Example e;
    InstructionBuilder builder (*e.block);
    auto value = number (builder, Type (row.type));
    REQUIRE ((value % 2).is_valid ());
    CHECK (last_opcode (*e.block) == row.mod);
    REQUIRE ((value >> 2).is_valid ());
    CHECK (last_opcode (*e.block) == row.right);
    REQUIRE ((value << 2).is_valid ());
    CHECK (last_opcode (*e.block) == (row.small ? Opcode::Lshs : Opcode::Lsh));
    REQUIRE ((value & 2).is_valid ());
    CHECK (last_opcode (*e.block) == (row.small ? Opcode::Ands : Opcode::And));
    REQUIRE ((value | 2).is_valid ());
    CHECK (last_opcode (*e.block) == (row.small ? Opcode::Ors : Opcode::Or));
    REQUIRE ((value ^ 2).is_valid ());
    CHECK (last_opcode (*e.block) == (row.small ? Opcode::Xors : Opcode::Xor));
    REQUIRE ((~value).is_valid ());
    CHECK (last_opcode (*e.block) == (row.small ? Opcode::Xors : Opcode::Xor));
    REQUIRE ((builder.ret ()).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native comparison values and direct branches share exact type selection") {
  struct Row {
    Type::Kind type;
    std::array<Opcode, 6> value, branch;
  };
  using O = Opcode;
  for (auto row : {Row{Type::Kind::I64,
                       {O::Eq, O::Ne, O::Lt, O::Le, O::Gt, O::Ge},
                       {O::Beq, O::Bne, O::Blt, O::Ble, O::Bgt, O::Bge}},
                   {Type::Kind::U64,
                    {O::Eq, O::Ne, O::ULt, O::ULe, O::UGt, O::UGe},
                    {O::Beq, O::Bne, O::UBlt, O::UBle, O::UBgt, O::UBge}},
                   {Type::Kind::I32,
                    {O::Eqs, O::Nes, O::Lts, O::Les, O::Gts, O::Ges},
                    {O::Beqs, O::Bnes, O::Blts, O::Bles, O::Bgts, O::Bges}},
                   {Type::Kind::U32,
                    {O::Eqs, O::Nes, O::ULts, O::ULes, O::UGts, O::UGes},
                    {O::Beqs, O::Bnes, O::UBlts, O::UBles, O::UBgts, O::UBges}},
                   {Type::Kind::F32,
                    {O::FEq, O::FNe, O::FLt, O::FLe, O::FGt, O::FGe},
                    {O::FBeq, O::FBne, O::FBlt, O::FBle, O::FBgt, O::FBge}},
                   {Type::Kind::F64,
                    {O::DEq, O::DNe, O::DLt, O::DLe, O::DGt, O::DGe},
                    {O::DBeq, O::DBne, O::DBlt, O::DBle, O::DBgt, O::DBge}},
                   {Type::Kind::LongDouble,
                    {O::LDEq, O::LDNe, O::LDLt, O::LDLe, O::LDGt, O::LDGe},
                    {O::LDBeq, O::LDBne, O::LDBlt, O::LDBle, O::LDBgt, O::LDBge}},
                   {Type::Kind::Pointer,
                    {O::Eq, O::Ne, O::ULt, O::ULe, O::UGt, O::UGe},
                    {O::Beq, O::Bne, O::UBlt, O::UBle, O::UBgt, O::UBge}},
                   {Type::Kind::Bool,
                    {O::Eq, O::Ne, O::Lt, O::Le, O::Gt, O::Ge},
                    {O::Beq, O::Bne, O::Blt, O::Ble, O::Bgt, O::Bge}}}) {
    for (unsigned n = 0; n < 6; ++n) {
      CAPTURE (static_cast<unsigned> (row.type));
      CAPTURE (n);
      Example e;
      InstructionBuilder builder (*e.block);
      auto x = number (builder, Type (row.type));
      auto compared = n == 0   ? x == x
                      : n == 1 ? x != x
                      : n == 2 ? x < x
                      : n == 3 ? x <= x
                      : n == 4 ? x > x
                               : x >= x;
      REQUIRE (compared.is_valid ());
      CHECK (compared.type ().kind () == Type::Kind::Bool);
      CHECK (last_opcode (*e.block) == row.value[n]);
      auto count = register_count (*e.function);
      auto exit = mirnext::ir::detail::access (*(e.function)).create_block ();
      REQUIRE ((exit).has_value ());
      auto label = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value ());
      REQUIRE ((label).has_value ());
      REQUIRE ((builder.branch (static_cast<Comparison> (n), x, x, (label).value ())).has_value ());
      CHECK (last_opcode (*e.block) == row.branch[n]);
      CHECK (register_count (*e.function) == count);
      CHECK (e.block->size () == 2);
      REQUIRE ((builder.set_insert_point ((exit).value ())).has_value ());
      REQUIRE ((builder.ret ()).has_value ());
      REQUIRE ((e.context.verify ()).has_value ());
    }
  }
}
TEST_CASE ("native unary expressions retain types and produce Boolean logical negation") {
  for (auto kind : {Type::Kind::I8, Type::Kind::U32, Type::Kind::I64, Type::Kind::U64,
                    Type::Kind::F32, Type::Kind::F64, Type::Kind::LongDouble}) {
    Example e;
    InstructionBuilder builder (*e.block);
    auto x = number (builder, Type (kind));
    auto negated = -x;
    REQUIRE (negated.is_valid ());
    CHECK (negated.type () == x.type ());
    auto expected = kind == Type::Kind::F32                             ? Opcode::FNeg
                    : kind == Type::Kind::F64                           ? Opcode::DNeg
                    : kind == Type::Kind::LongDouble                    ? Opcode::LDNeg
                    : kind == Type::Kind::I8 || kind == Type::Kind::U32 ? Opcode::Negs
                                                                        : Opcode::Neg;
    CHECK (last_opcode (*e.block) == expected);
    auto logical = !x;
    REQUIRE (logical.is_valid ());
    CHECK (logical.type ().kind () == Type::Kind::Bool);
    REQUIRE ((builder.ret ()).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native literal construction is read-only and preserves exact immediate bits") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto before = e.function->revisions ();
  auto maximum = builder.literal (UINT64_MAX);
  CHECK (maximum.type ().kind () == Type::Kind::U64);
  CHECK (std::bit_cast<std::uint64_t> ((maximum.value ().value ().get<std::int64_t> ()).value ())
         == UINT64_MAX);
  CHECK ((builder.literal (-1).value ().value ().get<std::int64_t> ()).value () == -1);
  auto boolean = builder.literal (Type (Type::Kind::Bool), -7);
  REQUIRE (boolean.is_valid ());
  CHECK ((boolean.value ().value ().get<std::int64_t> ()).value () == 1);
  CHECK (builder.literal (true).type ().kind () == Type::Kind::Bool);
  CHECK (builder.literal (false).type ().kind () == Type::Kind::Bool);
  auto nan = builder.literal (std::numeric_limits<double>::quiet_NaN ());
  CHECK (nan.is_valid ());
  auto zero = builder.literal (-0.0);
  CHECK (std::signbit ((zero.value ().value ().get<double> ()).value ()));
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
  SUBCASE ("wrong type") { CHECK_FALSE (builder.literal (Type::i64 (), 1.0).is_valid ()); }
  SUBCASE ("block") { CHECK_FALSE (builder.literal (Type::block (16), 42).is_valid ()); }
  SUBCASE ("invalid type") {
    CHECK_FALSE (builder.literal (Type (static_cast<Type::Kind> (99)), 42).is_valid ());
  }
  SUBCASE ("not immediate") {
    CHECK_FALSE (builder.literal (Operand (e.function->symbol ())).is_valid ());
  }
  SUBCASE ("wrong floating literal precision") {
    CHECK_FALSE ((builder.literal (1.0f) + 1.0).is_valid ());
  }
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
}
TEST_CASE ("native expression failures publish no result and retain their first diagnostic") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto before = e.function->revisions ();
  auto x = builder.literal (42);
  SUBCASE ("type mismatch") { CHECK_FALSE ((x + builder.literal (2.0)).is_valid ()); }
  SUBCASE ("floating modulo") { CHECK_FALSE ((builder.literal (2.0) % 2).is_valid ()); }
  SUBCASE ("floating shift") { CHECK_FALSE ((builder.literal (2.0) << 2).is_valid ()); }
  SUBCASE ("Boolean arithmetic") {
    CHECK_FALSE ((builder.literal (Type (Type::Kind::Bool), true) + 1).is_valid ());
  }
  SUBCASE ("pointer arithmetic") {
    CHECK_FALSE ((builder.literal (Type::pointer (), 0) + 1).is_valid ());
  }
  SUBCASE ("Boolean complement") {
    CHECK_FALSE ((~builder.literal (Type (Type::Kind::Bool), true)).is_valid ());
  }
  SUBCASE ("pointer negation") {
    CHECK_FALSE ((-builder.literal (Type::pointer (), 0)).is_valid ());
  }
  SUBCASE ("different Builder") {
    InstructionBuilder other (*e.block);
    CHECK_FALSE ((x + other.literal (2)).is_valid ());
  }
  SUBCASE ("foreign literal") {
    Example foreign;
    InstructionBuilder other (*foreign.block);
    CHECK_FALSE ((x + other.literal (2)).is_valid ());
  }
  SUBCASE ("foreign register") {
    Example foreign;
    auto reg = mirnext::ir::detail::access (*(foreign.function)).create_register (Type::i64 ());
    REQUIRE ((reg).has_value ());
    CHECK_FALSE (builder.expression ((reg).value ()).is_valid ());
  }
  SUBCASE ("no insertion point") {
    builder.clear_insert_point ();
    CHECK_FALSE ((x + 1).is_valid ());
  }
  REQUIRE ((e.module->error ()).has_value ());
  auto message = e.module->error ().value ().message;
  CHECK_FALSE (((x * 2).value ()).has_value ());
  CHECK (e.module->error ().value ().message == message);
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
}
TEST_CASE ("native conversion sequences cover every scalar source and destination") {
  for (unsigned from = 0; from <= static_cast<unsigned> (Type::Kind::Pointer); ++from)
    for (unsigned to = 0; to <= static_cast<unsigned> (Type::Kind::Pointer); ++to) {
      CAPTURE (from);
      CAPTURE (to);
      Example e;
      InstructionBuilder builder (*e.block);
      auto source = Type (static_cast<Type::Kind> (from)),
           target = Type (static_cast<Type::Kind> (to));
      auto before = e.function->revisions ();
      auto result = number (builder, source).convert (target);
      auto wide = [] (Type type) {
        return type == Type::i64 () || type.kind () == Type::Kind::U64 || type == Type::pointer ();
      };
      bool supported = target.kind () == Type::Kind::Bool
                       || (!(source.is_float () && target.kind () == Type::Kind::U64)
                           && ((source != Type::pointer () && target != Type::pointer ())
                               || (wide (source) && wide (target))));
      if (!supported) {
        CHECK_FALSE (result.is_valid ());
        CHECK (e.function->revisions () == before);
        continue;
      }
      REQUIRE (result.is_valid ());
      CHECK (result.type () == target);
      if (source == target) CHECK (e.function->revisions () == before);
      REQUIRE ((builder.ret ()).has_value ());
      REQUIRE ((e.context.verify ()).has_value ());
      auto bytes = serialization::write_binary (e.context);
      REQUIRE ((bytes).has_value ());
      CHECK ((serialization::read_binary ((bytes).value ())).has_value ());
    }
}
TEST_CASE ("native conversions validate complete sequences and normalize integer representations") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto small = builder.literal (Type (Type::Kind::I8), 255).convert (Type (Type::Kind::U16));
  REQUIRE (small.is_valid ());
  auto instruction = e.block->instructions ().begin ();
  CHECK (instruction++->opcode () == Opcode::Ext8);
  CHECK (instruction->opcode () == Opcode::UExt16);
  auto before = e.function->revisions ();
  auto count = register_count (*e.function);
  auto size = e.block->size ();
  SUBCASE ("closed block") {
    builder.clear_insert_point ();
    CHECK_FALSE (small.convert (Type (Type::Kind::F64)).is_valid ());
  }
  SUBCASE ("invalid destination") { CHECK_FALSE (small.convert (Type::block (16)).is_valid ()); }
  SUBCASE ("unsupported pointer") { CHECK_FALSE (small.convert (Type::pointer ()).is_valid ()); }
  SUBCASE ("floating unsigned full range") {
    CHECK_FALSE (builder.literal (42.0).convert (Type (Type::Kind::U64)).is_valid ());
  }
  CHECK (e.function->revisions () == before);
  CHECK (register_count (*e.function) == count);
  CHECK (e.block->size () == size);
}
TEST_CASE ("native typed expression boundaries validate before publishing instructions") {
  Example e ({{Type::i64 ()}, {}});
  InstructionBuilder builder (*e.block);
  auto wrong = builder.literal (42.0);
  auto variable
    = mirnext::ir::detail::access (*(e.module)).create_variable ("value", Type::i64 (), 0);
  REQUIRE ((variable).has_value ());
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  auto target = mirnext::ir::detail::access (*(e.module))
                  .import ("target", Signature{{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((target).has_value ());
  auto before = e.function->revisions ();
  SUBCASE ("return") { CHECK_FALSE ((builder.ret (wrong)).has_value ()); }
  SUBCASE ("assignment") { CHECK_FALSE ((builder.assign ((reg).value (), wrong)).has_value ()); }
  SUBCASE ("global store") {
    CHECK_FALSE ((builder.store ((variable).value (), wrong)).has_value ());
  }
  SUBCASE ("memory store") {
    CHECK_FALSE (
      (builder.store (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 ()), wrong))
        .has_value ());
  }
  SUBCASE ("call") { CHECK_FALSE ((builder.call ((target).value (), {wrong})).has_value ()); }
  SUBCASE ("foreign erroneous call argument") {
    Example foreign;
    InstructionBuilder other (*foreign.block);
    auto failed = other.literal (42.0) % 2;
    CHECK_FALSE ((builder.call ((target).value (), {failed})).has_value ());
  }
  CHECK (e.function->revisions () == before);
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 1);
}
TEST_CASE ("native integer conversions avoid redundant moves and extensions") {
  struct Row {
    Type::Kind from, to;
    Opcode opcode;
  };
  for (auto row : {Row{Type::Kind::I8, Type::Kind::U64, Opcode::Ext8},
                   {Type::Kind::U8, Type::Kind::I64, Opcode::UExt8},
                   {Type::Kind::I32, Type::Kind::I8, Opcode::Ext8},
                   {Type::Kind::U32, Type::Kind::I16, Opcode::Ext16},
                   {Type::Kind::I8, Type::Kind::I16, Opcode::Ext8},
                   {Type::Kind::U8, Type::Kind::U16, Opcode::UExt8},
                   {Type::Kind::U8, Type::Kind::I32, Opcode::UExt8},
                   {Type::Kind::I16, Type::Kind::U8, Opcode::UExt8}}) {
    Example e;
    InstructionBuilder builder (*e.block);
    auto result = number (builder, Type (row.from)).convert (Type (row.to));
    REQUIRE (result.is_valid ());
    CHECK (result.type () == Type (row.to));
    CHECK (e.block->size () == 1);
    CHECK (register_count (*e.function) == 1);
    CHECK (last_opcode (*e.block) == row.opcode);
    REQUIRE ((builder.ret ()).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
  }
}
TEST_CASE ("native expression handles survive bulk insertion and independent cloning") {
  std::unique_ptr<Program> copy;
  {
    Example e ({{Type::i64 ()}, {}});
    InstructionBuilder builder (*e.block);
    auto first = builder.literal (20) + 22;
    auto operand = first.value ();
    REQUIRE ((operand).has_value ());
    for (unsigned n = 0; n < 4096; ++n) REQUIRE ((first + n).is_valid ());
    CHECK ((first.value ()).value () == (operand).value ());
    REQUIRE ((builder.ret (first)).has_value ());
    REQUIRE ((e.context.verify ()).has_value ());
    auto cloned = clone (e.context);
    REQUIRE ((cloned).has_value ());
    copy = std::make_unique<Program> (std::move ((cloned).value ()));
  }
  CHECK ((copy->verify ()).has_value ());
  CHECK ((serialization::write_binary (*copy)).has_value ());
}
TEST_CASE ("native direct branch rejects invalid comparisons and owners atomically") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto target = mirnext::ir::detail::access (*(e.function)).create_label (*e.block);
  REQUIRE ((target).has_value ());
  auto x = builder.literal (42);
  auto before = e.function->revisions ();
  SUBCASE ("invalid comparison") {
    CHECK_FALSE (
      (builder.branch (static_cast<Comparison> (99), x, x, (target).value ())).has_value ());
  }
  SUBCASE ("mixed types") {
    CHECK_FALSE ((builder.branch (Comparison::Equal, x, builder.literal (42.0), (target).value ()))
                   .has_value ());
  }
  SUBCASE ("foreign label") {
    Example other;
    auto foreign = mirnext::ir::detail::access (*(other.function)).create_label (*other.block);
    REQUIRE ((foreign).has_value ());
    CHECK_FALSE ((builder.branch (Comparison::Equal, x, x, (foreign).value ())).has_value ());
  }
  CHECK (e.block->size () == 0);
  CHECK (register_count (*e.function) == 0);
  CHECK (e.function->revisions () == before);
}
TEST_CASE ("native Boolean memory expressions normalize raw writes before use") {
  Example e;
  InstructionBuilder builder (*e.block);
  auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
  REQUIRE ((address).has_value ());
  auto value = builder.expression (
    mirnext::ir::detail::NodeAccess::make_Memory (Type (Type::Kind::Bool), &(address).value ()));
  REQUIRE (value.is_valid ());
  CHECK (value.type ().kind () == Type::Kind::Bool);
  REQUIRE (e.block->size () == 2);
  auto instruction = e.block->instructions ().begin ();
  CHECK (instruction->opcode () == Opcode::Mov);
  REQUIRE ((instruction->operands ()[1].get<Memory> ()).has_value ());
  CHECK (instruction->operands ()[1].get<Memory> ().value ().type == Type::i64 ());
  CHECK ((++instruction)->opcode () == Opcode::Ne);
  REQUIRE ((builder.ret ()).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}
