/* Licensed under the MIT License. Native machine ownership and preparation. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Editor.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "../IR/PublicInstructions.hpp"
#include "../Transforms/InlineFixtures.hpp"
#include "../Transforms/JumpFixtures.hpp"
#include "../Transforms/MemoryFixtures.hpp"
#include <algorithm>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
static_assert (!std::is_copy_constructible_v<MachineVirtualRegister>);
static_assert (!std::is_copy_constructible_v<MachineStackSlot>);
static_assert (!std::is_copy_constructible_v<MachineLabel>);
static_assert (!std::is_copy_constructible_v<MachineBasicBlock>);
namespace {
target::TargetMachine host () {
  auto t = target::TargetMachine::native ();
  REQUIRE ((t).has_value ());
  return std::move ((t).value ());
}
std::unique_ptr<Program> parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO (((c).has_value () ? "" : c.error ().message));
  REQUIRE ((c).has_value ());
  return std::make_unique<Program> (std::move ((c).value ()));
}
const char *simple
  = "m:module\nf:func i64, i64:a\nlocal i64:r\nadd r, a, 2\nret r\nendfunc\nendmodule\n";
std::unique_ptr<MachineFunction> prepared (std::string_view text = simple) {
  auto c = parse (text);
  auto p = transforms::optimize (*c);
  INFO (((p).has_value () ? "" : p.error ().message));
  REQUIRE ((p).has_value ());
  auto f = p.value ().find_function ("m", "f").get ();
  REQUIRE (f);
  auto m = prepare_machine_ir (*f, host ());
  INFO (((m).has_value () ? "" : m.error ().message));
  REQUIRE ((m).has_value ());
  return std::make_unique<MachineFunction> (std::move ((m).value ()));
}
std::size_t blocks (const MachineFunction &f) {
  return std::distance (f.blocks ().begin (), f.blocks ().end ());
}
}  // namespace
TEST_CASE (
  "native target layouts and register identities are checked independently of virtual registers") {
  auto target = host ();
  CHECK (target.pointer_bits () == 64);
  CHECK (target.stack_alignment () == 16);
  CHECK (target.registers ().size ()
         == (target.architecture () == target::Architecture::AArch64 ? 64 : 34));
  CHECK_FALSE ((target.find_register ("missing")).has_value ());
  CHECK_FALSE ((target.layout (Type::block (8))).has_value ());
  CHECK_FALSE ((target.layout (Type (Type::Kind::Undef))).has_value ());
  CHECK ((target.layout (Type::i64 ())).value () == target::TypeLayout{8, 8});
  CHECK ((target.layout (Type (Type::Kind::Bool))).value () == target::TypeLayout{8, 8});
  CHECK ((target.layout (Type (Type::Kind::LongDouble))).value ()
         == target::TypeLayout{sizeof (long double), alignof (long double)});
  for (auto &info : target.registers ()) {
    auto r = target.find_register (info.name);
    REQUIRE ((r).has_value ());
    CHECK ((r).value () == info.reg);
    CHECK (target.register_info ((r).value ()).get () == &info);
    CHECK (target.accepts ((r).value (), Type::i64 ())
           == (info.bank == target::RegisterBank::Integer));
  }
  auto opposite = target.architecture () == target::Architecture::AArch64
                    ? target::Architecture::X86_64
                    : target::Architecture::AArch64;
  CHECK_FALSE ((target::TargetMachine::native (opposite)).has_value ());
  CHECK_FALSE (
    (target::TargetMachine::native (static_cast<target::Architecture> (100))).has_value ());
}
TEST_CASE (
  "machine preparation owns operands names signatures and nodes beyond both source lifetimes") {
  auto m = prepared ();
  REQUIRE ((m->verify ()).has_value ());
  auto first = m->dump ();
  REQUIRE_FALSE (first.empty ());
  auto cfg = MachineControlFlow::build (*m);
  REQUIRE ((cfg).has_value ());
  CHECK (cfg.value ().current (*m));
  CHECK (m->arguments ().size () == 1);
  CHECK (m->signature ().parameters == std::vector<Type>{Type::i64 ()});
  auto ptr = m.get ();
  auto moved = std::move (m);
  CHECK (moved.get () == ptr);
  CHECK (moved->dump () == first);
  REQUIRE ((moved->verify ()).has_value ());
  CHECK (cfg.value ().current (*moved));
}
TEST_CASE ("rewrites retain instruction and operand addresses across block ownership transfers") {
  auto f = prepared ();
  auto &first = *f->blocks ().begin ();
  auto &other = mirnext::codegen::detail::access (*(f)).create_block ("other");
  MachineRewriter r (*f);
  auto reg = mirnext::codegen::detail::access (*(f)).create_register (Type::i64 (), "temporary");
  REQUIRE ((reg).has_value ());
  auto mov
    = r.append (other, Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (42))});
  REQUIRE ((mov).has_value ());
  auto ret
    = r.append (other, Opcode::Ret,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ())});
  REQUIRE ((ret).has_value ());
  auto address = &(mov).value ();
  auto operands = mov.value ().operands ().data ();
  auto anchor = const_cast<MachineInstruction *> (&*first.instructions ().begin ());
  REQUIRE ((r.move_before ((mov).value (), *anchor)).has_value ());
  CHECK (&(mov).value () == address);
  CHECK (mov.value ().operands ().data () == operands);
  CHECK (&mov.value ().block () == &first);
  REQUIRE (
    (r.replace ((mov).value (), Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (7))}))
      .has_value ());
  CHECK (&(mov).value () == address);
  auto revision = f->revision ();
  auto count = f->instruction_count ();
  CHECK_FALSE ((r.replace ((mov).value (), Opcode::Add, {})).has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->instruction_count () == count);
  REQUIRE ((r.erase ((mov).value ())).has_value ());
  REQUIRE ((f->verify ()).has_value ());
}
TEST_CASE ("machine hardware bindings cannot become addressable storage") {
  auto target = host ();
  MachineFunction f ("m", "entry", {{Type::i64 ()}, {}}, target);
  auto physical = target.find_register (
    target.architecture () == target::Architecture::AArch64 ? "r19" : "r12");
  REQUIRE ((physical).has_value ());
  auto revision = f.revision ();
  CHECK_FALSE (
    (mirnext::codegen::detail::access (f).create_register (Type::i64 (), "invalid", false, true,
                                                           (physical).value ()))
      .has_value ());
  CHECK (f.revision () == revision);
  auto bound = mirnext::codegen::detail::access (f).create_register (Type::i64 (), "bound", false,
                                                                     false, (physical).value ());
  REQUIRE ((bound).has_value ());
  auto pointer = mirnext::codegen::detail::access (f).create_register (Type::pointer (), "pointer");
  REQUIRE ((pointer).has_value ());
  auto &block = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter rw (f);
  revision = f.revision ();
  for (auto opcode : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    CHECK_FALSE (
      (rw.append (block, opcode,
                  {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((pointer).value ()),
                   mirnext::codegen::detail::NodeAccess::make_MachineOperand ((bound).value ())}))
        .has_value ());
    CHECK (f.revision () == revision);
    CHECK (f.instruction_count () == 0);
  }
}
TEST_CASE ("foreign machine handles and wrong instruction types are atomic diagnostics") {
  auto f = prepared (), other = prepared ();
  auto &b = *f->blocks ().begin ();
  MachineRewriter rw (*f);
  auto reg = mirnext::codegen::detail::access (*(other)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  auto label
    = mirnext::codegen::detail::access (*(other)).create_label (*other->blocks ().begin ());
  REQUIRE ((label).has_value ());
  auto slot = mirnext::codegen::detail::access (*(other)).create_stack_slot (8, 8);
  REQUIRE ((slot).has_value ());
  auto local = mirnext::codegen::detail::access (*(f)).create_register (Type::i64 ());
  REQUIRE ((local).has_value ());
  auto revision = f->revision ();
  auto count = f->instruction_count ();
  CHECK_FALSE (
    (rw.append (b, Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((local).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ())}))
      .has_value ());
  CHECK_FALSE (
    (rw.append (b, Opcode::Jmp,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((label).value ())}))
      .has_value ());
  CHECK_FALSE (
    (rw.append (b, Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((local).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                   mirnext::codegen::detail::NodeAccess::make_StackSlotReference (
                     &(slot).value ()))}))
      .has_value ());
  CHECK_FALSE (
    (rw.append (b, Opcode::DMov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((local).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (2.0)}))
      .has_value ());
  CHECK_FALSE ((rw.append (b, Opcode::Label, {})).has_value ());
  CHECK_FALSE (
    (rw.append (b, MachinePseudoOpcode::Use,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))}))
      .has_value ());
  CHECK_FALSE (
    (rw.append (b, Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((local).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                   mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (nullptr))}))
      .has_value ());
  CHECK_FALSE (
    (rw.append (*other->blocks ().begin (), Opcode::Ret,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))}))
      .has_value ());
  CHECK (f->revision () == revision);
  CHECK (f->instruction_count () == count);
  REQUIRE ((f->verify ()).has_value ());
  CHECK_FALSE ((mirnext::codegen::detail::access (*(f)).create_stack_slot (0, 8)).has_value ());
  CHECK_FALSE ((mirnext::codegen::detail::access (*(f)).create_stack_slot (8, 3)).has_value ());
}
TEST_CASE (
  "stack slots physical registers builtins and call register sets retain separate identities") {
  auto f = prepared (
    "m:module\np:proto i64\nimport ext\nf:func i64\nlocal i64:r\ncall p, ext, r\nret "
    "r\nendfunc\nendmodule\n");
  auto reg = mirnext::codegen::detail::access (*(f)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  auto slot = mirnext::codegen::detail::access (*(f)).create_stack_slot (16, 16);
  REQUIRE ((slot).has_value ());
  auto integer = std::find_if (f->target ().registers ().begin (), f->target ().registers ().end (),
                               [] (auto &i) {
                                 return i.bank == target::RegisterBank::Integer && !i.reserved;
                               });
  REQUIRE (integer != f->target ().registers ().end ());
  MachineRewriter rw (*f);
  MachineInstruction *call = nullptr;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == MachineOpcode (Opcode::Call)) call = &i;
  REQUIRE (call);
  auto original = call->operands ().data ();
  std::vector<target::PhysicalRegister> args{integer->reg, integer->reg};
  REQUIRE ((rw.set_call_arguments (*call, args)).has_value ());
  CHECK (call->call_arguments ().size () == 1);
  CHECK (call->operands ().data () == original);
  REQUIRE (
    (rw.insert_before (*call, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (integer->reg)}))
      .has_value ());
  REQUIRE (
    (rw.insert_before (*call, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          mirnext::codegen::detail::NodeAccess::
                            make_StackSlotReference (&(slot).value (), 8))}))
      .has_value ());
  REQUIRE (
    (rw.insert_before (*call, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          BuiltinReference{Builtin::CopyBlock})}))
      .has_value ());
  CHECK (f->dump ().find ("call-regs") != std::string::npos);
  CHECK (f->dump ().find ("slot0+8") != std::string::npos);
  REQUIRE ((f->verify ()).has_value ());
}
TEST_CASE ("hardware identity is shared and every ordinary return has a typed USE") {
  auto target = host ();
  auto hardware = target.architecture () == target::Architecture::AArch64 ? "r19" : "r12";
  auto c = parse (std::string ("m:module\nf:func i64, i64:c\nglobal i64:h:") + hardware
                  + "\nmov h,42\nbt yes,c\nret h\nyes:\nret h\nendfunc\nendmodule\n");
  auto &source = *c->modules ().begin ()->functions ().begin ();
  auto before = dump (*c);
  auto m = prepare_machine_ir (source, target);
  REQUIRE ((m).has_value ());
  CHECK (dump (*c) == before);
  std::size_t returns = 0, uses = 0;
  for (auto &b : m.value ().blocks ())
    for (auto &i : b.instructions ()) {
      returns += i.opcode () == MachineOpcode (Opcode::Ret);
      uses += i.opcode () == MachineOpcode (MachinePseudoOpcode::Use);
    }
  CHECK (returns == 2);
  CHECK (uses == returns);
  const MachineVirtualRegister *bound = nullptr;
  for (auto &r : m.value ().registers ())
    if (r.binding ()) bound = &r;
  REQUIRE (bound);
  auto alias
    = mirnext::codegen::detail::access (((m)).value ())
        .create_register (Type (Type::Kind::U32), "alias", false, false, bound->binding ());
  REQUIRE ((alias).has_value ());
  CHECK (&(alias).value () == bound);
  CHECK_FALSE (
    (mirnext::codegen::detail::access (((m)).value ())
       .create_register (Type (Type::Kind::F64), "wrong", false, false, bound->binding ()))
      .has_value ());
}
TEST_CASE (
  "preparation rejects unmaterialized scope SSA and unsimplified strings without source mutation") {
  auto c = parse (simple);
  auto &f = *c->modules ().begin ()->functions ().begin ();
  transforms::FunctionPassManager passes;
  passes.add<transforms::PrepareCFGPass> ();
  passes.add<transforms::BuildSSAPass> ();
  analysis::FunctionAnalysisManager a (f);
  REQUIRE ((passes.run (f, a)).has_value ());
  CHECK_FALSE ((prepare_machine_ir (f, host ())).has_value ());
  Program scoped;
  auto m = mirnext::ir::detail::access (scoped).create_module ("scoped");
  REQUIRE ((m).has_value ());
  auto fun = mirnext::ir::detail::access (((m)).value ()).create_function ("f", {{}, {}});
  REQUIRE ((fun).has_value ());
  auto b = mirnext::ir::detail::access (((fun)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  InstructionBuilder builder ((b).value ());
  auto scope = builder.begin_stack_scope ();
  REQUIRE ((scope).has_value ());
  REQUIRE ((builder.ret ()).has_value ());
  auto before = dump (scoped);
  auto result = prepare_machine_ir ((fun).value (), host ());
  CHECK_FALSE ((result).has_value ());
  CHECK (dump (scoped) == before);
  CHECK (result.error ().message.find ("prepare-machine-ir") != std::string::npos);
}
TEST_CASE ("machine CFG preserves duplicate switch edges address roots and indirect targets") {
  auto c = parse (
    "m:module\nf:func i64, i64:a\nlocal i64:p\nladdr p, target\nswitch a, target, "
    "target\nunused:\nret 1\ntarget:\njmpi p\nendfunc\nref:lref target, 0\nendmodule\n");
  auto &f = *c->modules ().begin ()->functions ().begin ();
  auto m = prepare_machine_ir (f, host ());
  INFO (((m).has_value () ? "" : m.error ().message));
  REQUIRE ((m).has_value ());
  auto cfg = MachineControlFlow::build ((m).value ());
  REQUIRE ((cfg).has_value ());
  std::size_t cases = 0, indirect = 0;
  for (auto &e : cfg.value ().edges) {
    cases += e.kind == MachineControlFlow::EdgeKind::SwitchCase;
    indirect += e.kind == MachineControlFlow::EdgeKind::Indirect;
  }
  CHECK (cases == 2);
  CHECK (indirect == 1);
  CHECK (cfg.value ().current ((m).value ()));
  mirnext::codegen::detail::access (((m)).value ()).create_block ();
  CHECK_FALSE (cfg.value ().current ((m).value ()));
}
TEST_CASE ("every public opcode and full optimization output prepares machine IR") {
  auto target = host ();
  for (auto &d : instruction_descriptors ())
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (d.name);
      CAPTURE (level);
      auto source = test::public_instruction_example (d);
      transforms::OptimizationOptions options;
      options.level = static_cast<transforms::OptimizationLevel> (level);
      auto p = transforms::optimize (*source, options);
      INFO (((p).has_value () ? "" : p.error ().message));
      REQUIRE ((p).has_value ());
      for (auto &m : p.value ().program ().modules ())
        for (auto &f : m.functions ()) {
          auto machine = prepare_machine_ir (f, target);
          INFO (((machine).has_value () ? "" : machine.error ().message));
          REQUIRE ((machine).has_value ());
          REQUIRE ((machine.value ().verify ()).has_value ());
          auto cfg = MachineControlFlow::build ((machine).value ());
          REQUIRE ((cfg).has_value ());
          CHECK (mirnext::codegen::detail::NodeAccess::blocks (((cfg)).value ()).size ()
                 == blocks ((machine).value ()) + 2);
          CHECK_FALSE (machine.value ().dump ().empty ());
          auto graph = MachineGraphAnalysis::build ((machine).value ());
          REQUIRE ((graph).has_value ());
          auto live = MachineLiveness::analyze ((machine).value (), (graph).value (),
                                                {.optimization_level = level});
          REQUIRE ((live).has_value ());
          CHECK (live.value ().current ((machine).value ()));
        }
    }
}
TEST_CASE ("machine relocations preserve distinct anonymous data identities") {
  auto machine = prepared ();
  auto a = mirnext::codegen::detail::access (*(machine))
             .intern_symbol ({"data", {}, MachineSymbol::Kind::Data, {}, false, 0});
  auto b = mirnext::codegen::detail::access (*(machine))
             .intern_symbol ({"data", {}, MachineSymbol::Kind::Data, {}, false, 1});
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  auto reg = mirnext::codegen::detail::access (*(machine)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  MachineRewriter rw (*machine);
  auto anchor = &*machine->blocks ().begin ()->instructions ().begin ();
  REQUIRE (
    (rw.insert_before (*anchor, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                            &(a).value ()))}))
      .has_value ());
  REQUIRE (
    (rw.insert_before (*anchor, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                            &(b).value ()))}))
      .has_value ());
  std::vector<const MachineSymbol *> symbols;
  for (auto &s : machine->symbols ()) symbols.push_back (&s);
  REQUIRE (symbols.size () == 2);
  CHECK (symbols[0] != symbols[1]);
  CHECK (symbols[0]->anonymous_ordinal != symbols[1]->anonymous_ordinal);
  auto again = mirnext::codegen::detail::access (*(machine)).intern_symbol (*symbols[0]);
  REQUIRE ((again).has_value ());
  CHECK (&(again).value () == symbols[0]);
  REQUIRE ((machine->verify ()).has_value ());
  auto conflicting = *symbols[0];
  conflicting.kind = MachineSymbol::Kind::Forward;
  auto revision = machine->revision ();
  CHECK_FALSE (
    (mirnext::codegen::detail::access (*(machine)).intern_symbol (conflicting)).has_value ());
  CHECK (machine->revision () == revision);
  CHECK (machine->dump ().find ("anonymous#") != std::string::npos);
}
TEST_CASE (
  "architecture state invalid bindings and backend private opcodes have checked boundaries") {
  auto f = prepared ();
  auto &b = *f->blocks ().begin ();
  auto anchor = &*b.instructions ().begin ();
  MachineRewriter rw (*f);
  auto old = f->revision ();
  auto state = f->state ();
  if (f->target ().architecture () == target::Architecture::AArch64)
    state.target = MachineFunctionState::X86{};
  else
    state.target = MachineFunctionState::AArch64{};
  CHECK_FALSE ((rw.set_state (state)).has_value ());
  CHECK (f->revision () == old);
  REQUIRE ((rw.insert_before (*anchor, MachinePseudoOpcode::Use, {})).has_value ());
  CHECK_FALSE (
    (rw.insert_before (*anchor, static_cast<MachinePseudoOpcode> (99), {})).has_value ());
  CHECK_FALSE ((rw.insert_before (*anchor, MachinePseudoOpcode::X86Mov128, {})).has_value ());
  auto reserved
    = std::find_if (f->target ().registers ().begin (), f->target ().registers ().end (),
                    [] (auto &r) { return r.reserved; });
  REQUIRE (reserved != f->target ().registers ().end ());
  CHECK_FALSE (
    (mirnext::codegen::detail::access (*(f)).create_register (Type::i64 (), "bad-binding", false,
                                                              false, reserved->reg))
      .has_value ());
  auto reg = mirnext::codegen::detail::access (*(f)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  CHECK_FALSE (
    (rw.insert_before (*anchor, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          BuiltinReference{static_cast<Builtin> (-1)})}))
      .has_value ());
  REQUIRE ((f->verify ()).has_value ());
  REQUIRE (
    (rw.append (b, Opcode::Mov,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((reg).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))}))
      .has_value ());
  CHECK_FALSE ((f->verify ()).has_value ());
  CHECK_FALSE ((MachineControlFlow::build (*f)).has_value ());
}

TEST_CASE ("machine editor batches retain identity across owner moves") {
  MachineFunction source ("edit", "entry", {{Type::i64 ()}, {}}, host ());
  MachineEditor editor (source);
  auto entry = editor.block ("entry");
  auto value = editor.reg (Type::i64 (), "value");
  auto assigned = editor.append (entry, Opcode::Mov, {value, std::int64_t (42)});
  editor.append (entry, Opcode::Ret, {value});
  REQUIRE ((assigned.get ()).has_value ());
  auto identity = &(value.get ()).value ();
  CHECK_FALSE ((source.verify ()).has_value ());
  MachineFunction destination (std::move (source));
  REQUIRE ((editor.finish ()).has_value ());
  REQUIRE ((destination.verify ()).has_value ());
  CHECK (&(value.get ()).value () == identity);
  CHECK (&identity->function () == &destination);
  CHECK (&*destination.blocks ().begin () == &(entry.get ()).value ());
  CHECK_FALSE ((editor.finish ()).has_value ());
  CHECK_FALSE ((destination.verify ()).has_value ());
}
TEST_CASE ("machine editing failures stop writes and preserve the first diagnostic") {
  auto function = prepared ();
  auto &block = *function->blocks ().begin ();
  auto &ret = *std::prev (block.instructions ().end ());
  MachineEditor editor (*function);
  editor.replace (ret, Opcode::Ret, std::array{MachineInput (std::int64_t (42))});
  auto retained = function->instruction_count ();
  auto invalid = editor.reg (Type (Type::Kind::Undef));
  REQUIRE ((editor.error ()).has_value ());
  auto first = editor.error ().value ().message;
  editor.block ("must-not-appear");
  editor.erase (ret);
  CHECK (function->instruction_count () == retained);
  CHECK_FALSE ((invalid.get ()).has_value ());
  CHECK (editor.error ().value ().message == first);
  CHECK_FALSE ((editor.finish ()).has_value ());
  CHECK_FALSE ((function->verify ()).has_value ());
  CHECK_FALSE ((MachineGraphAnalysis::build (*function)).has_value ());
}
TEST_CASE ("machine node deletion and foreign operands are diagnosed before mutation") {
  auto function = prepared ();
  auto &block = *function->blocks ().begin ();
  MachineEditor editor (*function);
  auto temporary = editor.reg (Type::i64 (), "scratch");
  auto instruction
    = editor.insert_before (*block.instructions ().begin (), Opcode::Mov,
                            std::array{MachineInput (temporary), MachineInput (std::int64_t (1))});
  REQUIRE ((instruction.get ()).has_value ());
  editor.erase (instruction);
  CHECK_FALSE ((instruction.get ()).has_value ());
  REQUIRE ((editor.finish ()).has_value ());
  auto other = prepared ();
  MachineEditor second (*other);
  auto count = other->instruction_count ();
  second.insert_before (*other->blocks ().begin ()->instructions ().begin (), Opcode::Mov,
                        std::array{MachineInput (temporary), MachineInput (std::int64_t (2))});
  CHECK_FALSE ((second.finish ()).has_value ());
  CHECK (other->instruction_count () == count);
}
TEST_CASE ("discarded machine edit batches remain invalid") {
  auto function = prepared ();
  {
    MachineEditor editor (*function);
  }
  auto checked = function->verify ();
  REQUIRE_FALSE ((checked).has_value ());
  CHECK (checked.error ().message.find ("not finished") != std::string::npos);
}

TEST_CASE ("machine handles detect deletion by a subsequent completed batch") {
  auto function = prepared ();
  MachineEditor first (*function);
  auto temporary = first.reg (Type::i64 (), "later-erased");
  auto instruction = first.insert_before (*function->blocks ().begin ()->instructions ().begin (),
                                          Opcode::Mov, {temporary, std::int64_t (1)});
  REQUIRE ((first.finish ()).has_value ());
  REQUIRE ((instruction.get ()).has_value ());
  MachineEditor next (*function);
  next.erase (instruction);
  REQUIRE ((next.finish ()).has_value ());
  CHECK_FALSE ((instruction.get ()).has_value ());
  REQUIRE ((function->verify ()).has_value ());
}

static_assert (!std::is_constructible_v<MachineRegister, const MachineVirtualRegister *>);
static_assert (!std::is_constructible_v<MachineOperand, const MachineVirtualRegister *>);
static_assert (!std::is_constructible_v<MachineOperand, const MachineLabel *>);
