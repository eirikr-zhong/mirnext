/* Licensed under the MIT License. Native x86 constraints, without an x86 runtime. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "Navigation.hpp"
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include <bit>
#include <charconv>
#include <climits>
#include <limits>
#include <sstream>
#include <tuple>

using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
std::unique_ptr<MachineFunction> function () {
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  return std::make_unique<MachineFunction> ("native", "x86-pattern", Signature{{}, {}},
                                            (target).value ());
}
MachineOperand reg (MachineFunction &f, Type t = Type::i64 ()) {
  auto result = mirnext::codegen::detail::access (f).create_register (t);
  REQUIRE ((result).has_value ());
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand ((result).value ());
}
MachineOperand memory (MachineFunction &f, Type type = Type::i64 (), std::int64_t offset = 0) {
  auto base = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
  REQUIRE ((base).has_value ());
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    mirnext::codegen::detail::NodeAccess::
      make_MachineMemory (type,
                          mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                            &(base).value ()),
                          {}, offset, 1, {}, {}));
}
std::optional<X86InstructionPattern> select (MachineOpcode code,
                                             std::initializer_list<MachineOperand> ops,
                                             X86PatternOptions options = {}) {
  return x86_instruction_pattern (code, std::span (ops.begin (), ops.size ()), options);
}
MachineOperand integer (std::int64_t v) {
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (v);
}
std::int64_t number (std::string_view text) {
  std::int64_t value = 0;
  auto result = std::from_chars (text.data (), text.data () + text.size (), value);
  REQUIRE (result.ec == std::errc{});
  REQUIRE (result.ptr == text.data () + text.size ());
  return value;
}
MachineOperand physical (MachineFunction &f, unsigned n) {
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    f.target ().registers ()[n].reg);
}
}  // namespace
TEST_CASE ("x86 constraints enforce ties widths signedness and ordered immediate forms") {
  auto f = function ();
  auto a = reg (*f), b = reg (*f), c = reg (*f);
  CHECK (select (Opcode::Sub, {a, a, b}));
  CHECK_FALSE (select (Opcode::Sub, {a, b, c}));
  auto zero = select (Opcode::Mov, {a, integer (0)});
  REQUIRE (zero);
  CHECK (zero->constraints == "r z");
  for (auto value : {std::int64_t (-128), std::int64_t (127)}) {
    auto p = select (Opcode::Sub, {a, a, integer (value)});
    REQUIRE (p);
    CHECK (p->constraints == "r 0 i0");
  }
  for (auto value : {std::int64_t (-129), std::int64_t (128), std::int64_t (INT32_MIN),
                     std::int64_t (INT32_MAX)}) {
    auto p = select (Opcode::Sub, {a, a, integer (value)});
    REQUIRE (p);
    CHECK (p->constraints == "r 0 i2");
  }
  for (auto value :
       {std::int64_t (INT32_MIN) - 1, std::int64_t (INT32_MAX) + 1, INT64_MIN, INT64_MAX}) {
    CHECK_FALSE (select (Opcode::Sub, {a, a, integer (value)}));
    auto p = select (Opcode::Mov, {a, integer (value)});
    REQUIRE (p);
    CHECK (p->constraints == "r i3");
  }
  auto unsigned_minus_one = mirnext::codegen::detail::NodeAccess::make_MachineOperand (UINT64_MAX);
  CHECK (x86_operands_equal (integer (-1), unsigned_minus_one));
  auto p = select (Opcode::Sub, {a, a, unsigned_minus_one});
  REQUIRE (p);
  CHECK (p->constraints == "r 0 i0");
  CHECK (select (Opcode::Add, {a, b, c}));  // Later LEA form does not require a tie.
  CHECK (select (Opcode::Muls, {a, b, integer (32768)}));
  CHECK_FALSE (select (Opcode::Sub, {a, a}));
  CHECK_FALSE (select (Opcode::Sub, {a, a, b, c}));
  CHECK_FALSE (select (static_cast<Opcode> (instruction_descriptors ().size ()), {a, b}));
  CHECK_FALSE (select (static_cast<Opcode> (65535), {a, b}));
  CHECK_FALSE (select (MachinePseudoOpcode::Use, {}));
}
TEST_CASE ("x86 memory rules preserve signed types displacement and SIB boundaries") {
  auto f = function ();
  auto a = reg (*f), b = reg (*f);
  for (auto kind :
       {Type::Kind::I8, Type::Kind::U8, Type::Kind::I16, Type::Kind::U16, Type::Kind::I32,
        Type::Kind::U32, Type::Kind::I64, Type::Kind::U64, Type::Kind::Pointer}) {
    auto m = memory (*f, Type (kind), INT32_MIN);
    CHECK (select (Opcode::Mov, {a, m}));
    CHECK (select (Opcode::Mov, {m, a}));
  }
  CHECK (select (Opcode::Sub, {a, a, memory (*f, Type::i64 (), INT32_MAX)}));
  CHECK_FALSE (select (Opcode::Sub, {a, a, memory (*f, Type (Type::Kind::I32))}));
  CHECK (select (Opcode::Subs, {a, a, memory (*f, Type (Type::Kind::I32))}));
  CHECK_FALSE (select (Opcode::Subs, {a, a, memory (*f)}));
  for (auto offset : {std::int64_t (INT32_MIN) - 1, std::int64_t (INT32_MAX) + 1})
    CHECK_FALSE (select (Opcode::Mov, {a, memory (*f, Type::i64 (), offset)}));
  auto m = *mirnext::codegen::detail::operand_access (memory (*f)).get<MachineMemory> ();
  m.index = m.base;
  for (unsigned scale : {1u, 2u, 4u, 8u}) {
    m.scale = scale;
    CHECK (
      select (Opcode::Mov, {a, mirnext::codegen::detail::NodeAccess::make_MachineOperand (m)}));
  }
  for (unsigned scale : {0u, 3u, 16u, UINT_MAX}) {
    m.scale = scale;
    CHECK_FALSE (
      select (Opcode::Mov, {a, mirnext::codegen::detail::NodeAccess::make_MachineOperand (m)}));
  }
  m.index.reset ();
  m.scale = 37;
  CHECK (select (Opcode::Mov, {a, mirnext::codegen::detail::NodeAccess::make_MachineOperand (m)}));
  auto other = m;
  other.alias = "different";
  other.nonalias = "metadata";
  other.scale = 12;
  CHECK (x86_operands_equal (mirnext::codegen::detail::NodeAccess::make_MachineOperand (m),
                             mirnext::codegen::detail::NodeAccess::make_MachineOperand (other)));
  CHECK (
    select (Opcode::Sub, {mirnext::codegen::detail::NodeAccess::make_MachineOperand (m),
                          mirnext::codegen::detail::NodeAccess::make_MachineOperand (other), b}));
  other.index = other.base;
  CHECK_FALSE (
    x86_operands_equal (mirnext::codegen::detail::NodeAccess::make_MachineOperand (m),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (other)));
  CHECK_FALSE (
    select (Opcode::Mov, {mirnext::codegen::detail::NodeAccess::make_MachineOperand (m),
                          mirnext::codegen::detail::NodeAccess::make_MachineOperand (m)}));
  CHECK_FALSE (select (Opcode::FMov, {a, memory (*f, Type (Type::Kind::F64))}));
  CHECK (select (Opcode::DMov, {a, memory (*f, Type (Type::Kind::F64))}));
  CHECK_FALSE (select (Opcode::Mov, {a, memory (*f, Type (static_cast<Type::Kind> (65535)))}));
}
TEST_CASE ("x86 relocation selection reserves full addresses and retains call tails") {
  auto f = function ();
  auto a = reg (*f);
  auto s = mirnext::codegen::detail::access (*(f)).intern_symbol (
    {"native", "callee", MachineSymbol::Kind::Function, Signature{{}, {}}, false, {}});
  REQUIRE ((s).has_value ());
  auto symbol = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (), -16));
  auto p = select (Opcode::Mov, {a, symbol});
  REQUIRE (p);
  CHECK (p->constraints == "r i3");
  CHECK_FALSE (select (Opcode::Sub, {a, a, symbol}));
  p = select (Opcode::Mov, {a, mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                 BuiltinReference{Builtin::CopyBlock})});
  REQUIRE (p);
  CHECK (p->constraints == "r i3");
  CHECK (select (Opcode::Call, {symbol, symbol}));
  CHECK (select (Opcode::Call, {symbol, a, a, a}));
  CHECK (select (Opcode::JCall, {symbol, symbol}));
  CHECK (select (Opcode::JRet, {a}));
  CHECK_FALSE (select (Opcode::Call, {}));
  CHECK_FALSE (select (Opcode::Call, {symbol}));
  CHECK_FALSE (select (Opcode::Call, {symbol, memory (*f)}));
  CHECK (select (Opcode::Ret, {}));
  CHECK (select (Opcode::Ret, {a, a}));
}
TEST_CASE ("x86 short branches enforce layout bounds without signed overflow") {
  auto f = function ();
  auto &block = mirnext::codegen::detail::access (*(f)).create_block ();
  auto label = mirnext::codegen::detail::access (*(f)).create_label (block);
  REQUIRE ((label).has_value ());
  MachineOperand l ((label).value ());
  auto a = reg (*f);
  auto p = select (Opcode::Bt, {l, a});
  REQUIRE (p);
  CHECK (p->constraints == "l r");
  p = select (Opcode::Bt, {l, a}, {.allow_short_branches = false});
  REQUIRE (p);
  CHECK (p->constraints == "L r");
  for (auto [distance, maximum, short_form] :
       {std::tuple<std::int64_t, std::size_t, bool>{-120, 4, true},
        {-121, 4, false},
        {129, 4, true},
        {130, 4, false},
        {INT64_MIN, 4, false},
        {INT64_MAX, 4, false},
        {-1, 123, true},
        {-1, 124, false},
        {-1, SIZE_MAX, false}}) {
    CAPTURE (distance);
    CAPTURE (maximum);
    p = select (Opcode::Bt, {l, a}, {.branch_range = X86BranchRange{distance, maximum}});
    REQUIRE (p);
    CHECK (p->constraints == (short_form ? "l r" : "L r"));
  }
  // The unconditional long form precedes its short form in the original table.
  p = select (Opcode::Jmp, {l});
  REQUIRE (p);
  CHECK (p->constraints == "L");
  CHECK_FALSE (select (Opcode::Bt, {integer (0), a}));
  CHECK_FALSE (select (Opcode::LAddr, {a, integer (0)}));
}
TEST_CASE ("x86 private aligned moves have no public selector operand") {
  auto f = function ();
  auto a = reg (*f, Type (Type::Kind::F64)), b = reg (*f, Type (Type::Kind::F64));
  auto m = memory (*f, Type (Type::Kind::F64));
  auto p = select (MachinePseudoOpcode::X86Mov128, {a, b});
  REQUIRE (p);
  CHECK (p->constraints == "r r");
  CHECK (p->encoding == "66 Y 0F 6F r0 R1");
  p = select (MachinePseudoOpcode::X86Mov128, {a, m});
  REQUIRE (p);
  CHECK (p->encoding == "66 Y 0F 6F r0 m1");
  p = select (MachinePseudoOpcode::X86Mov128, {m, a});
  REQUIRE (p);
  CHECK (p->encoding == "66 Y 0F 7F r1 m0");
  CHECK_FALSE (select (MachinePseudoOpcode::X86Mov128, {integer (0), a, b}));
  CHECK_FALSE (select (MachinePseudoOpcode::X86Mov128, {m, m}));
  p = select (MachinePseudoOpcode::X86Mov128Unaligned, {a, m});
  REQUIRE (p);
  CHECK (p->encoding == "F3 Y 0F 6F r0 m1");
  p = select (MachinePseudoOpcode::X86Mov128Unaligned, {m, a});
  REQUIRE (p);
  CHECK (p->encoding == "F3 Y 0F 7F r1 m0");
  CHECK_FALSE (select (MachinePseudoOpcode::X86Mov128Unaligned, {m, m}));
}
TEST_CASE ("x86 physical restrictions reject foreign physical identities") {
  auto f = function ();
  auto a = reg (*f), b = reg (*f);
  if (f->target ().architecture () != target::Architecture::X86_64) {
    CHECK_FALSE (select (Opcode::Mov, {a, physical (*f, 0)}));
    CHECK_FALSE (select (Opcode::Div, {physical (*f, 0), physical (*f, 0), a}));
    CHECK_FALSE (select (Opcode::Lsh, {a, a, physical (*f, 1)}));
    return;
  }
  CHECK (select (Opcode::Div, {physical (*f, 0), physical (*f, 0), a}));
  CHECK_FALSE (select (Opcode::Div, {physical (*f, 2), physical (*f, 0), a}));
  CHECK (select (Opcode::Mod, {physical (*f, 2), physical (*f, 0), a}));
  CHECK (select (Opcode::Lsh, {a, a, physical (*f, 1)}));
  CHECK_FALSE (select (Opcode::Lsh, {a, a, physical (*f, 2)}));
  auto ld = memory (*f, Type (Type::Kind::LongDouble));
  CHECK (select (Opcode::LDMov, {ld, physical (*f, 32)}));
  CHECK (select (Opcode::LDMov, {physical (*f, 33), ld}));
  CHECK_FALSE (select (Opcode::LDMov, {physical (*f, 31), ld}));
}
TEST_CASE ("x86 memory legalization reloads ordered inputs and preserves x87 storage") {
  auto f = function ();
  if (f->target ().architecture () != target::Architecture::X86_64) return;
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto left = memory (*f), right = memory (*f), result = reg (*f);
  auto sub = rw.append (b, Opcode::Sub, {left, left, right});
  REQUIRE ((sub).has_value ());
  auto eq = rw.append (b, Opcode::Eq, {result, left, right});
  REQUIRE ((eq).has_value ());
  auto ftype = Type (Type::Kind::F32);
  auto out = memory (*f, ftype), x = memory (*f, ftype), y = memory (*f, ftype);
  auto add = rw.append (b, Opcode::FAdd, {out, x, y});
  REQUIRE ((add).has_value ());
  auto state = f->state ();
  state.machinized = true;
  REQUIRE ((rw.set_state (state)).has_value ());
  REQUIRE ((legalize_operands (*f)).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  auto before = mirnext::test::neighbor (rw.previous ((sub).value ()));
  REQUIRE ((before).has_value ());
  REQUIRE (&(before).value ());
  CHECK (before.value ().opcode () == MachineOpcode (Opcode::Mov));
  CHECK (before.value ().operands ()[1] == right);
  CHECK (sub.value ().operands ()[0] == left);
  CHECK (sub.value ().operands ()[1] == left);
  CHECK (sub.value ().operands ()[2] == before.value ().operands ()[0]);
  before = mirnext::test::neighbor (rw.previous ((eq).value ()));
  REQUIRE ((before).has_value ());
  REQUIRE (&(before).value ());
  CHECK (before.value ().operands ()[1] == left);
  CHECK (eq.value ().operands ()[2] == right);
  CHECK (eq.value ().operands ()[1] == before.value ().operands ()[0]);
  before = mirnext::test::neighbor (rw.previous ((add).value ()));
  REQUIRE ((before).has_value ());
  REQUIRE (&(before).value ());
  CHECK (before.value ().opcode () == MachineOpcode (Opcode::FMov));
  CHECK (before.value ().operands ()[1] == x);
  auto temp = before.value ().operands ()[0];
  CHECK (add.value ().operands ()[0] == temp);
  CHECK (add.value ().operands ()[1] == temp);
  CHECK (add.value ().operands ()[2] == y);
  auto after = mirnext::test::neighbor (rw.next ((add).value ()));
  REQUIRE ((after).has_value ());
  REQUIRE (&(after).value ());
  CHECK (after.value ().opcode () == MachineOpcode (Opcode::FMov));
  CHECK (after.value ().operands ()[0] == out);
  CHECK (after.value ().operands ()[1] == temp);
  for (auto &i : b.instructions ()) {
    auto matched = has_instruction_pattern (i);
    REQUIRE ((matched).has_value ());
    CHECK ((matched).value ());
  }
  if (sizeof (long double) > sizeof (double)) {
    auto ld = function ();
    auto &lb = mirnext::codegen::detail::access (*(ld)).create_block ();
    MachineRewriter lr (*ld);
    auto a = reg (*ld, Type (Type::Kind::LongDouble)), v = reg (*ld, Type (Type::Kind::LongDouble));
    auto m = memory (*ld, Type (Type::Kind::LongDouble));
    auto i = lr.append (lb, Opcode::LDAdd, {a, v, m});
    REQUIRE ((i).has_value ());
    auto ls = ld->state ();
    ls.machinized = true;
    REQUIRE ((lr.set_state (ls)).has_value ());
    REQUIRE ((legalize_operands (*ld)).has_value ());
    REQUIRE ((ld->verify ()).has_value ());
    CHECK (i.value ().operands ()[2] == m);
    CHECK (std::ranges::distance (lb.instructions ()) == 3);
    CHECK (x86_instruction_ok (i.value ().opcode (), i.value ().operands ()));
  }
}
TEST_CASE ("x86 declaration rows have typed examples on every host without cross execution") {
  std::size_t exercised = 0, physical_rows = 0;
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (auto &p : x86_instruction_patterns ()) {
    CAPTURE (p.index);
    CAPTURE (p.constraints);
    auto f = function ();
    auto &block = mirnext::codegen::detail::access (*(f)).create_block ();
    auto label = mirnext::codegen::detail::access (*(f)).create_label (block);
    REQUIRE ((label).has_value ());
    std::istringstream tokens{std::string (p.constraints)};
    std::string token;
    std::vector<MachineOperand> operands;
    bool unavailable = false;
    while (tokens >> token) {
      if (token == "$") break;
      if (token == "X" || token == "r")
        operands.push_back (reg (*f));
      else if (token == "l" || token == "L")
        operands.emplace_back ((label).value ());
      else if (token == "z")
        operands.push_back (integer (0));
      else if (token == "s")
        operands.push_back (integer (4));
      else if (token.front () == 'h' || token == "t") {
        if (target.value ().architecture () != target::Architecture::X86_64) {
          unavailable = true;
          break;
        }
        unsigned index = token == "t" ? 0 : unsigned (number (std::string_view (token).substr (1)));
        operands.push_back (physical (*f, index));
      } else if (token.front () == 'i') {
        operands.push_back (integer (token == "i0"   ? 100
                                     : token == "i1" ? 1000
                                     : token == "i2" ? 65536
                                                     : INT64_C (1) << 40));
      } else if (token.front () == 'c')
        operands.push_back (integer (number (std::string_view (token).substr (1))));
      else if (token.front () == 'm') {
        Type::Kind kind;
        auto tag = token.substr (1);
        if (tag == "f")
          kind = Type::Kind::F32;
        else if (tag == "d")
          kind = Type::Kind::F64;
        else if (tag == "ld")
          kind = Type::Kind::LongDouble;
        else {
          const bool u = tag.front () == 'u';
          if (tag.front () == 's' || u) tag.erase (0, 1);
          constexpr Type::Kind signed_types[]{Type::Kind::I8, Type::Kind::I16, Type::Kind::I32,
                                              Type::Kind::I64};
          constexpr Type::Kind unsigned_types[]{Type::Kind::U8, Type::Kind::U16, Type::Kind::U32,
                                                Type::Kind::U64};
          kind = u ? unsigned_types[tag.front () - '0'] : signed_types[tag.front () - '0'];
        }
        operands.push_back (memory (*f, Type (kind)));
      } else {
        REQUIRE (token.size () == 1);
        auto n = unsigned (token.front () - '0');
        REQUIRE (n < operands.size ());
        operands.push_back (operands[n]);
      }
    }
    if (unavailable) {
      ++physical_rows;
      continue;
    }
    auto selected = x86_instruction_pattern (p.opcode, operands);
    REQUIRE (selected);
    CHECK (selected->index <= p.index);
    CHECK (selected->opcode == p.opcode);
    ++exercised;
    // Non-tail encodings reject both missing and trailing operands.
    if (p.constraints.find ('$') == std::string_view::npos) {
      auto excess = operands;
      excess.push_back (integer (0));
      CHECK_FALSE (x86_instruction_pattern (p.opcode, excess));
    }
  }
  CAPTURE (exercised);
  CAPTURE (physical_rows);
  CHECK (exercised > 600);
  CHECK (exercised + physical_rows == x86_instruction_patterns ().size ());
  if (target.value ().architecture () == target::Architecture::X86_64)
    CHECK (physical_rows == 0);
  else
    CHECK (physical_rows > 0);
}
TEST_CASE ("x86 tied operand legalization preserves input and output locations") {
  auto f = function ();
  if (f->target ().architecture () != target::Architecture::X86_64) return;
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto a = reg (*f), c = reg (*f), d = reg (*f);
  auto sub = rw.append (b, Opcode::Sub, {a, c, d});
  REQUIRE ((sub).has_value ());
  auto state = f->state ();
  state.machinized = true;
  REQUIRE ((rw.set_state (state)).has_value ());
  REQUIRE ((legalize_operands (*f)).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  CHECK (std::ranges::distance (b.instructions ()) == 3);
  auto at = b.instructions ().begin ();
  CHECK (at->opcode () == MachineOpcode (Opcode::Mov));
  CHECK (at->operands ()[1] == c);
  auto temporary = at->operands ()[0];
  ++at;
  CHECK (&*at == &(sub).value ());
  CHECK (at->operands ()[0] == temporary);
  CHECK (at->operands ()[1] == temporary);
  REQUIRE ((has_instruction_pattern (*at)).has_value ());
  CHECK ((has_instruction_pattern (*at)).value ());
  ++at;
  CHECK (at->opcode () == MachineOpcode (Opcode::Mov));
  CHECK (at->operands ()[0] == a);
  CHECK (at->operands ()[1] == temporary);
  CHECK_FALSE ((legalize_operands (*f)).has_value ());
}
