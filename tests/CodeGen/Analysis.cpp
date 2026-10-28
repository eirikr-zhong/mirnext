/* Licensed under the MIT License. Direct machine analyses without a runtime. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/Transforms/Optimization.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::codegen;
namespace {
target::TargetMachine native_target () {
  auto t = target::TargetMachine::native ();
  REQUIRE ((t).has_value ());
  return std::move ((t).value ());
}
MachineVirtualRegister *reg (MachineFunction &f, Type t = Type::i64 ()) {
  auto r = mirnext::codegen::detail::access (f).create_register (t);
  REQUIRE ((r).has_value ());
  return &(r).value ();
}
MachineInstruction *append (MachineRewriter &r, MachineBasicBlock &b, MachineOpcode code,
                            std::vector<MachineOperand> operands = {}) {
  auto i = r.append (b, code, std::move (operands));
  INFO (((i).has_value () ? "" : i.error ().message));
  REQUIRE ((i).has_value ());
  return &(i).value ();
}
template <class T>
bool has (const MachineLiveness &a, const SparseBitSet &set, T r) {
  auto v = a.contains (set, mirnext::codegen::detail::NodeAccess::make_MachineRegister (r));
  REQUIRE (v.has_value ());
  return v.value ();
}
}  // namespace
TEST_CASE ("machine loop numbering and liveness retain stable owners and reject stale graphs") {
  auto source = serialization::parse_text (
    "m:module\nf:func i64\nlocal i64:x\nmov x,42\nret x\nendfunc\nendmodule\n");
  REQUIRE ((source).has_value ());
  auto p = transforms::optimize ((source).value ());
  REQUIRE ((p).has_value ());
  auto f = prepare_machine_ir (*p.value ().find_function ("m", "f").get (), native_target ());
  REQUIRE ((f).has_value ());
  auto &function = (f).value ();
  auto graph = MachineGraphAnalysis::build (function);
  REQUIRE ((graph).has_value ());
  auto dump = function.dump ();
  auto live = MachineLiveness::analyze (function, (graph).value ());
  REQUIRE ((live).has_value ());
  CHECK (live.value ().current (function));
  CHECK (graph.value ().current (function));
  CHECK (function.dump () == dump);
  MachineFunction other ("m", "other", {}, native_target ());
  mirnext::codegen::detail::access (other).create_block ();
  CHECK_FALSE ((MachineLiveness::analyze (other, (graph).value ())).has_value ());
  CHECK_FALSE ((MachineLiveness::analyze (function, (graph).value (), {.optimization_level = 4}))
                 .has_value ());
  reg (function);
  CHECK_FALSE (graph.value ().current (function));
  CHECK_FALSE (live.value ().current (function));
  CHECK_FALSE ((MachineLiveness::analyze (function, (graph).value ())).has_value ());
  CHECK_FALSE (
    (live.value ().frequency (
       mirnext::codegen::detail::NodeAccess::make_MachineRegister (live.value ().registers ()[0])))
      .has_value ());
}
TEST_CASE ("machine register effects retain duplicate address uses and private instructions") {
  MachineFunction f ("m", "effects", {}, native_target ());
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter r (f);
  auto x = reg (f), y = reg (f);
  MachineMemory memory
    = mirnext::codegen::detail::NodeAccess::make_MachineMemory (Type::i64 (), x, x, 8, 1);
  auto store = append (r, b, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand (memory),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (*y)});
  auto load = append (r, b, Opcode::Mov,
                      {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*y),
                       mirnext::codegen::detail::NodeAccess::make_MachineOperand (memory)});
  auto label = mirnext::codegen::detail::access (f).create_label (b);
  REQUIRE ((label).has_value ());
  auto laddr
    = append (r, b, Opcode::LAddr,
              {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*y),
               mirnext::codegen::detail::NodeAccess::make_MachineOperand ((label).value ())});
  auto use = append (r, b, MachinePseudoOpcode::Use,
                     {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
                      mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
  auto a = machine_register_effects (f, *store);
  REQUIRE ((a).has_value ());
  CHECK (a.value ().definitions.empty ());
  CHECK (
    a.value ().uses
    == std::vector<MachineRegister>{mirnext::codegen::detail::NodeAccess::make_MachineRegister (x),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (x),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      y)});
  a = machine_register_effects (f, *load);
  REQUIRE ((a).has_value ());
  CHECK (a.value ().definitions
         == std::vector<MachineRegister>{
           mirnext::codegen::detail::NodeAccess::make_MachineRegister (y)});
  CHECK (
    a.value ().uses
    == std::vector<MachineRegister>{mirnext::codegen::detail::NodeAccess::make_MachineRegister (x),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      x)});
  a = machine_register_effects (f, *laddr);
  REQUIRE ((a).has_value ());
  CHECK (a.value ().definitions.empty ());
  CHECK (a.value ().uses
         == std::vector<MachineRegister>{
           mirnext::codegen::detail::NodeAccess::make_MachineRegister (y)});
  a = machine_register_effects (f, *use);
  REQUIRE ((a).has_value ());
  CHECK (
    a.value ().uses
    == std::vector<MachineRegister>{mirnext::codegen::detail::NodeAccess::make_MachineRegister (x),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      x)});
  MachineFunction other ("m", "other", {}, native_target ());
  CHECK_FALSE ((machine_register_effects (other, *use)).has_value ());
  if (f.target ().architecture () == target::Architecture::X86_64) {
    auto vector = f.target ().find_register ("xmm0");
    REQUIRE ((vector).has_value ());
    auto move
      = append (r, b, MachinePseudoOpcode::X86Mov128,
                {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((vector).value ()),
                 mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                   mirnext::codegen::detail::NodeAccess::make_MachineMemory (Type (Type::Kind::F64),
                                                                             x))});
    a = machine_register_effects (f, *move);
    REQUIRE ((a).has_value ());
    CHECK (a.value ().definitions
           == std::vector<MachineRegister>{
             mirnext::codegen::detail::NodeAccess::make_MachineRegister ((vector).value ())});
    CHECK (a.value ().uses
           == std::vector<MachineRegister>{
             mirnext::codegen::detail::NodeAccess::make_MachineRegister (x)});
  }
}
TEST_CASE (
  "machine liveness solves loops and counts operand occurrences with original loop weights") {
  for (unsigned level = 0; level < 4; ++level) {
    MachineFunction f ("m", "loop", {{Type::i64 ()}, {}}, native_target ());
    auto &entry = mirnext::codegen::detail::access (f).create_block (),
         &loop = mirnext::codegen::detail::access (f).create_block (),
         &exit = mirnext::codegen::detail::access (f).create_block ();
    auto label = mirnext::codegen::detail::access (f).create_label (loop);
    REQUIRE ((label).has_value ());
    auto x = reg (f);
    MachineRewriter r (f);
    append (r, entry, Opcode::Mov,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
    append (r, loop, Opcode::Add,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (1))});
    append (r, loop, Opcode::Bt,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((label).value ()),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
    append (r, exit, Opcode::Ret, {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
    auto graph = MachineGraphAnalysis::build (f);
    REQUIRE ((graph).has_value ());
    CHECK (graph.value ().has_loops ());
    CHECK (graph.value ().loop_depth ()[3] == 1);
    auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = level});
    REQUIRE ((live).has_value ());
    CHECK (
      (live.value ().frequency (mirnext::codegen::detail::NodeAccess::make_MachineRegister (x)))
        .value ()
      == (level ? 17 : 5));
    CHECK (has ((live).value (), live.value ().blocks ()[3].in, x));
    CHECK (has ((live).value (), live.value ().blocks ()[3].out, x));
    CHECK_FALSE (has ((live).value (), live.value ().blocks ()[2].in, x));
    CHECK (has ((live).value (), live.value ().blocks ()[2].kill, x));
    CHECK (live.value ().blocks ()[3].max_integer_pressure == (level ? 2 : 0));
    auto no_freq
      = MachineLiveness::analyze (f, (graph).value (),
                                  {.optimization_level = level, .calculate_frequencies = false});
    REQUIRE ((no_freq).has_value ());
    CHECK (
      (no_freq.value ().frequency (mirnext::codegen::detail::NodeAccess::make_MachineRegister (x)))
        .value ()
      == 0);
  }
}
TEST_CASE (
  "machine call clobbers physical arguments globals and dead uses preserve separate identities") {
  MachineFunction f ("m", "call", {}, native_target ());
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter r (f);
  auto prototype = mirnext::codegen::detail::access (f).intern_symbol (
    {"m", "prototype", MachineSymbol::Kind::Prototype, Signature{}});
  REQUIRE ((prototype).has_value ());
  auto callee = mirnext::codegen::detail::access (f).intern_symbol (
    {"m", "callee", MachineSymbol::Kind::Import, Signature{}});
  REQUIRE ((callee).has_value ());
  auto argument = f.target ().registers ()[0].reg;
  auto saved = f.target ().architecture () == target::Architecture::AArch64
                 ? f.target ().registers ()[19].reg
                 : f.target ().registers ()[3].reg;
  auto call = append (r, b, Opcode::Call,
                      {mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                         mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                           &(prototype).value ())),
                       mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                         mirnext::codegen::detail::NodeAccess::make_MachineSymbolReference (
                           &(callee).value ()))});
  REQUIRE ((r.set_call_arguments (*call, {&argument, 1})).has_value ());
  auto use = append (r, b, MachinePseudoOpcode::Use,
                     {mirnext::codegen::detail::NodeAccess::make_MachineOperand (argument),
                      mirnext::codegen::detail::NodeAccess::make_MachineOperand (saved)});
  append (r, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value ());
  REQUIRE ((live).has_value ());
  CHECK (has ((live).value (), live.value ().blocks ()[2].kill, argument));
  CHECK_FALSE (has ((live).value (), live.value ().blocks ()[2].kill, saved));
  CHECK (has ((live).value (), live.value ().blocks ()[2].gen, argument));
  CHECK (has ((live).value (), live.value ().blocks ()[2].gen, saved));
  auto deaths = live.value ().deaths (*use);
  REQUIRE ((deaths).has_value ());
  CHECK (deaths.value ().size () == 2);
  CHECK (
    std::vector<MachineRegister> (deaths.value ().begin (), deaths.value ().end ())
    == std::vector<MachineRegister>{mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      argument),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      saved)});
  auto globals = MachineLiveness::analyze (f, (graph).value (), {.global_registers = {&saved, 1}});
  REQUIRE ((globals).has_value ());
  CHECK (has ((globals).value (), globals.value ().blocks ()[1].out, saved));
  CHECK (has ((globals).value (), globals.value ().blocks ()[0].in, saved));
  auto low = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = 0});
  REQUIRE ((low).has_value ());
  CHECK_FALSE ((low.value ().deaths (*use)).has_value ());
}
TEST_CASE ("compact machine scans retain ordered typed sets and validate membership") {
  MachineFunction f ("m", "compact", {}, native_target ());
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter r (f);
  auto x = reg (f), y = reg (f);
  auto use = append (r, b, MachinePseudoOpcode::Use,
                     {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
                      mirnext::codegen::detail::NodeAccess::make_MachineOperand (*y),
                      mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
  append (r, b, Opcode::Ret);
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  std::array<MachineRegister, 2> scan{mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                        y),
                                      mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                        x)};
  auto live = MachineLiveness::analyze (f, (graph).value (), {.scan_registers = scan});
  REQUIRE ((live).has_value ());
  CHECK (live.value ().registers ()[0]
         == mirnext::codegen::detail::NodeAccess::make_MachineRegister (y));
  CHECK (live.value ().registers ()[1]
         == mirnext::codegen::detail::NodeAccess::make_MachineRegister (x));
  CHECK ((live.value ().frequency (mirnext::codegen::detail::NodeAccess::make_MachineRegister (x)))
           .value ()
         == 2);
  CHECK ((live.value ().frequency (mirnext::codegen::detail::NodeAccess::make_MachineRegister (y)))
           .value ()
         == 1);
  CHECK (live.value ().blocks ()[2].gen.contains (0));
  CHECK (live.value ().blocks ()[2].gen.contains (1));
  auto dead = live.value ().deaths (*use);
  REQUIRE ((dead).has_value ());
  CHECK (
    std::vector<MachineRegister> (dead.value ().begin (), dead.value ().end ())
    == std::vector<MachineRegister>{mirnext::codegen::detail::NodeAccess::make_MachineRegister (x),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      y)});
  scan[1] = mirnext::codegen::detail::NodeAccess::make_MachineRegister (y);
  CHECK_FALSE (
    (MachineLiveness::analyze (f, (graph).value (), {.scan_registers = scan})).has_value ());
  MachineFunction other ("m", "other", {}, native_target ());
  scan[1] = mirnext::codegen::detail::NodeAccess::make_MachineRegister (reg (other));
  CHECK_FALSE (
    (MachineLiveness::analyze (f, (graph).value (), {.scan_registers = scan})).has_value ());
}
TEST_CASE ("machine graph traversal does not recurse on long block chains") {
  MachineFunction f ("m", "chain", {}, native_target ());
  MachineRewriter r (f);
  for (unsigned n = 0; n < 4096; ++n) mirnext::codegen::detail::access (f).create_block ();
  append (r, *std::prev (f.blocks ().end ()), Opcode::Ret);
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  CHECK_FALSE (graph.value ().has_loops ());
  CHECK (graph.value ().preorder ().size () == 4098);
  CHECK (graph.value ().preorder ()[1] == 2);
  CHECK (graph.value ().preorder ()[4097] == 4098);
  CHECK (graph.value ().reverse_postorder ()[0] == 1);
  auto live = MachineLiveness::analyze (f, (graph).value ());
  REQUIRE ((live).has_value ());
  CHECK (live.value ().blocks ().size () == 4098);
}
TEST_CASE ("hardware declarations remain observable across all functions in their module") {
  auto target = native_target ();
  auto hardware = target.architecture () == target::Architecture::AArch64 ? "r19" : "r12";
  auto source = serialization::parse_text (
    "m:module\ng:func\nglobal i64:bound:" + std::string (hardware)
    + "\nmov bound,42\nret\nendfunc\nf:func\nret\nendfunc\nendmodule\n");
  REQUIRE ((source).has_value ());
  auto symbol = source.value ().modules ().begin ()->find_symbol ("f").get ();
  REQUIRE (symbol);
  auto function = symbol->function ().get ();
  REQUIRE (function);
  auto machine = prepare_machine_ir (*function, target);
  REQUIRE ((machine).has_value ());
  (source).value () = mirnext::ir::Program{};
  auto &f = (machine).value ();
  auto physical = target.find_register (hardware);
  REQUIRE ((physical).has_value ());
  REQUIRE (f.global_registers ().size () == 1);
  CHECK (f.global_registers ()[0] == (physical).value ());
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  auto live = MachineLiveness::analyze (f, (graph).value ());
  REQUIRE ((live).has_value ());
  CHECK (has ((live).value (), live.value ().blocks ()[1].out, (physical).value ()));
  CHECK (has ((live).value (), live.value ().blocks ()[0].in, (physical).value ()));
  MachineRewriter rewrite (f);
  auto reserved
    = target.registers ()[target.architecture () == target::Architecture::AArch64 ? 31 : 4].reg;
  auto revision = f.revision ();
  CHECK_FALSE ((rewrite.set_global_registers ({&reserved, 1})).has_value ());
  CHECK (f.revision () == revision);
  CHECK (f.global_registers ()[0] == (physical).value ());
}
TEST_CASE ("machine preparation observes late hardware declarations without cached module state") {
  auto target = native_target ();
  auto first = target.architecture () == target::Architecture::AArch64 ? "r20" : "r13";
  auto second = target.architecture () == target::Architecture::AArch64 ? "r19" : "r12";
  auto c = serialization::parse_text (
    "m:module\nf:func\nret\nendfunc\ng:func\nlocal "
    "i64:ordinary\nret\nendfunc\nendmodule\nother:module\nh:func\nglobal "
    "i64:unrelated:unknown\nret\nendfunc\nendmodule\n");
  REQUIRE ((c).has_value ());
  auto &module = *c.value ().modules ().begin ();
  auto &function = *module.functions ().begin ();
  auto declarations = std::next (module.functions ().begin ());
  CHECK_FALSE (function.has_hardware_bindings ());
  CHECK_FALSE (declarations->has_hardware_bindings ());
  auto before = prepare_machine_ir (function, target);
  REQUIRE ((before).has_value ());
  CHECK (before.value ().global_registers ().empty ());
  auto bound
    = mirnext::ir::detail::access (*(declarations)).create_register (Type::i64 (), "bound", first);
  REQUIRE ((bound).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (*(declarations)).create_register (Type::i64 (), "second", second))
      .has_value ());
  auto alias = mirnext::ir::detail::access (*(declarations))
                 .create_register (Type (Type::Kind::U32), "alias", first);
  REQUIRE ((alias).has_value ());
  CHECK (&(alias).value () == &(bound).value ());
  CHECK_FALSE ((mirnext::ir::detail::access (*(declarations))
                  .create_register (Type (Type::Kind::F64), "incompatible", first))
                 .has_value ());
  CHECK (declarations->has_hardware_bindings ());
  auto physical_first = target.find_register (first),
       physical_second = target.find_register (second);
  REQUIRE ((physical_first).has_value ());
  REQUIRE ((physical_second).has_value ());
  auto check = [&] (const Function &f) {
    auto machine = prepare_machine_ir (f, target);
    REQUIRE ((machine).has_value ());
    auto globals = machine.value ().global_registers ();
    REQUIRE (globals.size () == 2);
    CHECK (globals[0] == (physical_first).value ());
    CHECK (globals[1] == (physical_second).value ());
  };
  check (function);
  auto copy = clone ((c).value ());
  REQUIRE ((copy).has_value ());
  auto copied = copy.value ().find_module ("m").get ();
  REQUIRE (copied);
  (c).value () = mirnext::ir::Program{};
  check (*copied->functions ().begin ());
  CHECK (before.value ().global_registers ().empty ());
}
TEST_CASE ("nested machine loops retain their complete forest without recursive ancestry scans") {
  constexpr unsigned count = 512;
  MachineFunction f ("m", "nested", {}, native_target ());
  MachineRewriter r (f);
  auto x = reg (f);
  auto &entry = mirnext::codegen::detail::access (f).create_block ();
  append (r, entry, Opcode::Mov,
          {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
           mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::int64_t (0))});
  std::vector<MachineLabel *> labels;
  for (unsigned n = 0; n < count; ++n) {
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    auto label = mirnext::codegen::detail::access (f).create_label (b);
    REQUIRE ((label).has_value ());
    labels.push_back (&(label).value ());
    append (r, b, Opcode::Mov,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
  }
  for (unsigned n = count; n-- > 0;) {
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    append (r, b, Opcode::Bt,
            {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*labels[n]),
             mirnext::codegen::detail::NodeAccess::make_MachineOperand (*x)});
  }
  append (r, mirnext::codegen::detail::access (f).create_block (), Opcode::Ret);
  auto graph = MachineGraphAnalysis::build (f);
  REQUIRE ((graph).has_value ());
  CHECK (graph.value ().has_loops ());
  CHECK (graph.value ().loop_depth ()[count + 2] == count);
  CHECK (graph.value ().loops ().size ()
         == mirnext::codegen::detail::NodeAccess::blocks (graph.value ().cfg ()).size () + count
              + 1);
  auto live = MachineLiveness::analyze (f, (graph).value ());
  REQUIRE ((live).has_value ());
  CHECK ((live.value ().frequency (mirnext::codegen::detail::NodeAccess::make_MachineRegister (x)))
           .value ()
         > 0);
}
