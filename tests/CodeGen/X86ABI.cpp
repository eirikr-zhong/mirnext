/* Licensed under the MIT License. Pure ABI plans and native owning lowering. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Target/X86ABI.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "../IR/PublicInstructions.hpp"
#include "X86Fixtures.hpp"
#include <limits>

using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::target;
using namespace mirnext::codegen;
namespace {
using R = X86Register;
using K = Type::Kind;
X86ArgumentLayout layout (ABI abi, std::initializer_list<Type> types) {
  auto result = layout_x86_arguments (abi, std::span (types.begin (), types.size ()));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::move ((result).value ());
}
void location (const X86ArgumentLocation &loc, std::initializer_list<R> regs,
               std::size_t offset = 0) {
  REQUIRE (loc.register_count == regs.size ());
  unsigned n = 0;
  for (auto r : regs) CHECK (loc.registers[n++] == r);
  if (!regs.size ()) CHECK (loc.stack_offset == offset);
}
bool x86 () { return TargetMachine::native ().value ().architecture () == Architecture::X86_64; }
std::unique_ptr<MachineFunction> machine (std::string_view text, unsigned level) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto p = transforms::optimize ((c).value (), options);
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  auto t = TargetMachine::native ();
  REQUIRE ((t).has_value ());
  auto f = p.value ().find_function ("m", "f").get ();
  REQUIRE (f);
  auto result = prepare_machine_ir (*f, (t).value ());
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  return std::make_unique<MachineFunction> (std::move ((result).value ()));
}
}  // namespace
TEST_CASE ("x86 ABI scalar register banks exhaust independently on SysV and share Windows slots") {
  auto i = Type::i64 (), f = Type (K::F32), d = Type (K::F64), ld = Type (K::LongDouble);
  auto sysv = layout (ABI::X86SysV, {i, f, i, d, i, i, i, i, i, d});
  location (sysv.arguments[0], {R::DI});
  location (sysv.arguments[1], {R::XMM0});
  location (sysv.arguments[2], {R::SI});
  location (sysv.arguments[3], {R::XMM1});
  location (sysv.arguments[4], {R::DX});
  location (sysv.arguments[5], {R::CX});
  location (sysv.arguments[6], {R::R8});
  location (sysv.arguments[7], {R::R9});
  location (sysv.arguments[8], {}, 0);
  location (sysv.arguments[9], {R::XMM2});
  CHECK (sysv.stack_bytes == 8);
  CHECK (sysv.integer_count == 7);
  CHECK (sysv.floating_count == 3);
  CHECK (sysv.variadic_xmm_count == 3);
  auto win = layout (ABI::X86Windows, {i, f, i, d, i, d});
  location (win.arguments[0], {R::CX});
  location (win.arguments[1], {R::XMM1});
  location (win.arguments[2], {R::R8});
  location (win.arguments[3], {R::XMM3});
  location (win.arguments[4], {}, 32);
  location (win.arguments[5], {}, 40);
  CHECK (win.stack_bytes == 48);
  CHECK (win.integer_count == 6);
  CHECK (win.floating_count == 6);
  auto extended = layout (ABI::X86SysV, {ld, i, ld, d});
  location (extended.arguments[0], {}, 0);
  location (extended.arguments[1], {R::DI});
  location (extended.arguments[2], {}, 16);
  location (extended.arguments[3], {R::XMM0});
  CHECK (extended.stack_bytes == 32);
  std::vector<Type> many (20, d);
  auto all = layout_x86_arguments (ABI::X86SysV, many);
  REQUIRE ((all).has_value ());
  CHECK (all.value ().variadic_xmm_count == 8);
  CHECK (all.value ().stack_bytes == 96);
  location (all.value ().arguments[8], {}, 0);
  location (all.value ().arguments[19], {}, 88);
  CHECK (layout (ABI::X86Windows, {}).stack_bytes == 32);
  CHECK (layout (ABI::X86SysV, {}).stack_bytes == 0);
}

TEST_CASE ("x86 v128 uses SysV XMM and aligned stack slots or Windows indirect copies") {
  auto type = Type::vector128 (Type::VectorElement::F32, 4);
  std::vector<Type> arguments (8, type);
  arguments.push_back (Type::i64 ());
  for (unsigned n = 0; n < 7; ++n) arguments.push_back (Type::i64 ());
  arguments.push_back (type);
  auto sysv = layout_x86_arguments (ABI::X86SysV, arguments);
  REQUIRE (sysv.has_value ());
  for (unsigned n = 0; n < 8; ++n) {
    location (sysv.value ().arguments[n], {static_cast<R> (unsigned (R::XMM0) + n)});
    CHECK_FALSE (sysv.value ().arguments[n].indirect);
  }
  location (sysv.value ().arguments.back (), {}, 16);
  CHECK (sysv.value ().stack_bytes == 32);
  auto win = layout (ABI::X86Windows, {type, Type::i64 (), type, type, type});
  location (win.arguments[0], {R::CX});
  location (win.arguments[1], {R::DX});
  location (win.arguments[2], {R::R8});
  location (win.arguments[3], {R::R9});
  location (win.arguments[4], {}, 32);
  for (unsigned n : {0, 2, 3, 4}) CHECK (win.arguments[n].indirect);
  CHECK_FALSE (win.arguments[1].indirect);
  CHECK (win.stack_bytes == 40);
}
TEST_CASE ("x86 ABI all five block classifications retain ordered register pieces") {
  for (std::size_t size : {1u, 8u, 9u, 16u}) {
    auto a = layout (ABI::X86SysV, {Type::block (size, Type::BlockClass::Class1),
                                    Type::block (size, Type::BlockClass::Class2)});
    if (size <= 8) {
      location (a.arguments[0], {R::DI});
      location (a.arguments[1], {R::XMM0});
    } else {
      location (a.arguments[0], {R::DI, R::SI});
      location (a.arguments[1], {R::XMM0, R::XMM1});
    }
    CHECK (a.stack_bytes == 0);
    CHECK (a.arguments[0].aggregate);
    CHECK (a.arguments[0].by_value);
  }
  auto mixed = layout (ABI::X86SysV, {Type::block (16, Type::BlockClass::Class3),
                                      Type::block (16, Type::BlockClass::Class4)});
  location (mixed.arguments[0], {R::DI, R::XMM0});
  location (mixed.arguments[1], {R::XMM1, R::SI});
  for (std::size_t size : {1u, 8u, 9u, 16u, 17u, 128u, 129u}) {
    auto a = layout (ABI::X86SysV, {Type::block (size), Type::i64 ()});
    location (a.arguments[0], {}, 0);
    location (a.arguments[1], {R::DI});
    CHECK (a.stack_bytes == (size + 7) / 8 * 8);
  }
  for (auto classification :
       {Type::BlockClass::Default, Type::BlockClass::Class1, Type::BlockClass::Class2,
        Type::BlockClass::Class3, Type::BlockClass::Class4}) {
    auto a = layout (ABI::X86Windows, {Type::block (16, classification), Type::i64 ()});
    location (a.arguments[0], {R::CX});
    location (a.arguments[1], {R::DX});
    CHECK_FALSE (a.arguments[0].by_value);
    CHECK (a.stack_bytes == 32);
  }
}
TEST_CASE ("x86 ABI failed aggregate register assignment consumes neither register bank") {
  auto i = Type::i64 (), d = Type (K::F64);
  auto pair = Type::block (16, Type::BlockClass::Class1);
  auto a = layout (ABI::X86SysV, {i, i, i, i, i, pair, i});
  location (a.arguments[5], {}, 0);
  location (a.arguments[6], {R::R9});
  CHECK (a.stack_bytes == 16);
  std::vector<Type> types (7, d);
  types.push_back (Type::block (16, Type::BlockClass::Class2));
  types.push_back (d);
  auto floats = layout_x86_arguments (ABI::X86SysV, types);
  REQUIRE ((floats).has_value ());
  location (floats.value ().arguments[7], {}, 0);
  location (floats.value ().arguments[8], {R::XMM7});
  types.assign (6, i);
  types.push_back (Type::block (16, Type::BlockClass::Class3));
  types.push_back (d);
  auto mixed = layout_x86_arguments (ABI::X86SysV, types);
  REQUIRE ((mixed).has_value ());
  location (mixed.value ().arguments[6], {}, 0);
  location (mixed.value ().arguments[7], {R::XMM0});
  auto win = layout (ABI::X86Windows, {i, d, i, d, Type::block (8), Type::block (24)});
  location (win.arguments[4], {}, 32);
  CHECK (win.arguments[4].by_value);
  location (win.arguments[5], {}, 40);
  CHECK_FALSE (win.arguments[5].by_value);
  CHECK (win.stack_bytes == 48);
}
TEST_CASE ("x86 ABI hidden return-buffer addresses use ordinary integer parameter slots") {
  auto r = Type::return_block (24), i = Type::i64 ();
  auto a = layout (ABI::X86SysV, {r, i});
  location (a.arguments[0], {R::DI});
  location (a.arguments[1], {R::SI});
  CHECK_FALSE (a.arguments[0].aggregate);
  auto b = layout (ABI::X86Windows, {r, i});
  location (b.arguments[0], {R::CX});
  location (b.arguments[1], {R::DX});
  CHECK_FALSE (b.arguments[0].aggregate);
  auto overflow = layout (ABI::X86SysV, {i, i, i, i, i, i, r});
  location (overflow.arguments[6], {}, 0);
  CHECK (overflow.stack_bytes == 8);
}
TEST_CASE ("x86 ABI spills preserve frame direction variadic save areas and reload banks") {
  for (auto abi : {ABI::X86SysV, ABI::X86Windows})
    for (bool fp : {false, true})
      for (bool variadic : {false, true}) {
        for (auto kind : {K::I8, K::I64, K::F32, K::F64, K::LongDouble})
          for (std::size_t offset : {0u, 8u, 16u, 1024u, 32768u}) {
            auto type = Type (kind);
            auto disp = x86_spill_displacement (abi, fp, variadic, type, offset);
            REQUIRE ((disp).has_value ());
            CHECK (
              (disp).value ()
              == (fp ? -static_cast<std::int64_t> (offset + (kind == K::LongDouble ? 16 : 8)
                                                   + (variadic && abi == ABI::X86SysV ? 176 : 0))
                     : static_cast<std::int64_t> (offset)));
            for (bool first : {false, true}) {
              auto reg = x86_reload_register (abi, type, first);
              if (kind == K::LongDouble)
                CHECK_FALSE (reg);
              else {
                REQUIRE (reg);
                CHECK (*reg
                       == (type.is_float () ? abi == ABI::X86Windows ? (first ? R::XMM4 : R::XMM5)
                                                                     : (first ? R::XMM8 : R::XMM9)
                           : first          ? R::R10
                                            : R::R11));
              }
            }
          }
        auto edge = x86_spill_displacement (abi, fp, variadic, Type::i64 (),
                                            std::size_t (INT32_MAX) / 8 * 8);
        REQUIRE ((edge).has_value ());
        CHECK_FALSE (
          (x86_spill_displacement (abi, fp, variadic, Type::i64 (), SIZE_MAX)).has_value ());
        CHECK_FALSE ((x86_spill_displacement (abi, fp, variadic, Type::i64 (),
                                              std::size_t (INT64_MAX) / 8 * 8))
                       .has_value ());
        CHECK_FALSE ((x86_spill_displacement (abi, fp, variadic, Type::i64 (), 1)).has_value ());
        CHECK_FALSE ((x86_spill_displacement (abi, fp, variadic, Type::block (8), 0)).has_value ());
        CHECK_FALSE (x86_reload_register (abi, Type (K::Undef), true));
      }
  CHECK_FALSE (
    (x86_spill_displacement (ABI::AArch64Linux, true, false, Type::i64 (), 0)).has_value ());
  CHECK_FALSE (x86_reload_register (ABI::AArch64MacOS, Type::i64 (), true));
}
TEST_CASE ("x86 ABI result banks and unsupported layouts return diagnostics") {
  std::vector<Type> types{Type::i64 (), Type (K::F32), Type (K::LongDouble),
                          Type::i64 (), Type (K::F64), Type (K::LongDouble)};
  auto result = layout_x86_results (ABI::X86SysV, types);
  REQUIRE ((result).has_value ());
  CHECK ((result).value () == std::vector<R>{R::AX, R::XMM0, R::ST0, R::DX, R::XMM1, R::ST1});
  CHECK_FALSE ((layout_x86_results (ABI::X86Windows, types)).has_value ());
  for (auto kind : {K::I64, K::F64, K::LongDouble}) {
    types.assign (3, Type (kind));
    CHECK_FALSE ((layout_x86_results (ABI::X86SysV, types)).has_value ());
  }
  for (auto invalid : {Type::block (8), Type (K::Undef), Type (static_cast<K> (65535))}) {
    types.assign (1, invalid);
    CHECK_FALSE ((layout_x86_results (ABI::X86SysV, types)).has_value ());
  }
  CHECK_FALSE ((layout_x86_arguments (ABI::AArch64Linux, {})).has_value ());
  CHECK_FALSE ((layout_x86_results (ABI::AArch64MacOS, {})).has_value ());
  CHECK_FALSE (x86_integer_argument_register (ABI::X86SysV, 6));
  CHECK_FALSE (x86_floating_argument_register (ABI::X86Windows, 4));
  CHECK_FALSE (x86_integer_argument_register (ABI::AArch64Linux, 0));
  CHECK_FALSE (x86_floating_argument_register (ABI::X86SysV, SIZE_MAX));
  for (auto bad :
       {Type::block (0), Type::block (SIZE_MAX), Type::block (17, Type::BlockClass::Class1),
        Type::block (8, Type::BlockClass::Class3), Type (K::Undef)}) {
    types.assign (1, bad);
    CHECK_FALSE ((layout_x86_arguments (ABI::X86SysV, types)).has_value ());
  }
  types.assign (2, Type::block (std::size_t (INT64_MAX) - 63));
  CHECK_FALSE ((layout_x86_arguments (ABI::X86SysV, types)).has_value ());
}
TEST_CASE (
  "native x86 owning ABI lowering and legalization cover public opcodes and boundary fixtures") {
  if (!x86 ()) return;
  auto target = TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (auto &d : instruction_descriptors ())
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (d.name);
      CAPTURE (level);
      auto source = test::public_instruction_example (d);
      transforms::OptimizationOptions options;
      options.level = static_cast<transforms::OptimizationLevel> (level);
      auto p = transforms::optimize (*source, options);
      REQUIRE ((p).has_value ());
      for (auto &module : p.value ().program ().modules ())
        for (auto &f : module.functions ()) {
          auto m = prepare_machine_ir (f, (target).value ());
          REQUIRE ((m).has_value ());
          auto r = machinize ((m).value ());
          INFO (((r).has_value () ? "" : r.error ().message));
          REQUIRE ((r).has_value ());
          REQUIRE ((m.value ().verify ()).has_value ());
          REQUIRE ((legalize_operands ((m).value ())).has_value ());
          REQUIRE ((m.value ().verify ()).has_value ());
          CHECK (m.value ().state ().machinized);
        }
    }
  for (auto &text : test::x86_lowering_fixtures (target.value ().abi () == ABI::X86Windows))
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (text);
      CAPTURE (level);
      auto m = machine (text, level);
      auto r = machinize (*m);
      INFO (((r).has_value () ? "" : r.error ().message));
      REQUIRE ((r).has_value ());
      auto lowered = m->dump ();
      REQUIRE ((m->verify ()).has_value ());
      auto revision = m->revision ();
      CHECK_FALSE ((machinize (*m)).has_value ());
      CHECK (m->revision () == revision);
      CHECK (m->dump () == lowered);
      REQUIRE ((legalize_operands (*m)).has_value ());
      REQUIRE ((m->verify ()).has_value ());
    }
}
TEST_CASE ("x86 ABI regression fixtures prepare independently owned IR on every host") {
  for (bool windows : {false, true})
    for (auto &text : test::x86_lowering_fixtures (windows))
      for (unsigned level = 0; level < 4; ++level) {
        CAPTURE (windows);
        CAPTURE (level);
        CAPTURE (text);
        auto m = machine (text, level);
        REQUIRE ((m->verify ()).has_value ());
      }
}
TEST_CASE ("native x86 invalid ABI inputs fail before modifying machine nodes") {
  if (!x86 ()) return;
  auto target = TargetMachine::native ();
  REQUIRE ((target).has_value ());
  MachineFunction f ("bad", "results", {{Type::i64 (), Type::i64 (), Type::i64 ()}, {}},
                     (target).value ());
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter rw (f);
  auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
  REQUIRE ((r).has_value ());
  REQUIRE ((rw.append (b, Opcode::Ret,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ())}))
             .has_value ());
  auto before = f.dump ();
  auto revision = f.revision ();
  auto lowered = machinize (f);
  REQUIRE_FALSE ((lowered).has_value ());
  CHECK (lowered.error ().message.find ("machinize") != std::string::npos);
  CHECK (f.dump () == before);
  CHECK (f.revision () == revision);
}
