/* Licensed under the MIT License. x86 fields, owned artifacts and native bytes. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Detail/X86Encoding.hpp"
#include "mirnext/CodeGen/Detail/CompiledFunction.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include "X86Fixtures.hpp"
#include <bit>
#include <climits>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
using namespace mirnext::target;
namespace {
using R = X86Register;
using mirnext::codegen::detail::ArtifactBuilder;
using mirnext::codegen::detail::CompiledFunctionData;
std::uint64_t read (std::span<const std::uint8_t> bytes, std::size_t at, unsigned size = 8) {
  std::uint64_t out = 0;
  for (unsigned n = 0; n < size; ++n) out |= std::uint64_t (bytes[at + n]) << (n * 8);
  return out;
}
void write (std::vector<std::uint8_t> &bytes, std::size_t at, std::uint64_t value,
            unsigned size = 8) {
  for (unsigned n = 0; n < size; ++n, value >>= 8) bytes[at + n] = value & 255;
}
CompiledFunctionData call_fixture (bool jump = false) {
  CompiledFunctionData d ("fixture", "call", Architecture::X86_64, {});
  d.bytes_.resize (32);
  d.bytes_[0] = 0xff;
  d.bytes_[1] = jump ? 0x25 : 0x15;
  write (d.bytes_, 2, 10, 4);
  d.constants_.push_back ({16, 2, 6, true});
  d.labels_.push_back ({"entry", 0, 0});
  d.relocations_.push_back ({CodeRelocation::Kind::AbsoluteSymbol, 16, 0,
                             MachineSymbol{"m", "callee", MachineSymbol::Kind::Import}});
  return d;
}
CompiledFunction finish (CompiledFunctionData d) {
  auto r = ArtifactBuilder::finish (std::move (d));
  INFO (((r).has_value () ? "" : r.error ().message));
  REQUIRE ((r).has_value ());
  return std::move ((r).value ());
}
bool x86 () { return TargetMachine::native ().value ().architecture () == Architecture::X86_64; }
struct Fixture {
  MachineFunction f;
  MachineRewriter rw;
  MachineBasicBlock &entry;
  explicit Fixture (Signature signature = {{Type::i64 ()}, {}})
    : f ("native", "encode", std::move (signature), (TargetMachine::native ()).value ()),
      rw (f),
      entry (mirnext::codegen::detail::access (f).create_block ()) {}
  MachineOperand reg (R r) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
      f.target ().registers ()[static_cast<unsigned> (r)].reg);
  }
  void add (MachineOpcode op, std::vector<MachineOperand> operands) {
    REQUIRE ((rw.append (entry, op, std::move (operands))).has_value ());
  }
  MachineFrameResult frame () {
    auto state = f.state ();
    state.machinized = state.operands_legalized = true;
    REQUIRE ((rw.set_state (state)).has_value ());
    auto graph = MachineGraphAnalysis::build (f);
    REQUIRE ((graph).has_value ());
    auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = 0});
    REQUIRE ((live).has_value ());
    auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
    REQUIRE ((ranges).has_value ());
    auto alloc
      = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
    REQUIRE ((alloc).has_value ());
    auto rewritten = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                        std::move ((alloc).value ()));
    REQUIRE ((rewritten).has_value ());
    auto split = split_allocated_spills (f, std::move ((rewritten).value ()));
    REQUIRE ((split).has_value ());
    auto post = optimize_allocated_machine (f, std::move ((split).value ()));
    REQUIRE ((post).has_value ());
    auto frame = form_frame (f, std::move ((post).value ()));
    REQUIRE ((frame).has_value ());
    return std::move ((frame).value ());
  }
  CompiledFunction run (MachineEncodingOptions options = {}) {
    auto framed = frame ();
    auto before = f.dump ();
    auto revision = f.revision ();
    auto code = encode_machine (f, framed, std::move (options));
    INFO (((code).has_value () ? "" : code.error ().message));
    REQUIRE ((code).has_value ());
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
    return std::move ((code).value ());
  }
};
}  // namespace
TEST_CASE ("x86 address fields reconstruct every base index scale and signed displacement") {
  for (int b = -1; b < 16; ++b)
    for (int i = -1; i < 16; ++i)
      for (unsigned scale : {1, 2, 4, 8})
        for (std::int64_t disp :
             std::array<std::int64_t, 9>{INT32_MIN, -129, -128, -1, 0, 1, 127, 128, INT32_MAX}) {
          CAPTURE (b);
          CAPTURE (i);
          CAPTURE (scale);
          CAPTURE (disp);
          auto a
            = mirnext::codegen::detail::encode_x86_address (b < 0
                                                              ? std::nullopt
                                                              : std::optional (static_cast<R> (b)),
                                                            i < 0
                                                              ? std::nullopt
                                                              : std::optional (static_cast<R> (i)),
                                                            disp, scale);
          if (i == 4) {
            CHECK_FALSE ((a).has_value ());
            continue;
          }
          REQUIRE ((a).has_value ());
          CHECK (a.value ().rex_b == (b >= 8));
          CHECK (a.value ().rex_x == (i >= 8));
          CHECK (a.value ().sib == (i >= 0 || b < 0 || b == 4 || b == 12));
          CHECK (a.value ().rm == (a.value ().sib ? 4 : b % 8));
          const auto displacement
            = a.value ().displacement_bytes == 1
                ? std::int64_t (std::bit_cast<std::int8_t> (std::uint8_t (a.value ().displacement)))
              : a.value ().displacement_bytes == 4
                ? std::int64_t (std::bit_cast<std::int32_t> (a.value ().displacement))
                : 0;
          CHECK (displacement == disp);
          const auto value = [] (unsigned r) { return std::uint64_t{0x100000001} * (r + 1); };
          std::uint64_t address = displacement;
          if (a.value ().sib) {
            if (!(a.value ().mod == 0 && a.value ().base == 5))
              address += value (a.value ().base + 8 * a.value ().rex_b);
            if (a.value ().index != 4 || a.value ().rex_x)
              address += value (a.value ().index + 8 * a.value ().rex_x) << a.value ().scale;
          } else
            address += value (a.value ().rm + 8 * a.value ().rex_b);
          CHECK (address
                 == std::uint64_t (disp) + (b < 0 ? 0 : value (b))
                      + (i < 0 ? 0 : value (i) * scale));
          if (b < 0) {
            CHECK (a.value ().mod == 0);
            CHECK (a.value ().displacement_bytes == 4);
          } else if (!disp && b != 5 && b != 13) {
            CHECK (a.value ().mod == 0);
            CHECK (a.value ().displacement_bytes == 0);
          } else {
            CHECK (a.value ().mod == (disp >= -128 && disp <= 127 ? 1 : 2));
            CHECK (a.value ().displacement_bytes == (a.value ().mod == 1 ? 1 : 4));
          }
        }
  for (auto bad : {R::XMM0, R::ST0, static_cast<R> (999)}) {
    CHECK_FALSE ((mirnext::codegen::detail::encode_x86_address (bad, {}, 0)).has_value ());
    CHECK_FALSE ((mirnext::codegen::detail::encode_x86_address ({}, bad, 0)).has_value ());
  }
  for (auto bad : {0, 3, 16})
    CHECK_FALSE (
      (mirnext::codegen::detail::encode_x86_address (R::AX, R::CX, 0, bad)).has_value ());
  CHECK_FALSE (
    (mirnext::codegen::detail::encode_x86_address (R::AX, {}, std::int64_t (INT32_MIN) - 1))
      .has_value ());
  CHECK_FALSE (
    (mirnext::codegen::detail::encode_x86_address (R::AX, {}, std::int64_t (INT32_MAX) + 1))
      .has_value ());
}
TEST_CASE ("all x86 declarations have precompiled bounded encoding recipes") {
  auto rows = x86_instruction_patterns ();
  REQUIRE (rows.size () == 732);
  std::array<bool, static_cast<unsigned> (X86EncodingKind::Literal32) + 1> kinds{};
  for (auto &row : rows) {
    CAPTURE (row.index);
    auto recipe = x86_encoding_recipe (row.index);
    REQUIRE (recipe);
    REQUIRE_FALSE (recipe->words.empty ());
    std::size_t maximum = 0;
    for (auto &word : recipe->words) {
      CHECK (word.maximum_size > 0);
      REQUIRE (word.first_operation <= recipe->operations.size ());
      REQUIRE (word.operation_count <= recipe->operations.size () - word.first_operation);
      for (auto &op : recipe->operations.subspan (word.first_operation, word.operation_count))
        kinds[static_cast<unsigned> (op.kind)] = true;
      maximum += word.maximum_size;
    }
    CHECK (maximum == recipe->maximum_size);
  }
  CHECK_FALSE (x86_encoding_recipe (rows.size ()));
  CHECK_FALSE (x86_encoding_recipe (SIZE_MAX));
  // am and +h are supported by the original grammar but absent from this table.
  for (unsigned n = 0; n < kinds.size (); ++n)
    if (n != static_cast<unsigned> (X86EncodingKind::AddressScale)
        && n != static_cast<unsigned> (X86EncodingKind::OpcodePhysical)) {
      CAPTURE (n);
      CHECK (kinds[n]);
    }
}
TEST_CASE ("encoding retains only innermost loop header aliases from the allocation graph") {
  auto target = TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (unsigned level = 0; level < 4; ++level) {
    MachineFunction f ("m", "nested", {{Type::i64 ()}, {}}, (target).value ());
    MachineRewriter rw (f);
    auto outer_counter = mirnext::codegen::detail::access (f).create_register (Type::i64 ()),
         inner_counter = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
    REQUIRE ((outer_counter).has_value ());
    REQUIRE ((inner_counter).has_value ());
    MachineOperand x ((outer_counter).value ()), y ((inner_counter).value ()),
      one (std::int64_t{1}), two (std::int64_t{2}), zero (std::int64_t{0});
    auto &entry = mirnext::codegen::detail::access (f).create_block (),
         &outer = mirnext::codegen::detail::access (f).create_block (),
         &inner = mirnext::codegen::detail::access (f).create_block (),
         &back = mirnext::codegen::detail::access (f).create_block (),
         &exit = mirnext::codegen::detail::access (f).create_block ();
    auto outer_label = mirnext::codegen::detail::access (f).create_label (outer, "outer"),
         inner_label = mirnext::codegen::detail::access (f).create_label (inner, "inner"),
         alias = mirnext::codegen::detail::access (f).create_label (inner, "alias");
    REQUIRE ((outer_label).has_value ());
    REQUIRE ((inner_label).has_value ());
    REQUIRE ((alias).has_value ());
    REQUIRE ((rw.append (entry, Opcode::Mov, {x, two})).has_value ());
    REQUIRE ((rw.append (outer, Opcode::Mov, {y, two})).has_value ());
    REQUIRE ((rw.append (inner, Opcode::Sub, {y, y, one})).has_value ());
    REQUIRE ((rw.append (inner, Opcode::Bgt,
                         {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                            (inner_label).value ()),
                          y, zero}))
               .has_value ());
    REQUIRE ((rw.append (back, Opcode::Sub, {x, x, one})).has_value ());
    REQUIRE ((rw.append (back, Opcode::Bgt,
                         {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                            (outer_label).value ()),
                          x, zero}))
               .has_value ());
    REQUIRE ((rw.append (exit, Opcode::Ret, {x})).has_value ());
    auto state = f.state ();
    state.machinized = state.operands_legalized = true;
    REQUIRE ((rw.set_state (state)).has_value ());
    auto graph = MachineGraphAnalysis::build (f);
    REQUIRE ((graph).has_value ());
    REQUIRE (graph.value ().has_loops ());
    auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
    REQUIRE ((live).has_value ());
    auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
    REQUIRE ((ranges).has_value ());
    auto allocation
      = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
    REQUIRE ((allocation).has_value ());
    if (level >= 2) {
      REQUIRE (allocation.value ().loop_alignment_blocks ().size () == 1);
      CHECK (&allocation.value ().loop_alignment_blocks ()[0] == &inner);
    } else
      CHECK (allocation.value ().loop_alignment_blocks ().empty ());
    auto rewritten = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                        std::move ((allocation).value ()));
    REQUIRE ((rewritten).has_value ());
    auto spilled = split_allocated_spills (f, std::move ((rewritten).value ()));
    REQUIRE ((spilled).has_value ());
    auto post = optimize_allocated_machine (f, std::move ((spilled).value ()));
    REQUIRE ((post).has_value ());
    auto framed = form_frame (f, std::move ((post).value ()));
    REQUIRE ((framed).has_value ());
    if (level >= 2) {
      REQUIRE (framed.value ().aligned_loop_labels ().size () == 2);
      CHECK (&framed.value ().aligned_loop_labels ()[0] == &(inner_label).value ());
      CHECK (&framed.value ().aligned_loop_labels ()[1] == &(alias).value ());
    } else
      CHECK (framed.value ().aligned_loop_labels ().empty ());
  }
}
TEST_CASE ("owning x86 pool calls preserve exact signed rel32 boundaries and remain repeatable") {
  constexpr std::uint64_t base = 0x100000000ull;
  for (bool jump : {false, true})
    for (std::int64_t difference :
         std::array<std::int64_t, 7>{std::int64_t (INT32_MIN) - 1, INT32_MIN, -1, 0, 1, INT32_MAX,
                                     std::int64_t (INT32_MAX) + 1}) {
      CAPTURE (jump);
      CAPTURE (difference);
      auto code = finish (call_fixture (jump));
      auto original = std::vector (code.bytes ().begin (), code.bytes ().end ());
      unsigned calls = 0;
      auto target = base + 6 + std::uint64_t (difference);
      auto relocated = code.relocate (base, [&] (auto &request) -> Result<std::uint64_t> {
        ++calls;
        REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
        CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "callee");
        return target;
      });
      REQUIRE ((relocated).has_value ());
      CHECK (calls == 1);
      CHECK (read ((relocated).value (), 16) == target);
      if (difference >= INT32_MIN && difference <= INT32_MAX) {
        CHECK (((relocated).value ())[0] == 0x40);
        CHECK (((relocated).value ())[1] == (jump ? 0xe9 : 0xe8));
        CHECK (read ((relocated).value (), 2, 4) == std::uint32_t (difference));
      } else {
        CHECK (((relocated).value ())[0] == 0xff);
        CHECK (((relocated).value ())[1] == (jump ? 0x25 : 0x15));
        CHECK (read ((relocated).value (), 2, 4) == 10);
      }
      CHECK (original == std::vector (code.bytes ().begin (), code.bytes ().end ()));
      CHECK ((code.verify ()).has_value ());
      CHECK_FALSE ((code.relocate (base)).has_value ());
      auto label = code.find_label ("entry");
      REQUIRE ((label).has_value ());
      LabelAddress ref{(label).value ()};
      auto addresses = code.label_addresses (base, std::span (&ref, 1));
      REQUIRE ((addresses).has_value ());
      CHECK (((addresses).value ())[0] == base);
      auto other = finish (call_fixture ());
      CHECK_FALSE ((other.label_addresses (base, std::span (&ref, 1))).has_value ());
      CHECK_FALSE ((code.relocate (UINT64_MAX - 16)).has_value ());
    }
}
TEST_CASE (
  "owning x86 artifact relocations copy identities support unaligned immediates and share "
  "resolutions") {
  CompiledFunctionData d ("fixture", "symbols", Architecture::X86_64, {});
  d.bytes_.resize (48);
  d.labels_.push_back ({"target", 7, 2});
  MachineSymbol symbol{"owner", "data", MachineSymbol::Kind::Data};
  d.relocations_.push_back ({CodeRelocation::Kind::AbsoluteSymbol, 2, 0, symbol, {}, -7});
  d.relocations_.push_back ({CodeRelocation::Kind::AbsoluteSymbol, 12, 0, symbol, {}, 9});
  d.relocations_.push_back ({CodeRelocation::Kind::AbsoluteSymbol, 22, 0, {}, Builtin::CopyBlock});
  write (d.bytes_, 32, 7);
  d.relocations_.push_back ({CodeRelocation::Kind::LocalAddress, 32});
  auto code = finish (std::move (d));
  symbol.name = "destroyed";
  unsigned calls = 0;
  auto bytes = code.relocate (3, [&] (auto &request) -> Result<std::uint64_t> {
    ++calls;
    if (mirnext::codegen::detail::NodeAccess::symbol (request)) {
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "data");
      CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->module == "owner");
      return 100;
    }
    REQUIRE (request.builtin);
    CHECK (*request.builtin == Builtin::CopyBlock);
    return 200;
  });
  REQUIRE ((bytes).has_value ());
  CHECK (calls == 2);
  CHECK (read ((bytes).value (), 2) == 93);
  CHECK (read ((bytes).value (), 12) == 109);
  CHECK (read ((bytes).value (), 22) == 200);
  CHECK (read ((bytes).value (), 32) == 10);
  auto fail = code.relocate (0, [] (auto &) -> Result<std::uint64_t> {
    return Error{ErrorCode::UnknownName, "absent symbol"};
  });
  REQUIRE_FALSE ((fail).has_value ());
  CHECK (fail.error ().code == ErrorCode::UnknownName);
  CHECK (fail.error ().message.find ("fixture/symbols") != std::string::npos);
  if (!x86 ()) {
    auto r = execution::CodeMemory ().publish (code);
    REQUIRE_FALSE ((r).has_value ());
    CHECK (r.error ().message.find ("another architecture") != std::string::npos);
  }
}
TEST_CASE ("compiled artifact publication rejects malformed x86 fixups and pool metadata") {
  for (unsigned fault = 0; fault < 22; ++fault) {
    CAPTURE (fault);
    auto d = call_fixture ();
    switch (fault) {
    case 0: d.bytes_.clear (); break;
    case 1: d.bytes_.pop_back (); break;
    case 2: d.labels_[0].offset = 33; break;
    case 3: d.labels_.push_back ({"duplicate", 1, 0}); break;
    case 4: d.relocations_[0].offset = 30; break;
    case 5: d.relocations_[0].symbol.reset (); break;
    case 6: d.relocations_[0].builtin = Builtin::CopyBlock; break;
    case 7: d.relocations_[0].shift = 16; break;
    case 8: d.bytes_[16] = 1; break;
    case 9: d.relocations_.push_back (d.relocations_[0]); break;
    case 10: d.constants_[0].offset = 17; break;
    case 11: d.constants_[0].reference_offset = SIZE_MAX; break;
    case 12: d.constants_[0].next_instruction = 7; break;
    case 13: d.bytes_[24] = 1; break;
    case 14: d.bytes_[0] = 0x40; break;
    case 15: d.bytes_[1] = 0x14; break;
    case 16: d.labels_[0].offset = 17; break;
    case 17: d.bytes_.resize (48); break;
    case 18: d.relocations_[0].offset = 24; break;
    case 19: d.relocations_[0].offset = 12; break;
    case 20: d.labels_.push_back ({"entry", 1, 1}); break;
    case 21: d.relocations_[0].offset = 4; break;
    }
    CHECK_FALSE ((ArtifactBuilder::finish (std::move (d))).has_value ());
  }
}
TEST_CASE ("native x86 encoder emits exact immediate SIB byte-register and x87 forms") {
  if (!x86 ()) return;
  {
    Fixture t;
    t.add (Opcode::Mov, {t.reg (R::AX), mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                          std::uint64_t{42})});
    t.add (Opcode::Ret, {t.reg (R::AX)});
    auto code = t.run ();
    const std::uint8_t expected[]{0x48, 0xc7, 0xc0, 42, 0, 0, 0, 0xc3};
    CHECK (std::equal (std::begin (expected), std::end (expected), code.bytes ().begin ()));
    auto executable = execution::CodeMemory ().publish (code);
    REQUIRE ((executable).has_value ());
    CHECK (reinterpret_cast<std::int64_t (*) ()> (executable.value ().address ()) () == 42);
  }
  {
    Fixture t;
    t.add (Opcode::Mov,
           {t.reg (R::AX), mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                             mirnext::codegen::detail::NodeAccess::
                               make_MachineMemory (Type::i64 (), t.f.target ().registers ()[12].reg,
                                                   t.f.target ().registers ()[9].reg, -129, 8))});
    t.add (Opcode::Ret, {t.reg (R::AX)});
    auto code = t.run ();
    const std::uint8_t expected[]{0x4b, 0x8b, 0x84, 0xcc, 0x7f, 0xff, 0xff, 0xff};
    CHECK (std::search (code.bytes ().begin (), code.bytes ().end (), std::begin (expected),
                        std::end (expected))
           != code.bytes ().end ());
  }
  {
    Fixture t ({{Type (Type::Kind::LongDouble)}, {}});
    t.add (Opcode::LDMov,
           {t.reg (R::ST0), mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                              mirnext::codegen::detail::NodeAccess::
                                make_MachineMemory (Type (Type::Kind::LongDouble),
                                                    t.f.target ().registers ()[4].reg, {}, -16))});
    t.add (Opcode::Ret, {t.reg (R::ST0)});
    auto code = t.run ();
    const std::uint8_t expected[]{0xdb, 0x6c, 0x24, 0xf0};
    CHECK (std::search (code.bytes ().begin (), code.bytes ().end (), std::begin (expected),
                        std::end (expected))
           != code.bytes ().end ());
  }
}
TEST_CASE (
  "native x86 complete compilation covers public opcodes and ABI fixtures at every level") {
  if (!x86 ()) return;
  auto target = TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto cases = mirnext::test::x86_lowering_fixtures (target.value ().abi () == ABI::X86Windows);
  for (auto &d : instruction_descriptors ()) {
    auto fixture = mirnext::test::public_instruction_example (d);
    auto text = serialization::write_text (*fixture);
    REQUIRE ((text).has_value ());
    cases.push_back ((text).value ());
  }
  for (auto &text : cases)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (level);
      INFO (text);
      auto context = serialization::parse_text (text);
      REQUIRE ((context).has_value ());
      transforms::OptimizationOptions options;
      options.level = static_cast<transforms::OptimizationLevel> (level);
      auto program = transforms::optimize ((context).value (), options);
      REQUIRE ((program).has_value ());
      for (auto &module : program.value ().program ().modules ())
        for (auto &function : module.functions ()) {
          auto compiled
            = compile_function (function, (target).value (), {.optimization_level = level});
          INFO (((compiled).has_value () ? "" : compiled.error ().message));
          REQUIRE ((compiled).has_value ());
          REQUIRE ((compiled.value ().verify ()).has_value ());
          auto relocated = compiled.value ().relocate (0, [] (auto &) -> Result<std::uint64_t> {
            return 0x123456789abcdef0ull;
          });
          REQUIRE ((relocated).has_value ());
        }
    }
}
TEST_CASE ("native x86 encoder retains duplicate pool cells and typed symbol fixups") {
  if (!x86 ()) return;
  Fixture t ({{Type::i64 (), Type (Type::Kind::F64)}, {}});
  auto s = mirnext::codegen::detail::access (t.f).intern_symbol (
    {"source", "data", MachineSymbol::Kind::Data});
  REQUIRE ((s).has_value ());
  t.add (Opcode::Mov,
         {t.reg (R::AX),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand (
            mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (),
                                                                               -7))});
  t.add (Opcode::DNeg, {t.reg (R::XMM0), t.reg (R::XMM0)});
  t.add (Opcode::DNeg, {t.reg (R::XMM0), t.reg (R::XMM0)});
  t.add (Opcode::Ret, {t.reg (R::AX), t.reg (R::XMM0)});
  auto code = t.run ();
  REQUIRE (code.relocations ().size () == 1);
  CHECK (code.relocations ()[0].kind == CodeRelocation::Kind::AbsoluteSymbol);
  REQUIRE (code.constant_pool ().size () == 2);
  auto a = code.constant_pool ()[0], b = code.constant_pool ()[1];
  CHECK (b.offset == a.offset + 16);
  CHECK (read (code.bytes (), a.offset) == 0x8000000000000000ull);
  CHECK (read (code.bytes (), b.offset) == 0x8000000000000000ull);
  CHECK (read (code.bytes (), a.reference_offset, 4) == a.offset - a.next_instruction);
  CHECK_FALSE (a.call);
  auto resolved = code.relocate (0, [] (auto &request) -> Result<std::uint64_t> {
    REQUIRE (mirnext::codegen::detail::NodeAccess::symbol (request));
    CHECK (mirnext::codegen::detail::NodeAccess::symbol (request)->name == "data");
    return 49;
  });
  REQUIRE ((resolved).has_value ());
  CHECK (read ((resolved).value (), code.relocations ()[0].offset) == 42);
}
TEST_CASE (
  "native x86 address resolution fails before publishing and leaves source identities intact") {
  if (!x86 ()) return;
  Fixture t;
  auto s = mirnext::codegen::detail::access (t.f).intern_symbol (
    {"m", "known", MachineSymbol::Kind::Data});
  REQUIRE ((s).has_value ());
  t.add (Opcode::Mov,
         {t.reg (R::AX),
          mirnext::codegen::detail::NodeAccess::make_MachineOperand (
            mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (&(s).value (), 8))});
  t.add (Opcode::Ret, {t.reg (R::AX)});
  auto framed = t.frame ();
  auto before = t.f.dump ();
  unsigned calls = 0;
  auto code
    = encode_machine (t.f, framed, {.known_addresses = [&] (auto &) -> Result<std::uint64_t> {
                        ++calls;
                        return 34;
                      }});
  REQUIRE ((code).has_value ());
  CHECK (calls == 1);
  CHECK (code.value ().relocations ().empty ());
  CHECK (t.f.dump () == before);
  const std::uint8_t expected[]{0x48, 0xc7, 0xc0, 42, 0, 0, 0};
  CHECK (std::equal (std::begin (expected), std::end (expected), code.value ().bytes ().begin ()));
  auto failed
    = encode_machine (t.f, framed, {.known_addresses = [] (auto &) -> Result<std::uint64_t> {
                        return Error{ErrorCode::UnknownName, "address unavailable"};
                      }});
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().code == ErrorCode::UnknownName);
  CHECK (failed.error ().message.find ("native/encode") != std::string::npos);
  CHECK (failed.error ().message.find ("encode") != std::string::npos);
  CHECK (t.f.dump () == before);
  CHECK (s.value ().name == "known");
}
TEST_CASE ("native x86 encoder diagnoses rounded stack immediates and stale frames") {
  if (!x86 ()) return;
  for (auto size : {std::uint64_t (INT32_MAX), UINT64_MAX}) {
    Fixture t;
    t.add (Opcode::Alloca,
           {t.reg (R::AX), mirnext::codegen::detail::NodeAccess::make_MachineOperand (size)});
    t.add (Opcode::Ret, {t.reg (R::AX)});
    auto framed = t.frame ();
    auto before = t.f.dump ();
    auto code = encode_machine (t.f, framed);
    REQUIRE_FALSE ((code).has_value ());
    CHECK (code.error ().message.find ("encode") != std::string::npos);
    CHECK (t.f.dump () == before);
  }
  Fixture t;
  t.add (Opcode::Ret, {t.reg (R::AX)});
  auto framed = t.frame ();
  REQUIRE ((mirnext::codegen::detail::access (t.f).create_label (t.entry)).has_value ());
  CHECK_FALSE ((encode_machine (t.f, framed)).has_value ());
}
