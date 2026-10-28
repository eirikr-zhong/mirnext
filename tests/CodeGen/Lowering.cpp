/* Licensed under the MIT License. Target lowering over owning machine IR. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "../IR/PublicInstructions.hpp"
#include "AArch64Fixtures.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
std::unique_ptr<MachineFunction> input (std::string_view text, unsigned level = 2) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto p = transforms::optimize ((c).value (), options);
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  auto f = p.value ().find_function ("m", "f").get ();
  REQUIRE (f);
  auto m = prepare_machine_ir (*f, (target).value ());
  INFO (((m).has_value () ? "" : m.error ().message));
  REQUIRE ((m).has_value ());
  return std::make_unique<MachineFunction> (std::move ((m).value ()));
}
bool arm () {
  return target::TargetMachine::native ().value ().architecture () == target::Architecture::AArch64;
}
}  // namespace
TEST_CASE ("machine instruction navigation and insertion preserve owners and revisions") {
  auto f = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  auto &block = *f->blocks ().begin ();
  auto &first = *block.instructions ().begin ();
  MachineRewriter rw (*f);
  REQUIRE ((rw.previous (first)).has_value ());
  CHECK (rw.previous (first).value ().get () == nullptr);
  auto inserted = rw.insert_after (first, MachinePseudoOpcode::Use, {});
  REQUIRE ((inserted).has_value ());
  CHECK (rw.next (first).value ().get () == &(inserted).value ());
  CHECK (rw.previous ((inserted).value ()).value ().get () == &first);
  auto other = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  MachineRewriter foreign (*other);
  auto revision = other->revision ();
  CHECK_FALSE ((foreign.previous (first)).has_value ());
  CHECK_FALSE ((foreign.next (first)).has_value ());
  CHECK_FALSE ((foreign.insert_after (first, MachinePseudoOpcode::Use, {})).has_value ());
  CHECK (other->revision () == revision);
  REQUIRE ((rw.erase ((inserted).value ())).has_value ());
  REQUIRE ((f->verify ()).has_value ());
}
TEST_CASE ("AArch64 target lowering covers every public opcode") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
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
          auto machine = prepare_machine_ir (f, (target).value ());
          REQUIRE ((machine).has_value ());
          auto lowered = machinize ((machine).value ());
          INFO (((lowered).has_value () ? "" : lowered.error ().message));
          REQUIRE ((lowered).has_value ());
          REQUIRE ((machine.value ().verify ()).has_value ());
          CHECK (machine.value ().state ().machinized);
          CHECK_FALSE (machine.value ().dump ().empty ());
          REQUIRE ((legalize_operands ((machine).value ())).has_value ());
          CHECK (machine.value ().state ().operands_legalized);
          REQUIRE ((machine.value ().verify ()).has_value ());
          auto cfg = MachineGraphAnalysis::build ((machine).value ());
          REQUIRE ((cfg).has_value ());
          auto merged = coalesce_registers ((machine).value (), (cfg).value (), level);
          REQUIRE ((merged).has_value ());
          cfg = MachineGraphAnalysis::build ((machine).value ());
          REQUIRE ((cfg).has_value ());
          auto live = MachineLiveness::analyze ((machine).value (), (cfg).value (),
                                                {.optimization_level = level});
          REQUIRE ((live).has_value ());
          auto ranges
            = MachineLiveRanges::build ((machine).value (), (cfg).value (), (live).value ());
          REQUIRE ((ranges).has_value ());
          auto allocation = MachineAllocation::assign ((machine).value (), (cfg).value (),
                                                       (live).value (), (ranges).value ());
          REQUIRE ((allocation).has_value ());
          auto rewritten = rewrite_allocated ((machine).value (), (cfg).value (), (live).value (),
                                              (ranges).value (), std::move ((allocation).value ()));
          INFO (((rewritten).has_value () ? "" : rewritten.error ().message));
          REQUIRE ((rewritten).has_value ());
          REQUIRE ((machine.value ().verify ()).has_value ());
          auto spilled
            = split_allocated_spills ((machine).value (), std::move ((rewritten).value ()));
          INFO (((spilled).has_value () ? "" : spilled.error ().message));
          REQUIRE ((spilled).has_value ());
          REQUIRE ((spilled.value ().cfg ().verify ((machine).value ())).has_value ());
          auto post
            = optimize_allocated_machine ((machine).value (), std::move ((spilled).value ()));
          INFO (((post).has_value () ? "" : post.error ().message));
          REQUIRE ((post).has_value ());
          REQUIRE ((post.value ().cfg ().verify ((machine).value ())).has_value ());
          auto framed = form_frame ((machine).value (), std::move ((post).value ()));
          INFO (((framed).has_value () ? "" : framed.error ().message));
          REQUIRE ((framed).has_value ());
          REQUIRE ((framed.value ().cfg ().verify ((machine).value ())).has_value ());
          auto encoded = encode_machine ((machine).value (), (framed).value ());
          INFO (((encoded).has_value () ? "" : encoded.error ().message));
          REQUIRE ((encoded).has_value ());
          REQUIRE ((encoded.value ().verify ()).has_value ());
        }
    }
}
TEST_CASE ("lowering owns state after source destruction and rejects a repeated invocation") {
  auto f = input (
    "m:module\np:proto i64,i64:x\nimport ext\nf:func i64,i64:x\nlocal i64:r\ncall p,ext,r,x\nret "
    "r\nendfunc\nendmodule\n");
  auto before = f->dump ();
  auto result = machinize (*f);
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  CHECK_FALSE (f->state ().is_leaf);
  const MachineInstruction *call = nullptr;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.has_lowered_call ()) call = &i;
  REQUIRE (call);
  CHECK ((call->operands ()[2].get<target::PhysicalRegister> ()).has_value ());
  CHECK ((call->operands ()[3].get<target::PhysicalRegister> ()).has_value ());
  auto lowered = f->dump ();
  auto revision = f->revision ();
  CHECK_FALSE ((machinize (*f)).has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->dump () == lowered);
}
TEST_CASE ("AArch64 scalar block RBLK variadic and multi-result ABI boundaries lower natively") {
  if (!arm ()) return;
  for (auto &text : test::aarch64_lowering_fixtures ())
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (text);
      CAPTURE (level);
      auto f = input (text, level);
      auto r = machinize (*f);
      INFO (((r).has_value () ? "" : r.error ().message));
      REQUIRE ((r).has_value ());
      REQUIRE ((f->verify ()).has_value ());
      REQUIRE ((legalize_operands (*f)).has_value ());
      REQUIRE ((f->verify ()).has_value ());
      auto graph = MachineGraphAnalysis::build (*f);
      REQUIRE ((graph).has_value ());
      auto merged = coalesce_registers (*f, (graph).value (), level);
      REQUIRE ((merged).has_value ());
      graph = MachineGraphAnalysis::build (*f);
      REQUIRE ((graph).has_value ());
      auto live = MachineLiveness::analyze (*f, (graph).value (), {.optimization_level = level});
      REQUIRE ((live).has_value ());
      auto ranges = MachineLiveRanges::build (*f, (graph).value (), (live).value ());
      REQUIRE ((ranges).has_value ());
      auto allocation
        = MachineAllocation::assign (*f, (graph).value (), (live).value (), (ranges).value ());
      REQUIRE ((allocation).has_value ());
      auto rewritten = rewrite_allocated (*f, (graph).value (), (live).value (), (ranges).value (),
                                          std::move ((allocation).value ()));
      INFO (((rewritten).has_value () ? "" : rewritten.error ().message));
      REQUIRE ((rewritten).has_value ());
      REQUIRE ((f->verify ()).has_value ());
      auto spilled = split_allocated_spills (*f, std::move ((rewritten).value ()));
      INFO (((spilled).has_value () ? "" : spilled.error ().message));
      REQUIRE ((spilled).has_value ());
      REQUIRE ((spilled.value ().cfg ().verify (*f)).has_value ());
      auto post = optimize_allocated_machine (*f, std::move ((spilled).value ()));
      INFO (((post).has_value () ? "" : post.error ().message));
      REQUIRE ((post).has_value ());
      REQUIRE ((post.value ().cfg ().verify (*f)).has_value ());
      auto framed = form_frame (*f, std::move ((post).value ()));
      INFO (((framed).has_value () ? "" : framed.error ().message));
      REQUIRE ((framed).has_value ());
      REQUIRE ((framed.value ().cfg ().verify (*f)).has_value ());
      auto encoded = encode_machine (*f, (framed).value ());
      INFO (((encoded).has_value () ? "" : encoded.error ().message));
      REQUIRE ((encoded).has_value ());
      REQUIRE ((encoded.value ().verify ()).has_value ());
    }
}
TEST_CASE ("lowered call mutation rejects virtual ABI locations atomically") {
  auto f = input (
    "m:module\np:proto i64,i64:x\nimport ext\nf:func i64,i64:x\nlocal i64:r\ncall p,ext,r,x\nret "
    "r\nendfunc\nendmodule\n");
  MachineInstruction *call = nullptr;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == MachineOpcode (Opcode::Call)) call = &i;
  REQUIRE (call);
  auto before = f->dump ();
  auto revision = f->revision ();
  MachineRewriter rw (*f);
  CHECK_FALSE ((rw.lower_call (*call, Opcode::Call,
                               std::vector<MachineOperand> (call->operands ().begin (),
                                                            call->operands ().end ())))
                 .has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->dump () == before);
  CHECK_FALSE (call->has_lowered_call ());
}
TEST_CASE ("AArch64 unsupported result counts and unsimplified inputs fail before rewriting") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  SUBCASE ("unsimplified return") {
    MachineFunction f ("bad", "constant", {{Type::i64 ()}, {}}, (target).value ());
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    MachineRewriter rw (f);
    REQUIRE (
      (rw.append (b, Opcode::Ret,
                  {mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))}))
        .has_value ());
    auto before = f.dump ();
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("machinize") != std::string::npos);
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("too many return registers") {
    MachineFunction f ("bad", "returns", {std::vector<Type> (9, Type::i64 ()), {Type::i64 ()}},
                       (target).value ());
    auto arg = mirnext::codegen::detail::access (f).create_register (Type::i64 (), "arg", true);
    REQUIRE ((arg).has_value ());
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    MachineRewriter rw (f);
    REQUIRE ((rw.append (b, Opcode::Ret,
                         std::vector<MachineOperand> (9, mirnext::codegen::detail::NodeAccess::
                                                           make_MachineOperand ((arg).value ()))))
               .has_value ());
    auto before = f.dump ();
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("return values") != std::string::npos);
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("unpromoted variadic float") {
    MachineFunction f ("bad", "varargs", {{}, {}}, (target).value ());
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    MachineRewriter rw (f);
    auto x = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
    auto y = mirnext::codegen::detail::access (f).create_register (Type (Type::Kind::F32));
    REQUIRE ((x).has_value ());
    REQUIRE ((y).has_value ());
    auto proto = mirnext::codegen::detail::access (f).intern_symbol (
      {"bad", "p", MachineSymbol::Kind::Prototype, Signature{{}, {Type::i64 ()}, true}, false, {}});
    REQUIRE ((proto).has_value ());
    REQUIRE (
      (rw.append (b, Opcode::Call,
                  {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                     mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                       &(proto).value ())),
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand ((x).value ()),
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand ((x).value ()),
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand ((y).value ())}))
        .has_value ());
    REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("promoted") != std::string::npos);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("combined outgoing area overflow") {
    MachineFunction f ("bad", "area", {{}, {}}, (target).value ());
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    MachineRewriter rw (f);
    auto pointer = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
    REQUIRE ((pointer).has_value ());
    auto block = Type::block (static_cast<std::size_t> (INT64_MAX) - 15);
    auto proto = mirnext::codegen::detail::access (f).intern_symbol (
      {"bad", "p", MachineSymbol::Kind::Prototype, Signature{{}, {block, block}}, false, {}});
    REQUIRE ((proto).has_value ());
    auto memory = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
      mirnext::codegen::detail::NodeAccess::
        make_MachineMemory (block,
                            mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                              &(pointer).value ()),
                            {}, 0, 1, {}, {}));
    REQUIRE (
      (rw.append (b, Opcode::Call,
                  {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                     mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                       &(proto).value ())),
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand ((pointer).value ()),
                   memory, memory}))
        .has_value ());
    REQUIRE ((rw.append (b, Opcode::Ret, {})).has_value ());
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE ((result).has_value ());
    CHECK (result.error ().message.find ("argument area") != std::string::npos);
    CHECK (f.revision () == revision);
  }
}
TEST_CASE ("builtin prototypes have typed identities distinct from user symbols") {
  auto f = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  MachineSymbol user{"$builtin",        "mir.va_arg.p", MachineSymbol::Kind::Prototype,
                     Signature{{}, {}}, false,          {}};
  auto a = mirnext::codegen::detail::access (*(f)).intern_symbol (user);
  REQUIRE ((a).has_value ());
  MachineSymbol runtime{"$builtin",
                        "mir.va_arg.p",
                        MachineSymbol::Kind::BuiltinPrototype,
                        Signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}},
                        false,
                        {},
                        Builtin::VariadicScalar};
  auto b = mirnext::codegen::detail::access (*(f)).intern_symbol (runtime);
  REQUIRE ((b).has_value ());
  CHECK (&(a).value () != &(b).value ());
  auto again = mirnext::codegen::detail::access (*(f)).intern_symbol (runtime);
  REQUIRE ((again).has_value ());
  CHECK (&(again).value () == &(b).value ());
  auto revision = f->revision ();
  runtime.builtin.reset ();
  CHECK_FALSE ((mirnext::codegen::detail::access (*(f)).intern_symbol (runtime)).has_value ());
  CHECK (f->revision () == revision);
}
TEST_CASE ("AArch64 large incoming and outgoing argument offsets use checked address temporaries") {
  if (!arm ()) return;
  auto f = input (test::aarch64_large_argument_fixture (), 0);
  auto r = machinize (*f);
  INFO (((r).has_value () ? "" : r.error ().message));
  REQUIRE ((r).has_value ());
  REQUIRE ((f->verify ()).has_value ());
  CHECK (f->state ().has_block_arguments);
  bool materialized = false;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == MachineOpcode (Opcode::Mov)) {
        auto reg = mirnext::codegen::detail::operand_access (i.operands ()[0])
                     .get<target::PhysicalRegister> ();
        auto value
          = mirnext::codegen::detail::operand_access (i.operands ()[1]).get<std::int64_t> ();
        materialized |= reg && reg->encoding () == 10 && value && *value >= 32768;
      }
  CHECK (materialized);
}
