/* Licensed under the MIT License. Native pattern and reload regressions. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Analysis.hpp"
#include <array>
#include <limits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool arm () {
  return target::TargetMachine::native ().value ().architecture () == target::Architecture::AArch64;
}
std::unique_ptr<MachineFunction> function () {
  auto t = target::TargetMachine::native ();
  REQUIRE ((t).has_value ());
  return std::make_unique<MachineFunction> ("native", "legalize", Signature{{}, {}}, (t).value ());
}
MachineOperand reg (MachineFunction &f, Type type = Type::i64 ()) {
  auto r = mirnext::codegen::detail::access (f).create_register (type);
  REQUIRE ((r).has_value ());
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ());
}
MachineOperand memory (MachineFunction &f, Type type, std::int64_t displacement = 0,
                       bool indexed = false, unsigned scale = 1) {
  auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
  REQUIRE ((r).has_value ());
  return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    mirnext::codegen::detail::NodeAccess::
      make_MachineMemory (type,
                          mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                            &(r).value ()),
                          indexed ? mirnext::codegen::detail::NodeAccess::optional_register (
                                      &(r).value ())
                                  : std::nullopt,
                          displacement, scale, "a", "b"));
}
bool matches (MachineFunction &f, Opcode code, std::vector<MachineOperand> operands) {
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter rw (f);
  auto i = rw.append (b, code, std::move (operands));
  INFO (((i).has_value () ? "" : i.error ().message));
  REQUIRE ((i).has_value ());
  auto result = has_instruction_pattern ((i).value ());
  REQUIRE ((result).has_value ());
  return (result).value ();
}
void ready (MachineFunction &f) {
  auto s = f.state ();
  s.machinized = true;
  MachineRewriter rw (f);
  REQUIRE ((rw.set_state (s)).has_value ());
}
enum class TestCode {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) name,
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
  SubUnsignedOverflow,
  SubUnsignedNoOverflow,
  MultiplyOverflow,
  MultiplyNoOverflow
};
struct pattern {
  TestCode code;
  const char *pattern, *replacement;
  unsigned vector_shape = 0;
};
#define MIRNEXT_A64_OPCODE(name) TestCode::name
#define MIRNEXT_A64_TABLE static const struct pattern patterns[]
#include "mirnext/Target/AArch64Patterns.def"
#undef MIRNEXT_A64_TABLE
#undef MIRNEXT_A64_OPCODE
}  // namespace
TEST_CASE ("every shared AArch64 instruction pattern has a native matching example") {
  if (!arm ()) return;
  for (auto &p : patterns) {
    CAPTURE (p.pattern);
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto label = mirnext::codegen::detail::access (*(f)).create_label (b);
    REQUIRE ((label).has_value ());
    auto code = p.code >= TestCode::SubUnsignedOverflow ? Opcode::Bo : static_cast<Opcode> (p.code);
    CAPTURE (opcode_name (code));
    auto d = instruction_descriptor (code).get ();
    REQUIRE (d);
    std::vector<MachineOperand> operands;
    std::string_view spec (p.pattern);
    while (!spec.empty ()) {
      auto end = spec.find (' ');
      auto token = spec.substr (0, end);
      auto n = operands.size ();
      if (token == "$") {
        if (code == Opcode::Switch) operands.emplace_back ((label).value ());
        break;
      }
      if (token == "X") {
        auto s = mirnext::codegen::detail::access (*(f)).intern_symbol (
          {"native", "prototype", MachineSymbol::Kind::Prototype, Signature{{}, {}}, false, {}});
        REQUIRE ((s).has_value ());
        operands.emplace_back (
          mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value ()));
      } else if (token == "L" && (code == Opcode::Call || code == Opcode::Inline)) {
        auto s = mirnext::codegen::detail::access (*(f)).intern_symbol (
          {"native", "callee", MachineSymbol::Kind::Import, {}, false, {}});
        REQUIRE ((s).has_value ());
        operands.emplace_back (
          mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value ()));
      } else if (token == "l" || token == "L")
        operands.emplace_back ((label).value ());
      else if (token == "r") {
        char role = n < d->roles.size () ? d->roles[n] : 'i';
        auto physical
          = f->target ()
              .registers ()[role == 'f' || role == 'F' || role == 'd' || role == 'D' || role == 'e'
                                || role == 'E' || role == 'v' || role == 'V'
                              ? 32
                              : 0]
              .reg;
        operands.emplace_back (physical);
      } else if (token.front () == 'h') {
        unsigned n = 0;
        for (char c : token.substr (1)) n = n * 10 + unsigned (c - '0');
        operands.emplace_back (f->target ().registers ()[n].reg);
      } else if (token.front () == 'm' || token.front () == 'M') {
        auto tag = token.substr (1);
        Type type = Type::i64 ();
        if (tag == "f")
          type = Type (Type::Kind::F32);
        else if (tag == "d")
          type = Type (Type::Kind::F64);
        else if (tag == "ld")
          type = Type (Type::Kind::LongDouble);
        else if (tag == "v")
          type = Type::vector128 (Type::VectorElement::I8, 16);
        else {
          auto width = unsigned (tag.back () - '0');
          bool sign = tag.front () == 's';
          constexpr std::array signed_types{Type::Kind::I8, Type::Kind::I16, Type::Kind::I32,
                                            Type::Kind::I64};
          constexpr std::array unsigned_types{Type::Kind::U8, Type::Kind::U16, Type::Kind::U32,
                                              Type::Kind::U64};
          type = Type (sign ? signed_types[width] : unsigned_types[width]);
        }
        operands.push_back (memory (*f, type));
      } else if (token == "Zf")
        operands.emplace_back (0.0f);
      else if (token == "Zd")
        operands.emplace_back (0.0);
      else
        operands.emplace_back (std::uint64_t (0));
      if (end == spec.npos) break;
      spec.remove_prefix (end + 1);
    }
    auto i = rw.append (b, code, std::move (operands));
    INFO (((i).has_value () ? "" : i.error ().message));
    REQUIRE ((i).has_value ());
    auto result = has_instruction_pattern ((i).value ());
    REQUIRE ((result).has_value ());
    CHECK ((result).value ());
  }
}
TEST_CASE ("AArch64 matching preserves immediate shift zero and addressing boundaries") {
  if (!arm ()) return;
  auto f = function ();
  auto r = reg (*f);
  auto i = Type::i64 ();
  for (auto value : {0ULL, 4095ULL, 4096ULL, 4095ULL * 4096})
    CHECK (matches (*f, Opcode::Add,
                    {r, r, mirnext::codegen::detail::NodeAccess::make_MachineOperand (value)}));
  for (auto value : {std::uint64_t{4097}, std::uint64_t{4096} * 4096, UINT64_MAX})
    CHECK_FALSE (matches (*f, Opcode::Add,
                          {r, r,
                           mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                             std::uint64_t (value))}));
  CHECK (matches (*f, Opcode::Lsh,
                  {r, r,
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (63))}));
  CHECK_FALSE (
    matches (*f, Opcode::Lsh,
             {r, r,
              mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (64))}));
  CHECK_FALSE (
    matches (*f, Opcode::Rsh,
             {r, r,
              mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (-1))}));
  CHECK (matches (*f, Opcode::Rshs,
                  {r, r,
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (31))}));
  CHECK_FALSE (
    matches (*f, Opcode::Rshs,
             {r, r,
              mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (32))}));
  for (auto offset : {0, 8, 4095 * 8})
    CHECK (matches (*f, Opcode::Mov, {r, memory (*f, i, offset)}));
  for (auto offset : {-1, 1, 4096 * 8})
    CHECK_FALSE (matches (*f, Opcode::Mov, {r, memory (*f, i, offset)}));
  CHECK (matches (*f, Opcode::Mov, {r, memory (*f, i, 0, true, 1)}));
  CHECK (matches (*f, Opcode::Mov, {r, memory (*f, i, 0, true, 8)}));
  CHECK_FALSE (matches (*f, Opcode::Mov, {r, memory (*f, i, 0, true, 4)}));
  CHECK_FALSE (matches (*f, Opcode::Mov, {r, memory (*f, i, 8, true, 8)}));
  auto d = reg (*f, Type (Type::Kind::F64));
  CHECK (matches (*f, Opcode::DEq,
                  {r, d, mirnext::codegen::detail::NodeAccess::make_MachineOperand (-0.0)}));
  CHECK_FALSE (matches (*f, Opcode::DEq,
                        {r, d, mirnext::codegen::detail::NodeAccess::make_MachineOperand (1.0)}));
  CHECK_FALSE (matches (*f, Opcode::DEq,
                        {r, d,
                         mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                           std::numeric_limits<double>::quiet_NaN ())}));
  auto s = mirnext::codegen::detail::access (*(f)).intern_symbol (
    {"native", "unresolved", MachineSymbol::Kind::Import, {}, false, {}});
  REQUIRE ((s).has_value ());
  CHECK (
    matches (*f, Opcode::Mov,
             {r, mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                   mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (),
                                                                                      19))}));
}
TEST_CASE ("AArch64 single memory replacement retains aliases and invalidates analyses") {
  if (!arm ()) return;
  auto f = function ();
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto r = reg (*f), m = memory (*f, Type::i64 ());
  auto add = rw.append (b, Opcode::Add, {r, r, m});
  REQUIRE ((add).has_value ());
  REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
  ready (*f);
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  REQUIRE ((legalize_operands (*f)).has_value ());
  CHECK_FALSE (graph.value ().current (*f));
  auto at = b.instructions ().begin ();
  CHECK (at->opcode () == MachineOpcode (Opcode::Mov));
  CHECK (at->operands ()[1] == m);
  CHECK (&*++at == &(add).value ());
  CHECK (mirnext::codegen::detail::operand_access (add.value ().operands ()[2])
           .get<const MachineVirtualRegister *> ());
  CHECK (f->state ().operands_legalized);
  REQUIRE ((f->verify ()).has_value ());
  auto revision = f->revision ();
  auto dump = f->dump ();
  CHECK_FALSE ((legalize_operands (*f)).has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->dump () == dump);
}
TEST_CASE ("AArch64 multiple memory fallback preserves both loads and the output store") {
  if (!arm ()) return;
  auto f = function ();
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto output = memory (*f, Type::i64 ()), left = memory (*f, Type::i64 ()),
       right = memory (*f, Type::i64 ());
  auto add = rw.append (b, Opcode::Add, {output, left, right});
  REQUIRE ((add).has_value ());
  REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
  ready (*f);
  REQUIRE ((legalize_operands (*f)).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  auto at = b.instructions ().begin ();
  CHECK (at->operands ()[1] == left);
  CHECK (at->operands ()[0] == add.value ().operands ()[1]);
  ++at;
  CHECK (at->operands ()[1] == right);
  CHECK (at->operands ()[0] == add.value ().operands ()[2]);
  ++at;
  CHECK (&*at == &(add).value ());
  ++at;
  CHECK (at->operands ()[0] == output);
  CHECK (at->operands ()[1] == add.value ().operands ()[0]);
  CHECK (f->instruction_count () == 5);
  for (auto &i : b.instructions ()) {
    auto result = has_instruction_pattern (i);
    REQUIRE ((result).has_value ());
    CHECK ((result).value ());
  }
}
TEST_CASE ("AArch64 legalizer diagnoses incorrect phase without mutation") {
  auto f = function ();
  auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
  auto before = f->dump ();
  auto revision = f->revision ();
  CHECK_FALSE ((legalize_operands (*f)).has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->dump () == before);
  auto state = f->state ();
  state.operands_legalized = true;
  CHECK_FALSE ((rw.set_state (state)).has_value ());
  CHECK (f->revision () == revision);
}
TEST_CASE ("AArch64 failed memory trials reuse scalar temporaries across instructions") {
  if (!arm ()) return;
  for (auto type : {Type::i64 (), Type (Type::Kind::F32), Type (Type::Kind::F64)}) {
    CAPTURE (type.kind ());
    auto f = function ();
    auto &b = mirnext::codegen::detail::access (*(f)).create_block ();
    MachineRewriter rw (*f);
    auto a = memory (*f, type), c = memory (*f, type), d = memory (*f, type), r = reg (*f, type);
    auto count = std::ranges::distance (f->registers ());
    auto code = type.kind () == Type::Kind::F32   ? Opcode::FAdd
                : type.kind () == Type::Kind::F64 ? Opcode::DAdd
                                                  : Opcode::Add;
    auto first = rw.append (b, code, {a, c, d});
    REQUIRE ((first).has_value ());
    auto second = rw.append (b, code, {a, c, d});
    REQUIRE ((second).has_value ());
    auto third = rw.append (b, code, {r, r, a});
    REQUIRE ((third).has_value ());
    REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
    ready (*f);
    REQUIRE ((legalize_operands (*f)).has_value ());
    REQUIRE ((f->verify ()).has_value ());
    // One failed trial register, six fallback registers. The final single
    // reload consumes that original trial register without allocating again.
    CHECK (std::ranges::distance (f->registers ()) == count + 7);
    CHECK (f->instruction_count () == 11);
    auto cfg = MachineGraphAnalysis::build (*f);
    REQUIRE ((cfg).has_value ());
    REQUIRE ((MachineLiveness::analyze (*f, (cfg).value ())).has_value ());
  }
}
TEST_CASE ("AArch64 switch memory input reloads before its control flow boundary") {
  if (!arm ()) return;
  auto f = function ();
  auto &entry = mirnext::codegen::detail::access (*(f)).create_block (),
       &exit = mirnext::codegen::detail::access (*(f)).create_block ();
  MachineRewriter rw (*f);
  auto label = mirnext::codegen::detail::access (*(f)).create_label (exit);
  REQUIRE ((label).has_value ());
  auto m = memory (*f, Type::i64 ());
  auto branch
    = rw.append (entry, Opcode::Switch,
                 {m, mirnext::codegen::detail::NodeAccess::make_MachineOperand ((label).value ())});
  REQUIRE ((branch).has_value ());
  REQUIRE ((rw.append (exit, Opcode::Ret, {})).has_value ());
  ready (*f);
  REQUIRE ((legalize_operands (*f)).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  auto first = entry.instructions ().begin ();
  CHECK (first->opcode () == MachineOpcode (Opcode::Mov));
  CHECK (first->operands ()[1] == m);
  CHECK (first->operands ()[0] == branch.value ().operands ()[0]);
  CHECK (&*++first == &(branch).value ());
  auto matched = has_instruction_pattern ((branch).value ());
  REQUIRE ((matched).has_value ());
  CHECK ((matched).value ());
  auto graph = MachineGraphAnalysis::build (*f);
  REQUIRE ((graph).has_value ());
  REQUIRE ((MachineLiveness::analyze (*f, (graph).value ())).has_value ());
}
