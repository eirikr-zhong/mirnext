/* Licensed under the MIT License. Native machine ownership and preparation. */
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
  REQUIRE (t);
  return std::move (*t);
}
std::unique_ptr<Context> parse (std::string_view text) {
  auto c = serialization::parse_text (text);
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  return std::move (*c);
}
const char *simple
  = "m:module\nf:func i64, i64:a\nlocal i64:r\nadd r, a, 2\nret r\nendfunc\nendmodule\n";
std::unique_ptr<MachineFunction> prepared (std::string_view text = simple) {
  auto c = parse (text);
  auto p = transforms::optimize (*c);
  INFO ((p ? "" : p.error ().message));
  REQUIRE (p);
  auto f = p->find_function ("m", "f");
  REQUIRE (f);
  auto m = prepare_machine_ir (*f, host ());
  INFO ((m ? "" : m.error ().message));
  REQUIRE (m);
  return std::move (*m);
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
  CHECK_FALSE (target.find_register ("missing"));
  CHECK_FALSE (target.layout (Type::block (8)));
  CHECK_FALSE (target.layout (Type (Type::Kind::Undef)));
  CHECK (*target.layout (Type::i64 ()) == target::TypeLayout{8, 8});
  CHECK (*target.layout (Type (Type::Kind::Bool)) == target::TypeLayout{8, 8});
  CHECK (*target.layout (Type (Type::Kind::LongDouble))
         == target::TypeLayout{sizeof (long double), alignof (long double)});
  for (auto &info : target.registers ()) {
    auto r = target.find_register (info.name);
    REQUIRE (r);
    CHECK (*r == info.reg);
    CHECK (target.register_info (*r) == &info);
    CHECK (target.accepts (*r, Type::i64 ()) == (info.bank == target::RegisterBank::Integer));
  }
  auto opposite = target.architecture () == target::Architecture::AArch64
                    ? target::Architecture::X86_64
                    : target::Architecture::AArch64;
  CHECK_FALSE (target::TargetMachine::native (opposite));
  CHECK_FALSE (target::TargetMachine::native (static_cast<target::Architecture> (100)));
}
TEST_CASE (
  "machine preparation owns operands names signatures and nodes beyond both source lifetimes") {
  auto m = prepared ();
  REQUIRE (m->verify ());
  auto first = m->dump ();
  REQUIRE_FALSE (first.empty ());
  auto cfg = MachineControlFlow::build (*m);
  REQUIRE (cfg);
  CHECK (cfg->current (*m));
  CHECK (m->arguments ().size () == 1);
  CHECK (m->signature ().parameters == std::vector<Type>{Type::i64 ()});
  auto ptr = m.get ();
  auto moved = std::move (m);
  CHECK (moved.get () == ptr);
  CHECK (moved->dump () == first);
  REQUIRE (moved->verify ());
  CHECK (cfg->current (*moved));
}
TEST_CASE ("rewrites retain instruction and operand addresses across block ownership transfers") {
  auto f = prepared ();
  auto &first = *f->blocks ().begin ();
  auto &other = f->create_block ("other");
  MachineRewriter r (*f);
  auto reg = f->create_register (Type::i64 (), "temporary");
  REQUIRE (reg);
  auto mov
    = r.append (other, Opcode::Mov, {MachineOperand (**reg), MachineOperand (std::int64_t (42))});
  REQUIRE (mov);
  auto ret = r.append (other, Opcode::Ret, {MachineOperand (**reg)});
  REQUIRE (ret);
  auto address = *mov;
  auto operands = (*mov)->operands ().data ();
  auto anchor = const_cast<MachineInstruction *> (&*first.instructions ().begin ());
  REQUIRE (r.move_before (**mov, *anchor));
  CHECK (*mov == address);
  CHECK ((*mov)->operands ().data () == operands);
  CHECK (&(*mov)->block () == &first);
  REQUIRE (
    r.replace (**mov, Opcode::Mov, {MachineOperand (**reg), MachineOperand (std::int64_t (7))}));
  CHECK (*mov == address);
  auto revision = f->revision ();
  auto count = f->instruction_count ();
  CHECK_FALSE (r.replace (**mov, Opcode::Add, {}));
  CHECK (f->revision () == revision);
  CHECK (f->instruction_count () == count);
  REQUIRE (r.erase (**mov));
  REQUIRE (f->verify ());
}
TEST_CASE ("machine hardware bindings cannot become addressable storage") {
  auto target = host ();
  MachineFunction f ("m", "entry", {{Type::i64 ()}, {}}, target);
  auto physical = target.find_register (
    target.architecture () == target::Architecture::AArch64 ? "r19" : "r12");
  REQUIRE (physical);
  auto revision = f.revision ();
  CHECK_FALSE (f.create_register (Type::i64 (), "invalid", false, true, *physical));
  CHECK (f.revision () == revision);
  auto bound = f.create_register (Type::i64 (), "bound", false, false, *physical);
  REQUIRE (bound);
  auto pointer = f.create_register (Type::pointer (), "pointer");
  REQUIRE (pointer);
  auto &block = f.create_block ();
  MachineRewriter rw (f);
  revision = f.revision ();
  for (auto opcode : {Opcode::Addr, Opcode::Addr8, Opcode::Addr16, Opcode::Addr32}) {
    CHECK_FALSE (rw.append (block, opcode, {MachineOperand (**pointer), MachineOperand (**bound)}));
    CHECK (f.revision () == revision);
    CHECK (f.instruction_count () == 0);
  }
}
TEST_CASE ("foreign machine handles and wrong instruction types are atomic diagnostics") {
  auto f = prepared (), other = prepared ();
  auto &b = *f->blocks ().begin ();
  MachineRewriter rw (*f);
  auto reg = other->create_register (Type::i64 ());
  REQUIRE (reg);
  auto label = other->create_label (*other->blocks ().begin ());
  REQUIRE (label);
  auto slot = other->create_stack_slot (8, 8);
  REQUIRE (slot);
  auto local = f->create_register (Type::i64 ());
  REQUIRE (local);
  auto revision = f->revision ();
  auto count = f->instruction_count ();
  CHECK_FALSE (rw.append (b, Opcode::Mov, {MachineOperand (**local), MachineOperand (**reg)}));
  CHECK_FALSE (rw.append (b, Opcode::Jmp, {MachineOperand (**label)}));
  CHECK_FALSE (rw.append (b, Opcode::Mov,
                          {MachineOperand (**local), MachineOperand (StackSlotReference{*slot})}));
  CHECK_FALSE (rw.append (b, Opcode::DMov, {MachineOperand (**local), MachineOperand (2.0)}));
  CHECK_FALSE (rw.append (b, Opcode::Label, {}));
  CHECK_FALSE (rw.append (b, MachinePseudoOpcode::Use, {MachineOperand (std::int64_t (1))}));
  CHECK_FALSE (
    rw.append (b, Opcode::Mov,
               {MachineOperand (**local), MachineOperand (MachineSymbolReference{nullptr})}));
  CHECK_FALSE (
    rw.append (*other->blocks ().begin (), Opcode::Ret, {MachineOperand (std::int64_t (0))}));
  CHECK (f->revision () == revision);
  CHECK (f->instruction_count () == count);
  REQUIRE (f->verify ());
  CHECK_FALSE (f->create_stack_slot (0, 8));
  CHECK_FALSE (f->create_stack_slot (8, 3));
}
TEST_CASE (
  "stack slots physical registers builtins and call register sets retain separate identities") {
  auto f = prepared (
    "m:module\np:proto i64\nimport ext\nf:func i64\nlocal i64:r\ncall p, ext, r\nret "
    "r\nendfunc\nendmodule\n");
  auto reg = f->create_register (Type::i64 ());
  REQUIRE (reg);
  auto slot = f->create_stack_slot (16, 16);
  REQUIRE (slot);
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
  REQUIRE (rw.set_call_arguments (*call, args));
  CHECK (call->call_arguments ().size () == 1);
  CHECK (call->operands ().data () == original);
  REQUIRE (
    rw.insert_before (*call, Opcode::Mov, {MachineOperand (**reg), MachineOperand (integer->reg)}));
  REQUIRE (
    rw.insert_before (*call, Opcode::Mov,
                      {MachineOperand (**reg), MachineOperand (StackSlotReference{*slot, 8})}));
  REQUIRE (rw.insert_before (*call, Opcode::Mov,
                             {MachineOperand (**reg),
                              MachineOperand (BuiltinReference{Builtin::CopyBlock})}));
  CHECK (f->dump ().find ("call-regs") != std::string::npos);
  CHECK (f->dump ().find ("slot0+8") != std::string::npos);
  REQUIRE (f->verify ());
}
TEST_CASE ("hardware identity is shared and every ordinary return has a typed USE") {
  auto target = host ();
  auto hardware = target.architecture () == target::Architecture::AArch64 ? "r19" : "r12";
  auto c = parse (std::string ("m:module\nf:func i64, i64:c\nglobal i64:h:") + hardware
                  + "\nmov h,42\nbt yes,c\nret h\nyes:\nret h\nendfunc\nendmodule\n");
  auto &source = *c->modules ().begin ()->functions ().begin ();
  auto before = dump (*c);
  auto m = prepare_machine_ir (source, target);
  REQUIRE (m);
  CHECK (dump (*c) == before);
  std::size_t returns = 0, uses = 0;
  for (auto &b : (*m)->blocks ())
    for (auto &i : b.instructions ()) {
      returns += i.opcode () == MachineOpcode (Opcode::Ret);
      uses += i.opcode () == MachineOpcode (MachinePseudoOpcode::Use);
    }
  CHECK (returns == 2);
  CHECK (uses == returns);
  const MachineVirtualRegister *bound = nullptr;
  for (auto &r : (*m)->registers ())
    if (r.binding ()) bound = &r;
  REQUIRE (bound);
  auto alias
    = (*m)->create_register (Type (Type::Kind::U32), "alias", false, false, bound->binding ());
  REQUIRE (alias);
  CHECK (*alias == bound);
  CHECK_FALSE (
    (*m)->create_register (Type (Type::Kind::F64), "wrong", false, false, bound->binding ()));
}
TEST_CASE (
  "preparation rejects unmaterialized scope SSA and unsimplified strings without source mutation") {
  auto c = parse (simple);
  auto &f = *c->modules ().begin ()->functions ().begin ();
  transforms::FunctionPassManager passes;
  passes.add<transforms::PrepareCFGPass> ();
  passes.add<transforms::BuildSSAPass> ();
  analysis::FunctionAnalysisManager a (f);
  REQUIRE (passes.run (f, a));
  CHECK_FALSE (prepare_machine_ir (f, host ()));
  Context scoped;
  auto m = scoped.create_module ("scoped");
  REQUIRE (m);
  auto fun = (*m)->create_function ("f", {{}, {}});
  REQUIRE (fun);
  auto b = (*fun)->create_block ();
  REQUIRE (b);
  IRBuilder builder (**b);
  auto scope = builder.begin_stack_scope ();
  REQUIRE (scope);
  REQUIRE (builder.ret ());
  auto before = dump (scoped);
  auto result = prepare_machine_ir (**fun, host ());
  CHECK_FALSE (result);
  CHECK (dump (scoped) == before);
  CHECK (result.error ().message.find ("prepare-machine-ir") != std::string::npos);
}
TEST_CASE ("machine CFG preserves duplicate switch edges address roots and indirect targets") {
  auto c = parse (
    "m:module\nf:func i64, i64:a\nlocal i64:p\nladdr p, target\nswitch a, target, "
    "target\nunused:\nret 1\ntarget:\njmpi p\nendfunc\nref:lref target, 0\nendmodule\n");
  auto &f = *c->modules ().begin ()->functions ().begin ();
  auto m = prepare_machine_ir (f, host ());
  INFO ((m ? "" : m.error ().message));
  REQUIRE (m);
  auto cfg = MachineControlFlow::build (**m);
  REQUIRE (cfg);
  std::size_t cases = 0, indirect = 0;
  for (auto &e : cfg->edges) {
    cases += e.kind == MachineControlFlow::EdgeKind::SwitchCase;
    indirect += e.kind == MachineControlFlow::EdgeKind::Indirect;
  }
  CHECK (cases == 2);
  CHECK (indirect == 1);
  CHECK (cfg->current (**m));
  (*m)->create_block ();
  CHECK_FALSE (cfg->current (**m));
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
      INFO ((p ? "" : p.error ().message));
      REQUIRE (p);
      for (auto &m : p->context ().modules ())
        for (auto &f : m.functions ()) {
          auto machine = prepare_machine_ir (f, target);
          INFO ((machine ? "" : machine.error ().message));
          REQUIRE (machine);
          REQUIRE ((*machine)->verify ());
          auto cfg = MachineControlFlow::build (**machine);
          REQUIRE (cfg);
          CHECK (cfg->blocks.size () == blocks (**machine) + 2);
          CHECK_FALSE ((*machine)->dump ().empty ());
          auto graph = MachineGraphAnalysis::build (**machine);
          REQUIRE (graph);
          auto live = MachineLiveness::analyze (**machine, *graph, {.optimization_level = level});
          REQUIRE (live);
          CHECK (live->current (**machine));
        }
    }
}
TEST_CASE ("machine relocations preserve distinct anonymous data identities") {
  auto machine = prepared ();
  auto a = machine->intern_symbol ({"data", {}, MachineSymbol::Kind::Data, {}, false, 0});
  auto b = machine->intern_symbol ({"data", {}, MachineSymbol::Kind::Data, {}, false, 1});
  REQUIRE (a);
  REQUIRE (b);
  auto reg = machine->create_register (Type::i64 ());
  REQUIRE (reg);
  MachineRewriter rw (*machine);
  auto anchor = &*machine->blocks ().begin ()->instructions ().begin ();
  REQUIRE (
    rw.insert_before (*anchor, Opcode::Mov,
                      {MachineOperand (**reg), MachineOperand (MachineSymbolReference{*a})}));
  REQUIRE (
    rw.insert_before (*anchor, Opcode::Mov,
                      {MachineOperand (**reg), MachineOperand (MachineSymbolReference{*b})}));
  std::vector<const MachineSymbol *> symbols;
  for (auto &s : machine->symbols ()) symbols.push_back (&s);
  REQUIRE (symbols.size () == 2);
  CHECK (symbols[0] != symbols[1]);
  CHECK (symbols[0]->anonymous_ordinal != symbols[1]->anonymous_ordinal);
  auto again = machine->intern_symbol (*symbols[0]);
  REQUIRE (again);
  CHECK (*again == symbols[0]);
  REQUIRE (machine->verify ());
  auto conflicting = *symbols[0];
  conflicting.kind = MachineSymbol::Kind::Forward;
  auto revision = machine->revision ();
  CHECK_FALSE (machine->intern_symbol (conflicting));
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
  CHECK_FALSE (rw.set_state (state));
  CHECK (f->revision () == old);
  REQUIRE (rw.insert_before (*anchor, MachinePseudoOpcode::Use, {}));
  CHECK_FALSE (rw.insert_before (*anchor, static_cast<MachinePseudoOpcode> (99), {}));
  CHECK_FALSE (rw.insert_before (*anchor, MachinePseudoOpcode::X86Mov128, {}));
  auto reserved
    = std::find_if (f->target ().registers ().begin (), f->target ().registers ().end (),
                    [] (auto &r) { return r.reserved; });
  REQUIRE (reserved != f->target ().registers ().end ());
  CHECK_FALSE (f->create_register (Type::i64 (), "bad-binding", false, false, reserved->reg));
  auto reg = f->create_register (Type::i64 ());
  REQUIRE (reg);
  CHECK_FALSE (rw.insert_before (*anchor, Opcode::Mov,
                                 {MachineOperand (**reg),
                                  MachineOperand (BuiltinReference{static_cast<Builtin> (-1)})}));
  REQUIRE (f->verify ());
  REQUIRE (rw.append (b, Opcode::Mov, {MachineOperand (**reg), MachineOperand (std::int64_t (1))}));
  CHECK_FALSE (f->verify ());
  CHECK_FALSE (MachineControlFlow::build (*f));
}
