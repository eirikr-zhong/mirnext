/* Licensed under the MIT License. x86 register allocation tests. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "Navigation.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/Execution/CodeMemory.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include "../IR/PublicInstructions.hpp"
#include "X86Fixtures.hpp"
#include "PostAllocationFixtures.hpp"
#include <algorithm>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
bool x86 () {
  return target::TargetMachine::native ().value ().architecture () == target::Architecture::X86_64;
}
struct Fixture {
  MachineFunction f{"native", "x86-allocation", Signature{},
                    (target::TargetMachine::native ()).value ()};
  MachineRewriter rw{f};
  MachineBasicBlock &b = mirnext::codegen::detail::access (f).create_block ();
  Fixture (bool fp = false) {
    auto state = f.state ();
    state.machinized = state.operands_legalized = true;
    state.requires_frame_pointer = fp;
    REQUIRE ((rw.set_state (state)).has_value ());
  }
  MachineOperand reg (Type type) {
    auto r = mirnext::codegen::detail::access (f).create_register (type);
    REQUIRE ((r).has_value ());
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ());
  }
  MachineOperand physical (unsigned n) {
    return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
      f.target ().registers ()[n].reg);
  }
  MachineInstruction *add (MachineOpcode code, std::vector<MachineOperand> operands = {}) {
    auto i = rw.append (b, code, std::move (operands));
    INFO (((i).has_value () ? "" : i.error ().message));
    REQUIRE ((i).has_value ());
    return &(i).value ();
  }
  void force_stack () {
    std::vector<target::PhysicalRegister> all;
    for (auto &p : f.target ().registers ())
      if (!p.reserved) all.push_back (p.reg);
    REQUIRE ((rw.set_global_registers (all)).has_value ());
  }
};
MachineRewriteResult rewrite (MachineFunction &f, unsigned level) {
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
  REQUIRE ((live).has_value ());
  auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
  REQUIRE ((ranges).has_value ());
  auto allocation
    = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
  REQUIRE ((allocation).has_value ());
  auto result = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                   std::move ((allocation).value ()));
  INFO (((result).has_value () ? "" : result.error ().message));
  REQUIRE ((result).has_value ());
  REQUIRE (result.value ().current (f));
  REQUIRE ((f.verify ()).has_value ());
  return std::move ((result).value ());
}
MachinePostResult finish (MachineFunction &f, unsigned level) {
  auto allocated = rewrite (f, level);
  auto spill = split_allocated_spills (f, std::move (allocated));
  INFO (((spill).has_value () ? "" : spill.error ().message));
  REQUIRE ((spill).has_value ());
  auto post = optimize_allocated_machine (f, std::move ((spill).value ()));
  INFO (((post).has_value () ? "" : post.error ().message));
  REQUIRE ((post).has_value ());
  REQUIRE (post.value ().current (f));
  REQUIRE ((post.value ().cfg ().verify (f)).has_value ());
  REQUIRE ((f.verify ()).has_value ());
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) {
      auto pattern = has_instruction_pattern (i);
      REQUIRE ((pattern).has_value ());
      // DCE can retain a USE anchor in an otherwise empty block. It has no
      // direct encoding pattern and is explicitly skipped by the encoder.
      if (i.opcode () == MachineOpcode (MachinePseudoOpcode::Use))
        CHECK_FALSE ((pattern).value ());
      else
        CHECK ((pattern).value ());
      for (auto &o : i.operands ()) {
        CHECK_FALSE (
          mirnext::codegen::detail::operand_access (o).get<const MachineVirtualRegister *> ());
        if (auto m = mirnext::codegen::detail::operand_access (o).get<MachineMemory> ()) {
          if (m->base)
            CHECK (std::holds_alternative<target::PhysicalRegister> (
              mirnext::codegen::detail::NodeAccess::storage (*m->base)));
          if (m->index)
            CHECK (std::holds_alternative<target::PhysicalRegister> (
              mirnext::codegen::detail::NodeAccess::storage (*m->index)));
        }
      }
    }
  return std::move ((post).value ());
}
void pipeline (const Program &source, unsigned level, bool optimize) {
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto optimized = transforms::optimize (source, options);
  REQUIRE ((optimized).has_value ());
  auto &context = optimize ? optimized.value ().program () : source;
  for (auto &module : context.modules ())
    for (auto &function : module.functions ()) {
      auto m = prepare_machine_ir (function, (target::TargetMachine::native ()).value ());
      REQUIRE ((m).has_value ());
      REQUIRE ((machinize ((m).value ())).has_value ());
      REQUIRE ((legalize_operands ((m).value ())).has_value ());
      auto graph = MachineGraphAnalysis::build ((m).value ());
      REQUIRE ((graph).has_value ());
      REQUIRE ((coalesce_registers ((m).value (), (graph).value (), level)).has_value ());
      finish ((m).value (), level);
    }
}
}  // namespace
TEST_CASE (
  "owning copied-block dependencies retain analysis identity without virtual machine addresses") {
  Fixture t;
  auto value = t.reg (Type::i64 ());
  auto dependency
    = *mirnext::codegen::detail::operand_access (value).get<const MachineVirtualRegister *> ();
  auto proto
    = mirnext::codegen::detail::access (t.f).intern_symbol ({"native",
                                                             "block-proto",
                                                             MachineSymbol::Kind::Prototype,
                                                             Signature{{}, {Type::block (24)}},
                                                             false,
                                                             {}});
  REQUIRE ((proto).has_value ());
  auto ext = mirnext::codegen::detail::access (t.f).intern_symbol (
    {"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
  REQUIRE ((ext).has_value ());
  auto block = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    mirnext::codegen::detail::NodeAccess::
      make_MachineMemory (Type::block (24),
                          mirnext::codegen::detail::NodeAccess::make_MachineRegister (dependency),
                          {}, 24, 1, {}, {}));
  auto call
    = t.add (Opcode::Call, {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                              mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                                &(proto).value ())),
                            mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                              mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                                &(ext).value ())),
                            block});
  auto operands
    = std::vector<MachineOperand> (call->operands ().begin (), call->operands ().end ());
  operands[2] = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
    mirnext::codegen::detail::NodeAccess::
      make_MachineMemory (Type (Type::Kind::Undef),
                          mirnext::codegen::detail::NodeAccess::make_MachineRegister (dependency),
                          {}, 24, 1, {}, {}));
  REQUIRE ((t.rw.lower_call (*call, Opcode::Call, std::move (operands))).has_value ());
  CallArgumentDependency d
    = mirnext::codegen::detail::NodeAccess::make_CallArgumentDependency (2, dependency);
  CHECK_FALSE ((t.rw.set_call_dependencies (*call, std::span (&d, 1))).has_value ());
  t.add (Opcode::Ret);
  auto before = t.f.dump ();
  auto revision = t.f.revision ();
  for (auto bad :
       {mirnext::codegen::detail::NodeAccess::make_CallArgumentDependency (0, dependency),
        mirnext::codegen::detail::NodeAccess::make_CallArgumentDependency (99, dependency),
        mirnext::codegen::detail::NodeAccess::make_CallArgumentDependency (2, nullptr)}) {
    CHECK_FALSE ((t.rw.set_call_dependencies (*call, std::span (&bad, 1))).has_value ());
    CHECK (t.f.dump () == before);
    CHECK (t.f.revision () == revision);
  }
  Fixture foreign;
  auto other = foreign.reg (Type::i64 ());
  CallArgumentDependency bad = mirnext::codegen::detail::NodeAccess::
    make_CallArgumentDependency (2, *mirnext::codegen::detail::operand_access (other)
                                       .get<const MachineVirtualRegister *> ());
  CHECK_FALSE ((t.rw.set_call_dependencies (*call, std::span (&bad, 1))).has_value ());
  CHECK (t.f.dump () == before);
  auto rewritten = rewrite (t.f, 2);
  REQUIRE (call->call_dependencies ().size () == 1);
  CHECK (call->call_dependencies ()[0] == d);
  CHECK_FALSE (call->operands ()[2].get<MachineMemory> ().value ().base);
  REQUIRE ((t.f.verify ()).has_value ());
  before = t.f.dump ();
  revision = t.f.revision ();
  std::array duplicate{d, d};
  CHECK_FALSE ((t.rw.set_call_dependencies (*call, duplicate)).has_value ());
  CHECK_FALSE ((t.rw.set_call_dependencies (*call, std::span (&bad, 1))).has_value ());
  CHECK (t.f.dump () == before);
  CHECK (t.f.revision () == revision);
  auto split = split_allocated_spills (t.f, std::move (rewritten));
  REQUIRE ((split).has_value ());
  bool checked = false;
  auto post
    = optimize_allocated_machine (t.f, std::move ((split).value ()),
                                  {.observer = [&] (auto stage, const auto &view) -> Result<void> {
                                    if (stage == MachinePostStage::Liveness) {
                                      auto dead = view.deaths (*call);
                                      CHECK (std::find (dead.begin (), dead.end (),
                                                        mirnext::codegen::detail::NodeAccess::
                                                          make_MachineRegister (dependency))
                                             != dead.end ());
                                      checked = true;
                                    }
                                    return {};
                                  }});
  INFO (((post).has_value () ? "" : post.error ().message));
  REQUIRE ((post).has_value ());
  CHECK (checked);
  REQUIRE ((t.f.verify ()).has_value ());
  CHECK (t.f.dump ().find ("stack-argument 2 depends-on") != std::string::npos);
}
TEST_CASE ("native x86 allocation rewrites tied memory destinations with ABI reload registers") {
  if (!x86 ()) return;
  for (bool fp : {false, true})
    for (unsigned level = 0; level < 4; ++level) {
      Fixture t (fp);
      t.force_stack ();
      auto a = t.reg (Type::i64 ()), c = t.reg (Type::i64 ());
      auto first
        = t.add (Opcode::Mov, {a, mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                                    std::int64_t (42))});
      t.add (Opcode::Mov, {c, a});
      auto sum = t.add (Opcode::Add, {c, c, a});
      t.add (Opcode::Ret);
      auto result = rewrite (t.f, level);
      CHECK (result.allocation ().stack_bytes () == 16);
      REQUIRE ((first->operands ()[0].get<MachineMemory> ()).has_value ());
      REQUIRE ((sum->operands ()[0].get<MachineMemory> ()).has_value ());
      CHECK (sum->operands ()[0] == sum->operands ()[1]);
      REQUIRE ((sum->operands ()[2].get<target::PhysicalRegister> ()).has_value ());
      CHECK (sum->operands ()[2].get<target::PhysicalRegister> ().value ().encoding () == 10);
      CHECK (first->operands ()[0].get<MachineMemory> ().value ().base
             == mirnext::codegen::detail::NodeAccess::make_MachineRegister (
               t.f.target ().registers ()[fp ? 5 : 4].reg));
      CHECK ((first->operands ()[0].get<MachineMemory> ().value ().displacement < 0) == fp);
      REQUIRE ((has_instruction_pattern (*sum)).has_value ());
      CHECK ((has_instruction_pattern (*sum)).value ());
    }
}
TEST_CASE (
  "owning allocation preserves bound registers across repeated calls with target stack locations") {
  for (unsigned level : {2, 3})
    for (bool fp : {false, true}) {
      Fixture t (fp);
      auto physical = t.f.target ().registers ()[2].reg;
      auto r = mirnext::codegen::detail::access (t.f).create_register (Type::i64 (), "kept", false,
                                                                       false, physical);
      REQUIRE ((r).has_value ());
      auto proto = mirnext::codegen::detail::access (t.f).intern_symbol (
        {"native", "empty-proto", MachineSymbol::Kind::Prototype, Signature{}, false, {}});
      REQUIRE ((proto).has_value ());
      auto ext = mirnext::codegen::detail::access (t.f).intern_symbol (
        {"native", "ext", MachineSymbol::Kind::Import, {}, false, {}});
      REQUIRE ((ext).has_value ());
      t.add (Opcode::Mov,
             {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
              mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
      std::vector<MachineInstruction *> calls;
      for (unsigned n = 0; n < 2; ++n) {
        auto call = t.add (Opcode::Call,
                           {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                              mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                                &(proto).value ())),
                            mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                              mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                                &(ext).value ()))});
        REQUIRE ((t.rw.lower_call (*call, call->opcode (),
                                   std::vector<MachineOperand> (call->operands ().begin (),
                                                                call->operands ().end ())))
                   .has_value ());
        REQUIRE ((t.rw.set_call_arguments (*call, std::span (&physical, 1))).has_value ());
        calls.push_back (call);
        t.add (MachinePseudoOpcode::Use,
               {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ())});
      }
      t.add (Opcode::Ret);
      auto rewritten = rewrite (t.f, level);
      auto slot = rewritten.spill_slot ((r).value ());
      REQUIRE ((slot).has_value ());
      REQUIRE ((slot).value ());
      auto displacement = static_cast<std::int64_t> ((*(slot).value ()).offset + 16);
      unsigned base = 29;
      if (x86 ()) {
        auto value = target::x86_spill_displacement (t.f.target ().abi (), fp, false, Type::i64 (),
                                                     (*(slot).value ()).offset);
        REQUIRE ((value).has_value ());
        displacement = (value).value ();
        base = fp ? 5 : 4;
      }
      for (auto call : calls) {
        auto next = mirnext::test::neighbor (t.rw.next (*call));
        REQUIRE ((next).has_value ());
        REQUIRE (&(next).value ());
        auto memory = mirnext::codegen::detail::operand_access (next.value ().operands ()[1])
                        .get<MachineMemory> ();
        REQUIRE (memory);
        CHECK (memory->displacement == displacement);
        CHECK (memory->base
               == mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                 t.f.target ().registers ()[base].reg));
      }
      auto split = split_allocated_spills (t.f, std::move (rewritten));
      REQUIRE ((split).has_value ());
      auto post = optimize_allocated_machine (t.f, std::move ((split).value ()));
      REQUIRE ((post).has_value ());
      REQUIRE ((t.f.verify ()).has_value ());
    }
}
TEST_CASE ("native x86 floating reloads preserve tied operands and separate input temporaries") {
  if (!x86 ()) return;
  for (bool single : {false, true})
    for (unsigned level = 0; level < 4; ++level) {
      Fixture t (true);
      t.force_stack ();
      auto type = Type (single ? Type::Kind::F32 : Type::Kind::F64);
      auto a = t.reg (type), c = t.reg (type);
      auto mov = single ? Opcode::FMov : Opcode::DMov;
      t.add (mov, {a, t.physical (16)});
      t.add (mov, {c, t.physical (17)});
      auto sum = t.add (single ? Opcode::FAdd : Opcode::DAdd, {c, c, a});
      t.add (mov,
             {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                mirnext::codegen::detail::NodeAccess::
                  make_MachineMemory (type,
                                      mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                        t.f.target ().registers ()[3].reg),
                                      {}, 0, 1, {}, {})),
              c});
      t.add (Opcode::Ret);
      rewrite (t.f, level);
      auto first = target::x86_reload_register (t.f.target ().abi (), type, true),
           second = target::x86_reload_register (t.f.target ().abi (), type, false);
      REQUIRE (first);
      REQUIRE (second);
      CHECK (sum->operands ()[0] == sum->operands ()[1]);
      CHECK (sum->operands ()[0].get<target::PhysicalRegister> ().value ().encoding ()
             == static_cast<unsigned> (*first));
      CHECK (sum->operands ()[2].get<target::PhysicalRegister> ().value ().encoding ()
             == static_cast<unsigned> (*second));
      CHECK ((has_instruction_pattern (*sum)).value ());
    }
}
TEST_CASE ("native x86 x87 values stay in two-slot memory without reload registers") {
  if (!x86 () || sizeof (long double) == sizeof (double)) return;
  for (bool fp : {false, true})
    for (unsigned level = 0; level < 4; ++level) {
      Fixture t (fp);
      auto ld = Type (Type::Kind::LongDouble);
      auto a = t.reg (ld), b = t.reg (ld), c = t.reg (ld);
      t.add (Opcode::LDMov, {a, t.physical (32)});
      t.add (Opcode::LDMov, {b, t.physical (33)});
      auto sum = t.add (Opcode::LDAdd, {c, a, b});
      t.add (Opcode::LDMov, {t.physical (32), c});
      t.add (Opcode::Ret);
      auto rewritten = rewrite (t.f, level);
      CHECK (rewritten.allocation ().stack_bytes () >= 32);
      for (auto &o : sum->operands ()) {
        auto mem = mirnext::codegen::detail::operand_access (o).get<MachineMemory> ();
        REQUIRE (mem);
        CHECK (mem->type == ld);
        CHECK (mem->base
               == mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                 t.f.target ().registers ()[fp ? 5 : 4].reg));
      }
      CHECK ((has_instruction_pattern (*sum)).value ());
      auto split = split_allocated_spills (t.f, std::move (rewritten));
      REQUIRE ((split).has_value ());
      auto post = optimize_allocated_machine (t.f, std::move ((split).value ()));
      REQUIRE ((post).has_value ());
      REQUIRE ((t.f.verify ()).has_value ());
    }
}
TEST_CASE (
  "native x86 allocation consumers cover public opcodes ABI boundaries and raw combine rules") {
  if (!x86 ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (unsigned level = 0; level < 4; ++level) {
    CAPTURE (level);
    for (auto &d : instruction_descriptors ()) {
      CAPTURE (d.name);
      auto c = test::public_instruction_example (d);
      pipeline (*c, level, true);
    }
    for (auto &text :
         test::x86_lowering_fixtures (target.value ().abi () == target::ABI::X86Windows)) {
      CAPTURE (text);
      auto c = serialization::parse_text (text);
      REQUIRE ((c).has_value ());
      pipeline ((c).value (), level, true);
    }
    for (auto &text : test::post_allocation_fixtures ()) {
      CAPTURE (text);
      auto c = serialization::parse_text (text);
      REQUIRE ((c).has_value ());
      pipeline ((c).value (), level, false);
    }
  }
}
TEST_CASE ("native x86 empty property blocks retain zero-byte anchors through execution") {
  if (!x86 ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE ((target).has_value ());
  for (unsigned level = 0; level < 4; ++level)
    for (bool equal : {false, true})
      for (unsigned value : {0u, 1u}) {
        CAPTURE (level);
        CAPTURE (equal);
        CAPTURE (value);
        auto text = std::string ("m:module\nf:func i64,i64:x\nlocal i64:r\nprset x,1\n")
                    + (equal ? "prbeq" : "prbne") + " Finish,x," + std::to_string (value)
                    + "\nmov r,17\njmp Done\nFinish:\nmov r,42\nDone:\nret r\nendfunc\nendmodule\n";
        auto source = serialization::parse_text (text);
        REQUIRE ((source).has_value ());
        auto machine
          = prepare_machine_ir (*source.value ().modules ().begin ()->functions ().begin (),
                                (target).value ());
        REQUIRE ((machine).has_value ());
        auto &f = (machine).value ();
        REQUIRE ((machinize (f)).has_value ());
        REQUIRE ((legalize_operands (f)).has_value ());
        auto graph = MachineGraphAnalysis::build (f);
        REQUIRE ((graph).has_value ());
        REQUIRE ((coalesce_registers (f, (graph).value (), level)).has_value ());
        auto post = finish (f, level);
        const bool taken = equal == (value == 0);
        unsigned anchors = 0;
        for (auto &block : f.blocks ())
          for (auto &i : block.instructions ()) {
            CHECK (i.opcode () != MachineOpcode (Opcode::PrSet));
            CHECK (i.opcode () != MachineOpcode (Opcode::PrBeq));
            CHECK (i.opcode () != MachineOpcode (Opcode::PrBne));
            if (i.opcode () == MachineOpcode (MachinePseudoOpcode::Use)) {
              ++anchors;
              CHECK (i.operands ().empty ());
            }
          }
        CHECK (anchors == (level && !taken ? 1 : 0));
        CHECK (post.statistics ().no_op_anchors == anchors);
        auto frame = form_frame (f, std::move (post));
        REQUIRE ((frame).has_value ());
        auto artifact = encode_machine (f, (frame).value ());
        INFO (((artifact).has_value () ? "" : artifact.error ().message));
        REQUIRE ((artifact).has_value ());
        REQUIRE ((artifact.value ().verify ()).has_value ());
        auto executable = execution::CodeMemory ().publish (std::move ((artifact).value ()));
        REQUIRE ((executable).has_value ());
        auto call
          = reinterpret_cast<std::int64_t (*) (std::int64_t)> (executable.value ().address ());
        for (auto input : {0, 1, 42}) CHECK (call (input) == (taken ? 42 : 17));
      }
}
