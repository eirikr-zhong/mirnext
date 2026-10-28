/* Licensed under the MIT License. */
#include "mirnext/IR/Construction.hpp"
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Transforms/Materialize.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
Operand integer (std::int64_t value) { return Operand (value); }
struct Example {
  Program context;
  Module *module;
  Function *function;
  BasicBlock *entry;
  Example () {
    auto m = mirnext::ir::detail::access (context).create_module ("example");
    REQUIRE ((m).has_value ());
    module = &(m).value ();
    auto f
      = mirnext::ir::detail::access (*(module)).create_function ("entry", {{Type::i64 ()}, {}});
    REQUIRE ((f).has_value ());
    function = &(f).value ();
    auto b = mirnext::ir::detail::access (*(function)).create_block ("entry");
    REQUIRE ((b).has_value ());
    entry = &(b).value ();
  }
};
struct CountCFG {
  using Result = std::size_t;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run (const Function &function) {
    ++runs;
    return function.instruction_count ();
  }
};
struct CountInstructions {
  using Result = std::size_t;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::Instructions;
  static mirnext::Result<Result> run (const Function &function) {
    ++runs;
    return function.instruction_count ();
  }
};
struct CountActiveScopes {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::ControlFlow;
  static mirnext::Result<Result> run (const Function &function) {
    std::size_t count = 0;
    for (auto &scope : function.stack_scopes ()) count += !scope.materialized ();
    return count;
  }
};
struct ReplaceRunning {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Function &, FunctionAnalysisManager &manager) {
    auto replaced = manager.set<ReplaceRunning> (42);
    if (!(replaced).has_value ()) return replaced.error ();
    return 42;
  }
};
struct OwnedAnalysis {
  using Result = std::unique_ptr<unsigned>;
  static inline unsigned runs = 0;
  static constexpr Dependency dependencies = Dependency::Instructions;
  static mirnext::Result<Result> run (const Function &) {
    ++runs;
    return std::make_unique<unsigned> (42);
  }
};
struct TakeRunning {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Function &, FunctionAnalysisManager &manager) {
    auto taken = manager.take_cached<TakeRunning> ();
    if (!(taken).has_value ()) return taken.error ();
    return 42;
  }
};
struct RewriteLiteral final : FunctionPass {
  Instruction *instruction;
  explicit RewriteLiteral (Instruction *instruction) : instruction (instruction) {}
  std::string_view name () const noexcept override { return "rewrite-literal"; }
  mirnext::Result<PassResult> run (MIRBuilder &edit, FunctionAnalysisManager &) override {
    auto &function = *edit.function ();

    auto rewritten = InstructionRewriter (function).replace_operand (*instruction, 1, integer (42));
    if (!(rewritten).has_value ()) return rewritten.error ();
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
}  // namespace
TEST_CASE ("scope materialization invalidates cached analyses and emits pass observations") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto scope = builder.begin_stack_scope ();
  REQUIRE ((scope).has_value ());
  REQUIRE ((builder.allocate_stack (integer (16))).has_value ());
  REQUIRE ((builder.ret (integer (42))).has_value ());
  REQUIRE ((builder.end_stack_scope ((scope).value ())).has_value ());
  FunctionAnalysisManager manager (*e.function);
  CountCFG::runs = 0;
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 1);
  FunctionPassManager passes;
  passes.add<MaterializeStackScopesPass> ();
  std::size_t observations = 0;
  auto result
    = passes.run (*e.function, manager, [&] (const PassEvent &event) -> mirnext::Result<void> {
        ++observations;
        CHECK (event.pass == "materialize-stack-scopes");
        if (event.phase == PassEvent::Phase::After) {
          CHECK (event.transformations == 2);
          CHECK_FALSE (event.function.needs_materialization ());
        }
        return {};
      });
  REQUIRE ((result).has_value ());
  CHECK (observations == 2);
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 2);
  REQUIRE ((passes.run (*e.function, manager)).has_value ());
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 2);
  Example unused;
  REQUIRE ((mirnext::ir::detail::access (*(unused.function)).create_stack_scope ()).has_value ());
  REQUIRE ((InstructionBuilder (*unused.entry).ret (integer (42))).has_value ());
  FunctionAnalysisManager unused_analyses (*unused.function);
  auto before = unused_analyses.get<CountActiveScopes> ();
  REQUIRE ((before).has_value ());
  CHECK ((before).value () == 1);
  auto materialized = materialize_stack_scopes (*unused.function);
  REQUIRE ((materialized).has_value ());
  CHECK (materialized.value ().saves == 0);
  auto after = unused_analyses.get<CountActiveScopes> ();
  REQUIRE ((after).has_value ());
  CHECK ((after).value () == 0);
}
TEST_CASE ("native CFG preserves duplicate switch edges fallthrough and unreachable layout") {
  Example e;
  auto yes = mirnext::ir::detail::access (*(e.function)).create_block ("yes");
  REQUIRE ((yes).has_value ());
  auto no = mirnext::ir::detail::access (*(e.function)).create_block ("no");
  REQUIRE ((no).has_value ());
  auto dead = mirnext::ir::detail::access (*(e.function)).create_block ("dead");
  REQUIRE ((dead).has_value ());
  auto yes_label = mirnext::ir::detail::access (*(e.function)).create_label ((yes).value (), "yes");
  REQUIRE ((yes_label).has_value ());
  auto no_label = mirnext::ir::detail::access (*(e.function)).create_label ((no).value (), "no");
  REQUIRE ((no_label).has_value ());
  InstructionBuilder entry (*e.entry);
  REQUIRE (
    (entry.emit (Opcode::Switch, {integer (0), Operand ((yes_label).value ()),
                                  Operand ((no_label).value ()), Operand ((yes_label).value ())}))
      .has_value ());
  InstructionBuilder yes_builder ((yes).value ());
  REQUIRE ((yes_builder.ret (integer (42))).has_value ());
  InstructionBuilder no_builder ((no).value ());
  REQUIRE ((no_builder.ret (integer (0))).has_value ());
  InstructionBuilder dead_builder ((dead).value ());
  REQUIRE ((dead_builder.ret (integer (13))).has_value ());
  auto graph = ControlFlowAnalysis::run (*e.function);
  REQUIRE ((graph).has_value ());
  CHECK (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size () == 4);
  CHECK (graph.value ().edges.size () == 3);
  CHECK (graph.value ().incoming[1].size () == 2);
  CHECK (graph.value ().outgoing[0].size () == 3);
  CHECK_FALSE (graph.value ().reachable[3]);
  CHECK (graph.value ().reverse_postorder.size () == 3);
  for (auto &edge : graph.value ().edges) CHECK (edge.kind == EdgeKind::SwitchCase);
}
TEST_CASE ("native CFG traverses deep graphs without recursive stack growth") {
  Example e;
  for (unsigned n = 0; n < 12000; ++n) {
    auto block = mirnext::ir::detail::access (*(e.function)).create_block ();
    REQUIRE ((block).has_value ());
  }
  auto &last = *std::prev (e.function->blocks ().end ());
  InstructionBuilder builder (last);
  REQUIRE ((builder.ret (integer (42))).has_value ());
  auto graph = ControlFlowAnalysis::run (*e.function);
  REQUIRE ((graph).has_value ());
  CHECK (graph.value ().reverse_postorder.size () == 12001);
  CHECK (graph.value ().edges.size () == 12000);
}
TEST_CASE ("native analysis cache reuses CFG on value edits but cannot preserve stale results") {
  Example e;
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "value");
  REQUIRE ((reg).has_value ());
  InstructionBuilder builder (*e.entry);
  auto instruction = builder.assign ((reg).value (), integer (40));
  REQUIRE ((instruction).has_value ());
  REQUIRE ((builder.ret (Operand ((reg).value ()))).has_value ());
  FunctionAnalysisManager manager (*e.function);
  CountCFG::runs = 0;
  CountInstructions::runs = 0;
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  REQUIRE ((manager.get<CountInstructions> ()).has_value ());
  for (unsigned n = 0; n < 100; ++n) REQUIRE ((manager.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 1);
  REQUIRE (
    (InstructionRewriter (*e.function).replace_operand ((instruction).value (), 1, integer (42)))
      .has_value ());
  manager.invalidate (PreservedAnalyses::all ());
  CHECK (manager.cached () == 1);
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  REQUIRE ((manager.get<CountInstructions> ()).has_value ());
  CHECK (CountCFG::runs == 1);
  CHECK (CountInstructions::runs == 2);
  manager.invalidate (PreservedAnalyses::none ());
  CHECK (manager.cached () == 0);
}
TEST_CASE ("updated analyses retain new revisions and cannot replace an active computation") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  auto assigned = builder.assign ((reg).value (), integer (1));
  REQUIRE ((assigned).has_value ());
  REQUIRE ((builder.ret (Operand ((reg).value ()))).has_value ());
  FunctionAnalysisManager manager (*e.function);
  CountInstructions::runs = 0;
  REQUIRE ((manager.set<CountInstructions> (17)).has_value ());
  CHECK ((manager.get<CountInstructions> ()).value () == 17);
  CHECK (CountInstructions::runs == 0);
  REQUIRE ((InstructionRewriter (*e.function).replace_operand ((assigned).value (), 1, integer (2)))
             .has_value ());
  CHECK ((manager.get<CountInstructions> ()).value () == 2);
  CHECK (CountInstructions::runs == 1);
  REQUIRE ((manager.set<CountInstructions> (19)).has_value ());
  manager.invalidate (PreservedAnalyses::all ());
  CHECK ((manager.get<CountInstructions> ()).value () == 19);
  auto replaced = manager.get<ReplaceRunning> ();
  REQUIRE_FALSE ((replaced).has_value ());
  CHECK (replaced.error ().message.find ("running analysis") != std::string::npos);
  CHECK (manager.cached () == 1);
}
TEST_CASE (
  "analysis ownership transfer moves results without recomputing or returning stale values") {
  Example e;
  InstructionBuilder builder (*e.entry);
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  auto assigned = builder.assign ((reg).value (), integer (1));
  REQUIRE ((assigned).has_value ());
  REQUIRE ((builder.ret (Operand ((reg).value ()))).has_value ());
  FunctionAnalysisManager manager (*e.function);
  OwnedAnalysis::runs = 0;
  auto missing = manager.take_cached<OwnedAnalysis> ();
  REQUIRE ((missing).has_value ());
  CHECK_FALSE ((missing).value ());
  CHECK (OwnedAnalysis::runs == 0);
  auto cached = manager.get<OwnedAnalysis> ();
  REQUIRE ((cached).has_value ());
  auto identity = cached.value ().get ();
  CHECK (OwnedAnalysis::runs == 1);
  auto owned = manager.take_cached<OwnedAnalysis> ();
  REQUIRE ((owned).has_value ());
  REQUIRE ((owned).value ());
  CHECK ((*(owned).value ()).get () == identity);
  CHECK (manager.cached () == 0);
  REQUIRE ((manager.set<OwnedAnalysis> (std::move (*(owned).value ()))).has_value ());
  auto restored = manager.get<OwnedAnalysis> ();
  REQUIRE ((restored).has_value ());
  CHECK (restored.value ().get () == identity);
  REQUIRE ((InstructionRewriter (*e.function).replace_operand ((assigned).value (), 1, integer (2)))
             .has_value ());
  auto stale = manager.take_cached<OwnedAnalysis> ();
  REQUIRE ((stale).has_value ());
  CHECK_FALSE ((stale).value ());
  CHECK (manager.cached () == 0);
  CHECK (OwnedAnalysis::runs == 1);
  auto running = manager.get<TakeRunning> ();
  REQUIRE_FALSE ((running).has_value ());
  CHECK (running.error ().message == "cannot take a running analysis");
  CHECK (manager.cached () == 0);
}
TEST_CASE ("native register use index distinguishes definitions and both address occurrences") {
  Example e;
  auto a = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer (), "a");
  REQUIRE ((a).has_value ());
  auto b = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer (), "b");
  REQUIRE ((b).has_value ());
  auto value = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 (), "value");
  REQUIRE ((value).has_value ());
  InstructionBuilder builder (*e.entry);
  auto assignment = builder.assign ((a).value (), integer (4096));
  REQUIRE ((assignment).has_value ());
  Memory memory = mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (), &(a).value (),
                                                                &(a).value (), 0, 1);
  auto load = builder.assign ((value).value (), Operand (memory));
  REQUIRE ((load).has_value ());
  REQUIRE ((builder.ret (Operand ((value).value ()))).has_value ());
  auto index = RegisterUseAnalysis::run (*e.function);
  REQUIRE ((index).has_value ());
  REQUIRE (index.value ().uses ((a).value ()).size () == 3);
  CHECK (index.value ().uses ((a).value ())[0].definition);
  CHECK (index.value ().uses ((a).value ())[1].part == RegisterOccurrence::Part::MemoryBase);
  REQUIRE (
    (InstructionRewriter (*e.function).replace_uses ((a).value (), (b).value ())).has_value ());
  CHECK (*mirnext::ir::detail::operand_access (assignment.value ().operands ()[0])
            .get<const Register *> ()
         == &(a).value ());
  auto address = mirnext::ir::detail::operand_access (load.value ().operands ()[1]).get<Memory> ();
  REQUIRE (address);
  CHECK (mirnext::ir::detail::NodeAccess::base (*(address)) == &(b).value ());
  CHECK (mirnext::ir::detail::NodeAccess::index (*(address)) == &(b).value ());
}
TEST_CASE (
  "native pass manager observes transformations invalidates analyses and contextualizes failures") {
  Example e;
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  InstructionBuilder builder (*e.entry);
  auto instruction = builder.assign ((reg).value (), integer (40));
  REQUIRE ((instruction).has_value ());
  REQUIRE ((builder.ret (Operand ((reg).value ()))).has_value ());
  FunctionAnalysisManager manager (*e.function);
  REQUIRE ((manager.get<CountCFG> ()).has_value ());
  REQUIRE ((manager.get<CountInstructions> ()).has_value ());
  FunctionPassManager passes;
  passes.add<RewriteLiteral> (&(instruction).value ());
  std::vector<PassEvent::Phase> observed;
  auto executed = passes.run (*e.function, manager, [&] (const PassEvent &event) -> Result<void> {
    observed.push_back (event.phase);
    CHECK (event.instructions_before == 2);
    if (event.phase == PassEvent::Phase::After) CHECK (event.transformations == 1);
    return {};
  });
  REQUIRE ((executed).has_value ());
  CHECK (observed.size () == 2);
  CHECK (manager.cached () == 1);
  auto failed = passes.run (*e.function, manager, [] (const PassEvent &) -> Result<void> {
    return Error{ErrorCode::InvalidArgument, "observer declined"};
  });
  REQUIRE_FALSE ((failed).has_value ());
  CHECK (failed.error ().message.find ("example/entry [rewrite-literal]") != std::string::npos);
  CHECK (failed.error ().message.find ("observer declined") != std::string::npos);
}
TEST_CASE ("native CFG uses only address taken labels and invalidates on label data insertion") {
  Example e;
  auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
  REQUIRE ((address).has_value ());
  auto target = mirnext::ir::detail::access (*(e.function)).create_block ("target");
  REQUIRE ((target).has_value ());
  auto untouched = mirnext::ir::detail::access (*(e.function)).create_block ("untouched");
  REQUIRE ((untouched).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  auto unused = mirnext::ir::detail::access (*(e.function)).create_label ((untouched).value ());
  REQUIRE ((unused).has_value ());
  InstructionBuilder builder (*e.entry);
  REQUIRE ((builder.emit (Opcode::JmpIndirect, {Operand ((address).value ())})).has_value ());
  InstructionBuilder target_builder ((target).value ());
  REQUIRE ((target_builder.ret (integer (42))).has_value ());
  InstructionBuilder unused_builder ((untouched).value ());
  REQUIRE ((unused_builder.ret (integer (0))).has_value ());
  FunctionAnalysisManager analyses (*e.function);
  auto first = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((first).has_value ());
  CHECK (first.value ().edges.empty ());
  CHECK_FALSE (first.value ().reachable[1]);
  CHECK_FALSE (first.value ().reachable[2]);
  REQUIRE ((mirnext::ir::detail::access (*(e.module))
              .create_data ("target_address", mirnext::ir::detail::NodeAccess::make_LabelReference (
                                                &(label).value ())))
             .has_value ());
  auto second = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((second).has_value ());
  REQUIRE (second.value ().edges.size () == 1);
  CHECK (second.value ().edges[0].destination == 1);
  CHECK (second.value ().reachable[1]);
  CHECK_FALSE (second.value ().reachable[2]);
  CHECK (second.value ().entry_roots.size () == 2);
}
TEST_CASE ("native CFG invalidates when replacing a label address with an ordinary move") {
  Example e;
  auto address = mirnext::ir::detail::access (*(e.function)).create_register (Type::pointer ());
  REQUIRE ((address).has_value ());
  auto target = mirnext::ir::detail::access (*(e.function)).create_block ();
  REQUIRE ((target).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  InstructionBuilder builder (*e.entry);
  auto load_address
    = builder.emit (Opcode::LAddr, {Operand ((address).value ()), Operand ((label).value ())});
  REQUIRE ((load_address).has_value ());
  REQUIRE ((builder.emit (Opcode::JmpIndirect, {Operand ((address).value ())})).has_value ());
  InstructionBuilder ending ((target).value ());
  REQUIRE ((ending.ret (integer (42))).has_value ());
  FunctionAnalysisManager analyses (*e.function);
  auto first = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((first).has_value ());
  CHECK (first.value ().edges.size () == 1);
  CHECK (first.value ().reachable[1]);
  const Operand replacement[]{Operand ((address).value ()), integer (0)};
  REQUIRE (
    (InstructionRewriter (*e.function).replace ((load_address).value (), Opcode::Mov, replacement))
      .has_value ());
  auto second = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((second).has_value ());
  CHECK (second.value ().edges.empty ());
  CHECK_FALSE (second.value ().reachable[1]);
}
TEST_CASE ("native value insertion erasure and movement preserve cached CFG") {
  Example e;
  auto exit = mirnext::ir::detail::access (*(e.function)).create_block ();
  REQUIRE ((exit).has_value ());
  auto reg = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((reg).has_value ());
  InstructionBuilder ending ((exit).value ());
  auto ret = ending.ret (integer (42));
  REQUIRE ((ret).has_value ());
  FunctionAnalysisManager analyses (*e.function);
  REQUIRE ((analyses.get<ControlFlowAnalysis> ()).has_value ());
  auto revision = e.function->revisions ().control_flow;
  InstructionBuilder builder (*e.entry);
  auto value = builder.assign ((reg).value (), integer (0));
  REQUIRE ((value).has_value ());
  InstructionRewriter rewriter (*e.function);
  for (unsigned n = 0; n < 16; ++n) {
    REQUIRE (
      (rewriter.move_before ((value).value (), (exit).value (), &(ret).value ())).has_value ());
    REQUIRE ((rewriter.replace_operand ((value).value (), 1, integer (n))).has_value ());
    REQUIRE ((rewriter.move_before ((value).value (), *e.entry)).has_value ());
  }
  REQUIRE ((rewriter.erase ((value).value ())).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  CHECK (e.function->revisions ().control_flow == revision);
  analyses.invalidate (PreservedAnalyses::all ());
  CHECK (analyses.cached () == 1);
}
TEST_CASE ("native structural edits invalidate CFG and use analyses even when preserved") {
  Example e;
  auto value = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((value).has_value ());
  InstructionBuilder builder (*e.entry);
  auto assignment = builder.assign ((value).value (), integer (42));
  REQUIRE ((assignment).has_value ());
  auto ret = builder.ret (Operand ((value).value ()));
  REQUIRE ((ret).has_value ());
  FunctionAnalysisManager analyses (*e.function);
  CountCFG::runs = 0;
  CountInstructions::runs = 0;
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  REQUIRE ((analyses.get<CountInstructions> ()).has_value ());
  REQUIRE ((analyses.get<RegisterUseAnalysis> ()).has_value ());
  REQUIRE ((analyses.get<ControlFlowAnalysis> ()).has_value ());
  InstructionRewriter rewriter (*e.function);
  auto split = rewriter.split_before ((ret).value ());
  REQUIRE ((split).has_value ());
  analyses.invalidate (PreservedAnalyses::all ());
  CHECK (analyses.cached () == 0);
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 2);
  auto graph = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size () == 2);
  REQUIRE (graph.value ().edges.size () == 1);
  CHECK (graph.value ().edges[0].kind == EdgeKind::Fallthrough);
  CHECK (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ())[1]
         == &(split).value ());
  auto uses = analyses.get<RegisterUseAnalysis> ();
  REQUIRE ((uses).has_value ());
  REQUIRE (uses.value ().uses ((value).value ()).size () == 2);
  CHECK (&uses.value ().uses ((value).value ())[1].instruction ().block () == &(split).value ());
  REQUIRE ((rewriter.merge_fallthrough (*e.entry)).has_value ());
  analyses.invalidate (PreservedAnalyses::all ());
  CHECK (analyses.cached () == 0);
  graph = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((graph).has_value ());
  CHECK (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size () == 1);
  CHECK (graph.value ().edges.empty ());
  uses = analyses.get<RegisterUseAnalysis> ();
  REQUIRE ((uses).has_value ());
  CHECK (&uses.value ().uses ((value).value ())[1].instruction ().block () == e.entry);
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 3);
  REQUIRE ((e.context.verify ()).has_value ());
}
TEST_CASE ("native value insertion keeps CFG cache while label cleanup invalidates it once") {
  Example e;
  auto value = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((value).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label (*e.entry, "unused");
  REQUIRE ((label).has_value ());
  InstructionBuilder builder (*e.entry);
  auto ret = builder.ret (integer (42));
  REQUIRE ((ret).has_value ());
  FunctionAnalysisManager analyses (*e.function);
  CountCFG::runs = 0;
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  auto revision = e.function->revisions ();
  InstructionRewriter rewriter (*e.function);
  REQUIRE ((rewriter.insert_before ((ret).value (), Opcode::Mov,
                                    {Operand ((value).value ()), integer (0)}))
             .has_value ());
  CHECK (e.function->revisions ().control_flow == revision.control_flow);
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 1);
  auto erased = rewriter.erase_unused_labels ();
  REQUIRE ((erased).has_value ());
  CHECK ((erased).value () == 1);
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 2);
  erased = rewriter.erase_unused_labels ();
  REQUIRE ((erased).has_value ());
  CHECK ((erased).value () == 0);
  REQUIRE ((analyses.get<CountCFG> ()).has_value ());
  CHECK (CountCFG::runs == 2);
}
TEST_CASE ("native split and merge preserve overflow flags across unlabeled blocks") {
  Example e;
  auto value = mirnext::ir::detail::access (*(e.function)).create_register (Type::i64 ());
  REQUIRE ((value).has_value ());
  auto exit = mirnext::ir::detail::access (*(e.function)).create_block ();
  REQUIRE ((exit).has_value ());
  auto label = mirnext::ir::detail::access (*(e.function)).create_label ((exit).value ());
  REQUIRE ((label).has_value ());
  InstructionBuilder builder (*e.entry);
  REQUIRE (
    (builder.emit (Opcode::Addo, {Operand ((value).value ()), integer (INT64_MAX), integer (1)}))
      .has_value ());
  auto move = builder.assign ((value).value (), Operand ((value).value ()));
  REQUIRE ((move).has_value ());
  REQUIRE ((builder.emit (Opcode::Bo, {Operand ((label).value ())})).has_value ());
  REQUIRE ((InstructionBuilder ((exit).value ()).ret (integer (42))).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  InstructionRewriter rewriter (*e.function);
  auto split = rewriter.split_before ((move).value ());
  REQUIRE ((split).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
  auto graph = ControlFlowAnalysis::run (*e.function);
  REQUIRE ((graph).has_value ());
  CHECK (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size () == 3);
  CHECK (graph.value ().edges.size () == 3);
  REQUIRE ((rewriter.merge_fallthrough (*e.entry)).has_value ());
  REQUIRE ((e.context.verify ()).has_value ());
}

TEST_CASE ("public analysis lookups use typed references and absent values") {
  ProgramBuilder builder;
  auto f = builder.module ("lookup").function<std::int64_t (std::int64_t)> ("entry");
  f.ret (f.arg (0) + 1);
  auto program = builder.finish ();
  REQUIRE ((program).has_value ());
  auto &function = program.value ().modules ().front ().functions ().front ();
  auto &block = function.blocks ().front ();
  auto &argument = function.arguments ().front ();
  FunctionAnalysisManager analyses (function);
  auto cfg = analyses.get<ControlFlowAnalysis> ();
  auto uses = analyses.get<RegisterUseAnalysis> ();
  auto order = analyses.get<ConstructionOrderAnalysis> ();
  REQUIRE ((cfg).has_value ());
  REQUIRE ((uses).has_value ());
  REQUIRE ((order).has_value ());
  CHECK (cfg.value ().index (block) == 0);
  CHECK_FALSE (uses.value ().uses (argument).empty ());
  CHECK ((order.value ().block (block)).has_value ());
  CHECK (order.value ().instruction (block.instructions ().front ()));
  for (auto &use : uses.value ().uses (argument))
    CHECK (&use.instruction ().function () == &function);
  ProgramBuilder other;
  auto foreign = other.module ("other").function<std::int64_t (std::int64_t)> ("entry");
  foreign.ret (foreign.arg (0));
  auto separate = other.finish ();
  REQUIRE ((separate).has_value ());
  auto &other_function = separate.value ().modules ().front ().functions ().front ();
  CHECK_FALSE (cfg.value ().index (other_function.blocks ().front ()));
  CHECK_FALSE ((order.value ().block (other_function.blocks ().front ())).has_value ());
  CHECK (uses.value ().uses (other_function.arguments ().front ()).empty ());
}
