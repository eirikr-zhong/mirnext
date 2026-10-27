/* Licensed under the MIT License. Target lowering over owning machine IR. */
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
  INFO ((c ? "" : c.error ().message));
  REQUIRE (c);
  transforms::OptimizationOptions options;
  options.level = static_cast<transforms::OptimizationLevel> (level);
  auto p = transforms::optimize (**c, options);
  INFO ((p ? "" : p.error ().message));
  REQUIRE (p);
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  auto f = p->find_function ("m", "f");
  REQUIRE (f);
  auto m = prepare_machine_ir (*f, *target);
  INFO ((m ? "" : m.error ().message));
  REQUIRE (m);
  return std::move (*m);
}
bool arm () {
  return target::TargetMachine::native ()->architecture () == target::Architecture::AArch64;
}
}  // namespace
TEST_CASE ("machine instruction navigation and insertion preserve owners and revisions") {
  auto f = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  auto &block = *f->blocks ().begin ();
  auto &first = *block.instructions ().begin ();
  MachineRewriter rw (*f);
  REQUIRE (rw.previous (first));
  CHECK (*rw.previous (first) == nullptr);
  auto inserted = rw.insert_after (first, MachinePseudoOpcode::Use, {});
  REQUIRE (inserted);
  CHECK (*rw.next (first) == *inserted);
  CHECK (*rw.previous (**inserted) == &first);
  auto other = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  MachineRewriter foreign (*other);
  auto revision = other->revision ();
  CHECK_FALSE (foreign.previous (first));
  CHECK_FALSE (foreign.next (first));
  CHECK_FALSE (foreign.insert_after (first, MachinePseudoOpcode::Use, {}));
  CHECK (other->revision () == revision);
  REQUIRE (rw.erase (**inserted));
  REQUIRE (f->verify ());
}
TEST_CASE ("AArch64 target lowering covers every public opcode") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  for (auto &d : instruction_descriptors ())
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (d.name);
      CAPTURE (level);
      auto source = test::public_instruction_example (d);
      transforms::OptimizationOptions options;
      options.level = static_cast<transforms::OptimizationLevel> (level);
      auto p = transforms::optimize (*source, options);
      REQUIRE (p);
      for (auto &module : p->context ().modules ())
        for (auto &f : module.functions ()) {
          auto machine = prepare_machine_ir (f, *target);
          REQUIRE (machine);
          auto lowered = machinize (**machine);
          INFO ((lowered ? "" : lowered.error ().message));
          REQUIRE (lowered);
          REQUIRE ((*machine)->verify ());
          CHECK ((*machine)->state ().machinized);
          CHECK_FALSE ((*machine)->dump ().empty ());
          REQUIRE (legalize_operands (**machine));
          CHECK ((*machine)->state ().operands_legalized);
          REQUIRE ((*machine)->verify ());
          auto cfg = MachineGraphAnalysis::build (**machine);
          REQUIRE (cfg);
          auto merged = coalesce_registers (**machine, *cfg, level);
          REQUIRE (merged);
          cfg = MachineGraphAnalysis::build (**machine);
          REQUIRE (cfg);
          auto live = MachineLiveness::analyze (**machine, *cfg, {.optimization_level = level});
          REQUIRE (live);
          auto ranges = MachineLiveRanges::build (**machine, *cfg, *live);
          REQUIRE (ranges);
          auto allocation = MachineAllocation::assign (**machine, *cfg, *live, *ranges);
          REQUIRE (allocation);
          auto rewritten
            = rewrite_allocated (**machine, *cfg, *live, *ranges, std::move (*allocation));
          INFO ((rewritten ? "" : rewritten.error ().message));
          REQUIRE (rewritten);
          REQUIRE ((*machine)->verify ());
          auto spilled = split_allocated_spills (**machine, std::move (*rewritten));
          INFO ((spilled ? "" : spilled.error ().message));
          REQUIRE (spilled);
          REQUIRE (spilled->cfg ().verify (**machine));
          auto post = optimize_allocated_machine (**machine, std::move (*spilled));
          INFO ((post ? "" : post.error ().message));
          REQUIRE (post);
          REQUIRE (post->cfg ().verify (**machine));
          auto framed = form_frame (**machine, std::move (*post));
          INFO ((framed ? "" : framed.error ().message));
          REQUIRE (framed);
          REQUIRE (framed->cfg ().verify (**machine));
          auto encoded = encode_machine (**machine, *framed);
          INFO ((encoded ? "" : encoded.error ().message));
          REQUIRE (encoded);
          REQUIRE (encoded->verify ());
        }
    }
}
TEST_CASE ("lowering owns state after source destruction and rejects a repeated invocation") {
  auto f = input (
    "m:module\np:proto i64,i64:x\nimport ext\nf:func i64,i64:x\nlocal i64:r\ncall p,ext,r,x\nret "
    "r\nendfunc\nendmodule\n");
  auto before = f->dump ();
  auto result = machinize (*f);
  INFO ((result ? "" : result.error ().message));
  REQUIRE (result);
  REQUIRE (f->verify ());
  CHECK_FALSE (f->state ().is_leaf);
  const MachineInstruction *call = nullptr;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.has_lowered_call ()) call = &i;
  REQUIRE (call);
  CHECK (call->operands ()[2].get<target::PhysicalRegister> ());
  CHECK (call->operands ()[3].get<target::PhysicalRegister> ());
  auto lowered = f->dump ();
  auto revision = f->revision ();
  CHECK_FALSE (machinize (*f));
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
      INFO ((r ? "" : r.error ().message));
      REQUIRE (r);
      REQUIRE (f->verify ());
      REQUIRE (legalize_operands (*f));
      REQUIRE (f->verify ());
      auto graph = MachineGraphAnalysis::build (*f);
      REQUIRE (graph);
      auto merged = coalesce_registers (*f, *graph, level);
      REQUIRE (merged);
      graph = MachineGraphAnalysis::build (*f);
      REQUIRE (graph);
      auto live = MachineLiveness::analyze (*f, *graph, {.optimization_level = level});
      REQUIRE (live);
      auto ranges = MachineLiveRanges::build (*f, *graph, *live);
      REQUIRE (ranges);
      auto allocation = MachineAllocation::assign (*f, *graph, *live, *ranges);
      REQUIRE (allocation);
      auto rewritten = rewrite_allocated (*f, *graph, *live, *ranges, std::move (*allocation));
      INFO ((rewritten ? "" : rewritten.error ().message));
      REQUIRE (rewritten);
      REQUIRE (f->verify ());
      auto spilled = split_allocated_spills (*f, std::move (*rewritten));
      INFO ((spilled ? "" : spilled.error ().message));
      REQUIRE (spilled);
      REQUIRE (spilled->cfg ().verify (*f));
      auto post = optimize_allocated_machine (*f, std::move (*spilled));
      INFO ((post ? "" : post.error ().message));
      REQUIRE (post);
      REQUIRE (post->cfg ().verify (*f));
      auto framed = form_frame (*f, std::move (*post));
      INFO ((framed ? "" : framed.error ().message));
      REQUIRE (framed);
      REQUIRE (framed->cfg ().verify (*f));
      auto encoded = encode_machine (*f, *framed);
      INFO ((encoded ? "" : encoded.error ().message));
      REQUIRE (encoded);
      REQUIRE (encoded->verify ());
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
  CHECK_FALSE (rw.lower_call (*call, Opcode::Call,
                              std::vector<MachineOperand> (call->operands ().begin (),
                                                           call->operands ().end ())));
  CHECK (f->revision () == revision);
  CHECK (f->dump () == before);
  CHECK_FALSE (call->has_lowered_call ());
}
TEST_CASE ("AArch64 unsupported result counts and unsimplified inputs fail before rewriting") {
  if (!arm ()) return;
  auto target = target::TargetMachine::native ();
  REQUIRE (target);
  SUBCASE ("unsimplified return") {
    MachineFunction f ("bad", "constant", {{Type::i64 ()}, {}}, *target);
    auto &b = f.create_block ();
    MachineRewriter rw (f);
    REQUIRE (rw.append (b, Opcode::Ret, {MachineOperand (std::int64_t (42))}));
    auto before = f.dump ();
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find ("machinize") != std::string::npos);
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("too many return registers") {
    MachineFunction f ("bad", "returns", {std::vector<Type> (9, Type::i64 ()), {Type::i64 ()}},
                       *target);
    auto arg = f.create_register (Type::i64 (), "arg", true);
    REQUIRE (arg);
    auto &b = f.create_block ();
    MachineRewriter rw (f);
    REQUIRE (rw.append (b, Opcode::Ret, std::vector<MachineOperand> (9, MachineOperand (**arg))));
    auto before = f.dump ();
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find ("return values") != std::string::npos);
    CHECK (f.dump () == before);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("unpromoted variadic float") {
    MachineFunction f ("bad", "varargs", {{}, {}}, *target);
    auto &b = f.create_block ();
    MachineRewriter rw (f);
    auto x = f.create_register (Type::i64 ());
    auto y = f.create_register (Type (Type::Kind::F32));
    REQUIRE (x);
    REQUIRE (y);
    auto proto = f.intern_symbol (
      {"bad", "p", MachineSymbol::Kind::Prototype, Signature{{}, {Type::i64 ()}, true}, false, {}});
    REQUIRE (proto);
    REQUIRE (rw.append (b, Opcode::Call,
                        {MachineOperand (MachineSymbolReference{*proto}), MachineOperand (**x),
                         MachineOperand (**x), MachineOperand (**y)}));
    REQUIRE (rw.append (b, Opcode::Ret, {}));
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find ("promoted") != std::string::npos);
    CHECK (f.revision () == revision);
  }
  SUBCASE ("combined outgoing area overflow") {
    MachineFunction f ("bad", "area", {{}, {}}, *target);
    auto &b = f.create_block ();
    MachineRewriter rw (f);
    auto pointer = f.create_register (Type::i64 ());
    REQUIRE (pointer);
    auto block = Type::block (static_cast<std::size_t> (INT64_MAX) - 15);
    auto proto = f.intern_symbol (
      {"bad", "p", MachineSymbol::Kind::Prototype, Signature{{}, {block, block}}, false, {}});
    REQUIRE (proto);
    auto memory
      = MachineOperand (MachineMemory{block, MachineRegister (*pointer), {}, 0, 1, {}, {}});
    REQUIRE (rw.append (b, Opcode::Call,
                        {MachineOperand (MachineSymbolReference{*proto}),
                         MachineOperand (**pointer), memory, memory}));
    REQUIRE (rw.append (b, Opcode::Ret, {}));
    auto revision = f.revision ();
    auto result = machinize (f);
    REQUIRE_FALSE (result);
    CHECK (result.error ().message.find ("argument area") != std::string::npos);
    CHECK (f.revision () == revision);
  }
}
TEST_CASE ("builtin prototypes have typed identities distinct from user symbols") {
  auto f = input ("m:module\nf:func i64,i64:x\nret x\nendfunc\nendmodule\n");
  MachineSymbol user{"$builtin",        "mir.va_arg.p", MachineSymbol::Kind::Prototype,
                     Signature{{}, {}}, false,          {}};
  auto a = f->intern_symbol (user);
  REQUIRE (a);
  MachineSymbol runtime{"$builtin",
                        "mir.va_arg.p",
                        MachineSymbol::Kind::BuiltinPrototype,
                        Signature{{Type::i64 ()}, {Type::i64 (), Type::i64 ()}},
                        false,
                        {},
                        Builtin::VariadicScalar};
  auto b = f->intern_symbol (runtime);
  REQUIRE (b);
  CHECK (*a != *b);
  auto again = f->intern_symbol (runtime);
  REQUIRE (again);
  CHECK (*again == *b);
  auto revision = f->revision ();
  runtime.builtin.reset ();
  CHECK_FALSE (f->intern_symbol (runtime));
  CHECK (f->revision () == revision);
}
TEST_CASE ("AArch64 large incoming and outgoing argument offsets use checked address temporaries") {
  if (!arm ()) return;
  auto f = input (test::aarch64_large_argument_fixture (), 0);
  auto r = machinize (*f);
  INFO ((r ? "" : r.error ().message));
  REQUIRE (r);
  REQUIRE (f->verify ());
  CHECK (f->state ().has_block_arguments);
  bool materialized = false;
  for (auto &b : f->blocks ())
    for (auto &i : b.instructions ())
      if (i.opcode () == MachineOpcode (Opcode::Mov)) {
        auto reg = i.operands ()[0].get<target::PhysicalRegister> ();
        auto value = i.operands ()[1].get<std::int64_t> ();
        materialized |= reg && reg->encoding () == 10 && value && *value >= 32768;
      }
  CHECK (materialized);
}
