/* Licensed under the MIT License. Native module analysis and pass lifecycles. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Transforms/PassManager.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/IR/SSA.hpp"
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
Function &function (Module &module, std::string_view name = "entry") {
  auto f = module.create_function (name, {{Type::i64 ()}, {}});
  REQUIRE (f);
  auto b = (*f)->create_block ();
  REQUIRE (b);
  REQUIRE (IRBuilder (**b).ret (Operand (1)));
  return **f;
}
template <Dependency Dependencies>
struct RevisionAnalysis {
  using Result = Revisions;
  static constexpr Dependency dependencies = Dependencies;
  static inline unsigned runs = 0;
  static mirnext::Result<Result> run (const Module &module) {
    ++runs;
    return module.revisions ();
  }
};
using Bodies = RevisionAnalysis<Dependency::Instructions>;
using Flow = RevisionAnalysis<Dependency::ControlFlow>;
using Registers = RevisionAnalysis<Dependency::Registers>;
using Symbols = RevisionAnalysis<Dependency::Symbols>;
using Data = RevisionAnalysis<Dependency::ModuleData>;
struct ModuleBlocks {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::ControlFlow | Dependency::ModuleData;
  static mirnext::Result<Result> run (const Module &module, ModuleAnalysisManager &manager) {
    std::size_t blocks = 0;
    for (auto &f : module.functions ()) {
      auto child = manager.function_analyses (f);
      if (!child) return child.error ();
      auto graph = (*child)->get<ControlFlowAnalysis> ();
      if (!graph) return graph.error ();
      blocks += (*graph)->blocks.size ();
    }
    return blocks;
  }
};
struct FunctionSymbols {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::Symbols;
  static mirnext::Result<Result> run (const Function &f) {
    return std::ranges::distance (f.module ().symbols ());
  }
};
struct FunctionData {
  using Result = std::uint64_t;
  static constexpr Dependency dependencies = Dependency::ModuleData;
  static mirnext::Result<Result> run (const Function &f) { return f.module ().data_revision (); }
};
struct Dependent {
  using Result = std::uint64_t;
  static constexpr Dependency dependencies = Dependency::Symbols;
  static mirnext::Result<Result> run (const Module &, ModuleAnalysisManager &manager) {
    auto source = manager.get<Symbols> ();
    if (!source) return source.error ();
    return (*source)->symbols;
  }
};
struct Recursive {
  using Result = unsigned;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Module &, ModuleAnalysisManager &manager) {
    auto value = manager.get<Recursive> ();
    if (!value) return value.error ();
    return **value;
  }
};
struct Owned {
  using Result = std::unique_ptr<unsigned>;
  static constexpr Dependency dependencies = Dependency::Symbols;
  static mirnext::Result<Result> run (const Module &) { return std::make_unique<unsigned> (42); }
};
struct AddExport final : ModulePass {
  std::string_view name () const noexcept override { return "add-export"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    auto before = context.analyses.get<Symbols> ();
    if (!before) return before.error ();
    auto symbol = &module.functions ().begin ()->symbol ();
    auto result = module.export_symbol (*symbol);
    if (!result) return result.error ();
    auto preserved = PreservedAnalyses::all ();
    // A preserved entry still expires when one of its dependencies changes.
    return PassResult{1, std::move (preserved)};
  }
};
struct PreserveBodies final : ModulePass {
  std::string_view name () const noexcept override { return "preserve-bodies"; }
  mirnext::Result<PassResult> run (Module &, ModulePassContext &) override {
    auto preserved = PreservedAnalyses::none ();
    preserved.preserve<Bodies> ();
    return PassResult{0, std::move (preserved)};
  }
};
struct RewriteReturns final : FunctionPass {
  std::vector<std::string> &events;
  unsigned &initialized, &finalized;
  RewriteReturns (std::vector<std::string> &events, unsigned &initialized, unsigned &finalized)
    : events (events), initialized (initialized), finalized (finalized) {}
  std::string_view name () const noexcept override { return "rewrite-returns"; }
  mirnext::Result<void> initialize (Context &) override {
    ++initialized;
    return {};
  }
  void finalize () noexcept override { ++finalized; }
  mirnext::Result<PassResult> run (Function &f, FunctionAnalysisManager &analyses) override {
    events.push_back ("function:" + std::string (f.module ().name ()) + "/"
                      + std::string (f.symbol ().name ()));
    auto graph = analyses.get<ControlFlowAnalysis> ();
    if (!graph) return graph.error ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    auto result = IRRewriter (f).replace_operand (ret, 0, Operand (42));
    if (!result) return result.error ();
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
struct AddFunction final : ModulePass {
  std::vector<std::string> &events;
  explicit AddFunction (std::vector<std::string> &events) : events (events) {}
  std::string_view name () const noexcept override { return "add-function"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &) override {
    events.push_back ("module:" + std::string (module.name ()));
    function (module, "created");
    return PassResult{1, PreservedAnalyses::none ()};
  }
};
struct Failing final : ModulePass {
  unsigned &finalized;
  bool initialize_failure;
  explicit Failing (unsigned &finalized, bool initialize_failure = false)
    : finalized (finalized), initialize_failure (initialize_failure) {}
  std::string_view name () const noexcept override { return "fail-module"; }
  mirnext::Result<void> initialize (Context &) override {
    if (initialize_failure) return Error{ErrorCode::InvalidArgument, "initialization failed"};
    return {};
  }
  void finalize () noexcept override { ++finalized; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    auto source = context.analyses.get<Bodies> ();
    if (!source) return source.error ();
    auto &f = *module.functions ().begin ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    auto changed = IRRewriter (f).replace_operand (ret, 0, Operand (7));
    if (!changed) return changed.error ();
    return Error{ErrorCode::InvalidArgument, "transformation failed"};
  }
};
struct CrossModule final : ModulePass {
  std::unordered_map<const Module *, ModuleAnalysisManager *> &managers;
  bool mutate;
  CrossModule (std::unordered_map<const Module *, ModuleAnalysisManager *> &managers, bool mutate)
    : managers (managers), mutate (mutate) {}
  std::string_view name () const noexcept override { return "cross-module"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    if (!mutate) {
      managers.emplace (&module, &context.analyses);
      auto symbols = context.analyses.get<Symbols> ();
      if (!symbols) return symbols.error ();
      auto bodies = context.analyses.get<Bodies> ();
      if (!bodies) return bodies.error ();
    } else if (module.name () == "a") {
      auto other = const_cast<Module *> (module.context ().find_module ("b"));
      REQUIRE (other);
      auto result = other->export_symbol (other->functions ().begin ()->symbol ());
      if (!result) return result.error ();
      CHECK_FALSE (managers.at (other)->get_cached<Symbols> ());
      CHECK (managers.at (other)->get_cached<Bodies> ());
    } else {
      CHECK_FALSE (context.analyses.get_cached<Symbols> ());
      CHECK (context.analyses.get_cached<Bodies> ());
    }
    return PassResult{mutate ? 1u : 0u, PreservedAnalyses::all ()};
  }
};
struct AppendHelper final : FunctionPass {
  std::string_view name () const noexcept override { return "append-helper"; }
  mirnext::Result<PassResult> run (Function &f, FunctionAnalysisManager &) override {
    function (f.module (), "helper");
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
struct BreakFunction final : ModulePass {
  std::string_view name () const noexcept override { return "break-function"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &) override {
    auto &f = *module.functions ().begin ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    auto result = IRRewriter (f).erase (ret);
    if (!result) return result.error ();
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
}  // namespace
TEST_CASE ("module analyses track bodies registers symbols and data independently") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  ModuleAnalysisManager analyses (**m);
  Bodies::runs = Flow::runs = Registers::runs = Symbols::runs = Data::runs = 0;
  REQUIRE (analyses.get<Bodies> ());
  REQUIRE (analyses.get<Flow> ());
  REQUIRE (analyses.get<Registers> ());
  REQUIRE (analyses.get<Symbols> ());
  REQUIRE (analyses.get<Data> ());
  REQUIRE (analyses.get<Dependent> ());
  auto &ret = *f.blocks ().begin ()->instructions ().begin ();
  REQUIRE (IRRewriter (f).replace_operand (ret, 0, Operand (42)));
  CHECK_FALSE (analyses.get_cached<Bodies> ());
  CHECK (analyses.get_cached<Flow> ());
  CHECK (analyses.get_cached<Registers> ());
  CHECK (analyses.get_cached<Symbols> ());
  CHECK (analyses.get_cached<Data> ());
  REQUIRE (analyses.get<Bodies> ());
  CHECK (Bodies::runs == 2);
  CHECK (Flow::runs == 1);
  auto bound = f.create_register (Type::i64 (), "bound", "r5");
  REQUIRE (bound);
  CHECK_FALSE (analyses.get_cached<Registers> ());
  REQUIRE (analyses.get<Registers> ());
  auto alias = f.create_register (Type::i64 (), "alias", "r5");
  REQUIRE (alias);
  CHECK (*bound == *alias);
  CHECK_FALSE (analyses.get_cached<Registers> ());
  REQUIRE (analyses.get<Registers> ());
  CHECK (Registers::runs == 3);
  REQUIRE ((*m)->export_symbol (f.symbol ()));
  CHECK_FALSE (analyses.get_cached<Symbols> ());
  CHECK_FALSE (analyses.get_cached<Dependent> ());
  CHECK (analyses.get_cached<Bodies> ());
  CHECK (analyses.get_cached<Data> ());
  REQUIRE (analyses.get<Symbols> ());
  REQUIRE ((*m)->rename_symbol (f.symbol (), "renamed"));
  CHECK_FALSE (analyses.get_cached<Symbols> ());
  CHECK (analyses.get_cached<Data> ());
  CHECK (analyses.get_cached<Flow> ());
  REQUIRE (analyses.get<Symbols> ());
  REQUIRE ((*m)->create_data ("bytes", ZeroData{8}));
  CHECK_FALSE (analyses.get_cached<Symbols> ());
  CHECK_FALSE (analyses.get_cached<Data> ());
  CHECK (analyses.get_cached<Bodies> ());
  REQUIRE (analyses.get<Symbols> ());
  REQUIRE (analyses.get<Data> ());
  auto declaration = (*m)->declare_function ("later", {{Type::i64 ()}, {}});
  REQUIRE (declaration);
  REQUIRE (analyses.get<Symbols> ());
  auto definition = (*m)->define_function (**declaration);
  REQUIRE (definition);
  CHECK_FALSE (analyses.get_cached<Symbols> ());
  CHECK_FALSE (analyses.get_cached<Bodies> ());
  CHECK_FALSE (analyses.get_cached<Flow> ());
  auto b = (*definition)->create_block ();
  REQUIRE (b);
  REQUIRE (IRBuilder (**b).ret (Operand (0)));
  REQUIRE (context.verify ());
}
TEST_CASE ("module analyses own results detect cycles and reject foreign functions") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  ModuleAnalysisManager analyses (**m);
  auto value = analyses.get<Owned> ();
  REQUIRE (value);
  CHECK (***value == 42);
  auto taken = analyses.take_cached<Owned> ();
  REQUIRE (taken);
  REQUIRE (*taken);
  CHECK (***taken == 42);
  CHECK_FALSE (analyses.get_cached<Owned> ());
  REQUIRE (analyses.set<Owned> (std::make_unique<unsigned> (7)));
  CHECK (***analyses.get<Owned> () == 7);
  auto recursive = analyses.get<Recursive> ();
  CHECK_FALSE (recursive);
  CHECK (recursive.error ().message.find ("module: analysis dependency cycle")
         != std::string::npos);
  REQUIRE (analyses.set<Recursive> (3));
  REQUIRE (analyses.get<Recursive> ());
  CHECK (**analyses.get<Recursive> () == 3);
  auto child = analyses.function_analyses (f);
  REQUIRE (child);
  REQUIRE ((*child)->get<ControlFlowAnalysis> ());
  REQUIRE (analyses.get<ModuleBlocks> ());
  CHECK (**analyses.get<ModuleBlocks> () == 1);
  analyses.clear ();
  CHECK (analyses.cached () == 0);
  CHECK ((*child)->cached () == 0);
  auto again = analyses.function_analyses (f);
  REQUIRE (again);
  CHECK (*child == *again);
  auto other = context.create_module ("other");
  REQUIRE (other);
  auto &foreign = function (**other);
  CHECK_FALSE (analyses.function_analyses (foreign));
}
TEST_CASE ("module pass pipeline preserves global phase order and observes nested functions") {
  Context context;
  auto a = context.create_module ("a"), b = context.create_module ("b");
  REQUIRE (a);
  REQUIRE (b);
  function (**a);
  function (**b);
  std::vector<std::string> events;
  unsigned initialized = 0, finalized = 0;
  ModulePassManager passes;
  passes.add<AddFunction> (events);
  passes.add_function<RewriteReturns> (events, initialized, finalized);
  std::size_t module_events = 0, function_events = 0;
  auto result = passes.run (
    context,
    [&] (const ModulePassEvent &event) -> mirnext::Result<void> {
      ++module_events;
      if (event.phase == PassEvent::Phase::After) {
        CHECK (event.instructions_after == 2);
        CHECK (event.transformations == (event.pass == "add-function" ? 1 : 2));
      }
      return {};
    },
    [&] (const PassEvent &event) -> mirnext::Result<void> {
      ++function_events;
      CHECK (event.pass == "rewrite-returns");
      if (event.phase == PassEvent::Phase::After) CHECK (event.transformations == 1);
      return {};
    });
  REQUIRE (result);
  CHECK (initialized == 1);
  CHECK (finalized == 1);
  CHECK (module_events == 8);
  CHECK (function_events == 8);
  CHECK (events
         == std::vector<std::string>{"module:a", "module:b", "function:a/entry",
                                     "function:a/created", "function:b/entry",
                                     "function:b/created"});
  for (auto &module : context.modules ())
    for (auto &f : module.functions ())
      CHECK (*f.blocks ().begin ()->instructions ().begin ()->operands ()[0].get<std::int64_t> ()
             == 42);
  REQUIRE (context.verify ());
}
TEST_CASE ("module preservation retains current analyses and expires changed dependencies") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  ModuleAnalysisManager analyses (**m);
  REQUIRE (analyses.get<Bodies> ());
  REQUIRE (analyses.get<Symbols> ());
  auto child = analyses.function_analyses (f);
  REQUIRE (child);
  REQUIRE ((*child)->get<ControlFlowAnalysis> ());
  ModulePassManager passes;
  passes.add<AddExport> ();
  REQUIRE (passes.run (**m, analyses));
  CHECK (analyses.get_cached<Bodies> ());
  CHECK_FALSE (analyses.get_cached<Symbols> ());
  CHECK ((*child)->get_cached<ControlFlowAnalysis> ());
  std::vector<std::string> events;
  unsigned initialized = 0, finalized = 0;
  ModulePassManager rewrite;
  rewrite.add_function<RewriteReturns> (events, initialized, finalized);
  REQUIRE (rewrite.run (**m, analyses));
  CHECK_FALSE (analyses.get_cached<Bodies> ());
  CHECK ((*child)->get_cached<ControlFlowAnalysis> ());
  REQUIRE (analyses.get<Bodies> ());
  REQUIRE (analyses.get<Flow> ());
  ModulePassManager preserve;
  preserve.add<PreserveBodies> ();
  REQUIRE (preserve.run (**m, analyses));
  CHECK (analyses.get_cached<Bodies> ());
  CHECK_FALSE (analyses.get_cached<Flow> ());
  CHECK_FALSE ((*child)->get_cached<ControlFlowAnalysis> ());
  auto other = context.create_module ("other");
  REQUIRE (other);
  CHECK_FALSE (passes.run (**other, analyses));
}
TEST_CASE ("module failures finalize once clear caches and carry stage diagnostics") {
  for (bool initialize_failure : {false, true}) {
    Context context;
    auto m = context.create_module ("module");
    REQUIRE (m);
    auto &f = function (**m);
    ModuleAnalysisManager analyses (**m);
    REQUIRE (analyses.get<Bodies> ());
    auto child = analyses.function_analyses (f);
    REQUIRE (child);
    REQUIRE ((*child)->get<ControlFlowAnalysis> ());
    unsigned finalized = 0;
    ModulePassManager passes;
    passes.add<Failing> (finalized, initialize_failure);
    auto result = passes.run (**m, analyses);
    CHECK_FALSE (result);
    CHECK (finalized == 1);
    CHECK (analyses.cached () == 0);
    CHECK ((*child)->cached () == 0);
    CHECK (result.error ().message.find ("[fail-module]") != std::string::npos);
    CHECK (result.error ().message.find (initialize_failure
                                           ? "initialization failed"
                                           : "module [fail-module]: transformation failed")
           != std::string::npos);
    REQUIRE (context.verify ());
  }
  for (auto phase : {PassEvent::Phase::Before, PassEvent::Phase::After}) {
    Context context;
    auto m = context.create_module ("observe");
    REQUIRE (m);
    function (**m);
    ModuleAnalysisManager analyses (**m);
    REQUIRE (analyses.get<Bodies> ());
    ModulePassManager passes;
    passes.add<AddExport> ();
    auto result
      = passes.run (**m, analyses, [&] (const ModulePassEvent &event) -> mirnext::Result<void> {
          if (event.phase == phase) return Error{ErrorCode::InvalidArgument, "observer stopped"};
          return {};
        });
    CHECK_FALSE (result);
    CHECK (analyses.cached () == 0);
    CHECK (result.error ().message.find ("observe [add-export]: observer stopped")
           != std::string::npos);
    std::vector<std::string> events;
    unsigned initialized = 0, finalized = 0;
    ModulePassManager nested;
    nested.add_function<RewriteReturns> (events, initialized, finalized);
    result = nested.run (**m, analyses, {}, [&] (const PassEvent &event) -> mirnext::Result<void> {
      if (event.phase == phase)
        return Error{ErrorCode::InvalidArgument, "function observer stopped"};
      return {};
    });
    CHECK_FALSE (result);
    CHECK (initialized == 1);
    CHECK (finalized == 1);
    CHECK (analyses.cached () == 0);
    CHECK (
      result.error ().message.find ("observe/entry [rewrite-returns]: function observer stopped")
      != std::string::npos);
  }
  Context empty;
  ModulePassManager invalid;
  invalid.add<FunctionPassAdaptor> (std::unique_ptr<FunctionPass>{});
  auto rejected = invalid.run (empty);
  CHECK_FALSE (rejected);
  CHECK (rejected.error ().message.find ("requires a pass") != std::string::npos);
}
TEST_CASE ("cross-module mutations invalidate the changed owner immediately") {
  Context context;
  auto a = context.create_module ("a"), b = context.create_module ("b");
  REQUIRE (a);
  REQUIRE (b);
  function (**a);
  function (**b);
  std::unordered_map<const Module *, ModuleAnalysisManager *> managers;
  ModulePassManager passes;
  passes.add<CrossModule> (managers, false);
  passes.add<CrossModule> (managers, true);
  REQUIRE (passes.run (context));
  REQUIRE (context.verify ());
}
TEST_CASE ("adapted function traversal accepts helpers without revisiting the current pass") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  function (**m);
  std::vector<std::string> events;
  unsigned initialized = 0, finalized = 0;
  ModulePassManager passes;
  passes.add_function<AppendHelper> ();
  passes.add_function<RewriteReturns> (events, initialized, finalized);
  ModuleAnalysisManager analyses (**m);
  REQUIRE (passes.run (**m, analyses));
  CHECK (events.size () == 2);
  CHECK (events[1] == "function:module/helper");
  REQUIRE (context.verify ());
}
TEST_CASE ("module verification rejects broken results before after-stage observation") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  function (**m);
  ModuleAnalysisManager analyses (**m);
  REQUIRE (analyses.get<Bodies> ());
  ModulePassManager passes;
  passes.add<BreakFunction> ();
  unsigned after = 0;
  auto result
    = passes.run (**m, analyses, [&] (const ModulePassEvent &event) -> mirnext::Result<void> {
        after += event.phase == PassEvent::Phase::After;
        return {};
      });
  CHECK_FALSE (result);
  CHECK (after == 0);
  CHECK (analyses.cached () == 0);
  CHECK (result.error ().message.find ("module [break-function]") != std::string::npos);
}
TEST_CASE ("symbol-only mutations invalidate symbol analyses while retaining SSA def-use") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  ModuleAnalysisManager analyses (**m);
  auto child = analyses.function_analyses (f);
  REQUIRE (child);
  FunctionPassManager passes;
  passes.add<BuildSSAPass> ();
  REQUIRE (passes.run (f, **child));
  auto form = f.ssa_form ();
  REQUIRE (form);
  REQUIRE (form->current ());
  auto before = f.revisions ();
  REQUIRE ((*child)->get<FunctionSymbols> ());
  REQUIRE ((*m)->export_symbol (f.symbol ()));
  REQUIRE ((*m)->prototype ("unrelated", {}));
  REQUIRE ((*m)->rename_symbol (f.symbol (), "renamed"));
  CHECK (f.revisions () == before);
  CHECK_FALSE ((*child)->get_cached<FunctionSymbols> ());
  CHECK (form->current ());
  REQUIRE (form->verify ());
  REQUIRE (context.verify ());
}
TEST_CASE (
  "ordinary data changes retain function topology and liveness while expiring data analyses") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m), &sibling = function (**m, "sibling");
  ModuleAnalysisManager module (**m);
  auto child = module.function_analyses (f), other = module.function_analyses (sibling);
  REQUIRE (child);
  REQUIRE (other);
  auto cfg = (*child)->get<ControlFlowAnalysis> ();
  REQUIRE (cfg);
  auto graph = (*child)->get<FunctionGraphAnalysis> ();
  REQUIRE (graph);
  auto dominators = (*child)->get<DominatorAnalysis> ();
  REQUIRE (dominators);
  auto loops = (*child)->get<LoopAnalysis> ();
  REQUIRE (loops);
  auto live = (*child)->get<LivenessAnalysis> ();
  REQUIRE (live);
  auto uses = (*child)->get<RegisterUseAnalysis> ();
  REQUIRE (uses);
  auto other_cfg = (*other)->get<ControlFlowAnalysis> ();
  REQUIRE (other_cfg);
  auto bodies = module.get<Bodies> ();
  REQUIRE (bodies);
  auto flow = module.get<Flow> ();
  REQUIRE (flow);
  REQUIRE (module.get<Data> ());
  REQUIRE ((*child)->get<FunctionData> ());
  REQUIRE ((*other)->get<FunctionData> ());
  REQUIRE ((*child)->get<FunctionSymbols> ());
  auto before = f.revisions ();
  DataInitializer initial = ZeroData{8};
  SUBCASE ("BSS") {}
  SUBCASE ("initialized scalar") { initial = InitializedData{Type::i64 (), {Operand (42)}}; }
  SUBCASE ("function reference") { initial = SymbolReference{&f.symbol (), 0}; }
  SUBCASE ("expression") { initial = ExpressionData{&sibling}; }
  REQUIRE ((*m)->create_data ("new_data", std::move (initial)));
  CHECK (f.revisions ().instructions == before.instructions);
  CHECK (f.revisions ().control_flow == before.control_flow);
  CHECK (f.revisions ().registers == before.registers);
  CHECK (f.revisions ().module_data > before.module_data);
  CHECK ((*child)->get_cached<ControlFlowAnalysis> () == *cfg);
  CHECK ((*other)->get_cached<ControlFlowAnalysis> () == *other_cfg);
  CHECK ((*child)->get_cached<FunctionGraphAnalysis> () == *graph);
  CHECK ((*child)->get_cached<DominatorAnalysis> () == *dominators);
  CHECK ((*child)->get_cached<LoopAnalysis> () == *loops);
  CHECK ((*child)->get_cached<LivenessAnalysis> () == *live);
  CHECK ((*child)->get_cached<RegisterUseAnalysis> () == *uses);
  CHECK (module.get_cached<Bodies> () == *bodies);
  CHECK (module.get_cached<Flow> () == *flow);
  CHECK_FALSE (module.get_cached<Data> ());
  CHECK_FALSE ((*child)->get_cached<FunctionData> ());
  CHECK_FALSE ((*other)->get_cached<FunctionData> ());
  CHECK_FALSE ((*child)->get_cached<FunctionSymbols> ());
  REQUIRE (context.verify ());
}
TEST_CASE ("ordinary data changes retain SSA def-use and its checked editing phase") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto created = (*m)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE (created);
  auto &f = **created;
  auto entry = f.create_block (), no = f.create_block (), yes = f.create_block (),
       merge = f.create_block ();
  REQUIRE (entry);
  REQUIRE (no);
  REQUIRE (yes);
  REQUIRE (merge);
  auto selected = f.create_label (**yes), done = f.create_label (**merge);
  REQUIRE (selected);
  REQUIRE (done);
  auto value = f.create_register (Type::i64 ());
  REQUIRE (value);
  REQUIRE (IRBuilder (**entry).branch_if (Operand (*f.arguments ()[0]), **selected));
  IRBuilder cold (**no);
  REQUIRE (cold.emit (Opcode::Mov, {Operand (**value), Operand (1)}));
  REQUIRE (cold.jump (**done));
  REQUIRE (IRBuilder (**yes).emit (Opcode::Mov, {Operand (**value), Operand (42)}));
  REQUIRE (IRBuilder (**merge).ret (Operand (**value)));
  auto &sibling = function (**m, "sibling");
  FunctionAnalysisManager child (f), other (sibling);
  FunctionPassManager passes;
  passes.add<BuildSSAPass> ();
  REQUIRE (passes.run (f, child));
  REQUIRE (passes.run (sibling, other));
  auto form = f.ssa_form (), sibling_form = sibling.ssa_form ();
  REQUIRE (form);
  REQUIRE (sibling_form);
  REQUIRE (std::ranges::distance (form->phis ()) > 0);
  auto before = dump_ssa (f);
  DataInitializer initial = ZeroData{8};
  SUBCASE ("BSS") {}
  SUBCASE ("initialized scalar") { initial = InitializedData{Type::i64 (), {Operand (42)}}; }
  SUBCASE ("function reference") { initial = SymbolReference{&f.symbol (), 0}; }
  SUBCASE ("expression") { initial = ExpressionData{&sibling}; }
  REQUIRE ((*m)->create_data ("new_data", std::move (initial)));
  CHECK (form->current ());
  CHECK (sibling_form->current ());
  CHECK (form->verify ());
  CHECK (sibling_form->verify ());
  CHECK (dump_ssa (f) == before);
  auto writable = SSARewriter::get (f);
  REQUIRE (writable);
  if (!writable) return;
  FunctionPassManager finish;
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  REQUIRE (finish.run (f, child));
  CHECK_FALSE (f.ssa_form ());
  REQUIRE (context.verify ());
}
TEST_CASE ("label data updates only its owner's topology and SSA dependencies") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m), &sibling = function (**m, "sibling");
  auto target = f.create_block ();
  REQUIRE (target);
  REQUIRE (IRBuilder (**target).ret (Operand (7)));
  auto label = f.create_label (**target);
  REQUIRE (label);
  ModuleAnalysisManager module (**m);
  auto child = module.function_analyses (f), other = module.function_analyses (sibling);
  REQUIRE (child);
  REQUIRE (other);
  bool ssa = false;
  SUBCASE ("register IR") {}
  SUBCASE ("SSA") {
    ssa = true;
    FunctionPassManager passes;
    passes.add<BuildSSAPass> ();
    REQUIRE (passes.run (f, **child));
    REQUIRE (passes.run (sibling, **other));
  }
  auto cfg = (*child)->get<ControlFlowAnalysis> ();
  REQUIRE (cfg);
  CHECK_FALSE ((*cfg)->reachable[1]);
  REQUIRE ((*child)->get<FunctionGraphAnalysis> ());
  REQUIRE ((*child)->get<DominatorAnalysis> ());
  REQUIRE ((*child)->get<LoopAnalysis> ());
  const RegisterUseIndex *uses = nullptr;
  if (!ssa) {
    REQUIRE ((*child)->get<LivenessAnalysis> ());
    auto result = (*child)->get<RegisterUseAnalysis> ();
    REQUIRE (result);
    uses = *result;
  }
  auto other_cfg = (*other)->get<ControlFlowAnalysis> ();
  REQUIRE (other_cfg);
  const RegisterLiveness *other_live = nullptr;
  if (!ssa) {
    auto result = (*other)->get<LivenessAnalysis> ();
    REQUIRE (result);
    other_live = *result;
  }
  auto bodies = module.get<Bodies> ();
  REQUIRE (bodies);
  REQUIRE (module.get<Flow> ());
  auto before = f.revisions (), sibling_before = sibling.revisions (),
       module_before = (*m)->revisions ();
  if (ssa) {
    auto items = (*m)->items ().size ();
    auto count = std::ranges::distance ((*m)->symbols ());
    auto rejected = (*m)->create_data ("label_data", LabelReference{*label, *label, 4});
    CHECK_FALSE (rejected);
    CHECK (rejected.error ().message.find ("requires completed register IR") != std::string::npos);
    CHECK (f.revisions () == before);
    CHECK ((*m)->revisions () == module_before);
    CHECK (sibling.revisions () == sibling_before);
    CHECK ((*m)->items ().size () == items);
    CHECK (std::ranges::distance ((*m)->symbols ()) == count);
    CHECK_FALSE ((*m)->find_symbol ("label_data"));
    CHECK (f.label_data_uses ().empty ());
    CHECK ((*child)->get_cached<ControlFlowAnalysis> () == *cfg);
    CHECK ((*other)->get_cached<ControlFlowAnalysis> () == *other_cfg);
    REQUIRE (f.ssa_form ()->current ());
    REQUIRE (f.ssa_form ()->verify ());
    REQUIRE (sibling.ssa_form ()->current ());
    REQUIRE (sibling.ssa_form ()->verify ());
    return;
  }
  REQUIRE ((*m)->create_data ("label_data", LabelReference{*label, *label, 4}));
  CHECK (f.revisions ().control_flow == before.control_flow + 1);
  CHECK (f.revisions ().instructions == before.instructions);
  CHECK ((*m)->revisions ().control_flow == module_before.control_flow + 1);
  CHECK ((*m)->revisions ().instructions == module_before.instructions);
  CHECK (sibling.revisions ().control_flow == sibling_before.control_flow);
  CHECK_FALSE ((*child)->get_cached<ControlFlowAnalysis> ());
  CHECK_FALSE ((*child)->get_cached<FunctionGraphAnalysis> ());
  CHECK_FALSE ((*child)->get_cached<DominatorAnalysis> ());
  CHECK_FALSE ((*child)->get_cached<LoopAnalysis> ());
  CHECK_FALSE ((*child)->get_cached<LivenessAnalysis> ());
  if (!ssa) CHECK ((*child)->get_cached<RegisterUseAnalysis> () == uses);
  CHECK ((*other)->get_cached<ControlFlowAnalysis> () == *other_cfg);
  if (!ssa) CHECK ((*other)->get_cached<LivenessAnalysis> () == other_live);
  CHECK (module.get_cached<Bodies> () == *bodies);
  CHECK_FALSE (module.get_cached<Flow> ());
  module.invalidate (PreservedAnalyses::all ());
  CHECK ((*other)->get_cached<ControlFlowAnalysis> () == *other_cfg);
  auto rebuilt = (*child)->get<ControlFlowAnalysis> ();
  REQUIRE (rebuilt);
  CHECK ((*rebuilt)->reachable[1]);
  CHECK ((*rebuilt)->entry_roots.size () == 2);
  REQUIRE (context.verify ());
}
TEST_CASE ("deferred and rejected label data publish control flow changes atomically") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  auto label = f.create_label (*f.blocks ().begin ());
  REQUIRE (label);
  FunctionAnalysisManager analyses (f);
  auto cfg = analyses.get<ControlFlowAnalysis> ();
  REQUIRE (cfg);
  auto before = f.revisions (), module_before = (*m)->revisions ();
  auto declared = (*m)->declare_data ("deferred");
  REQUIRE (declared);
  CHECK (f.revisions ().control_flow == before.control_flow);
  CHECK (analyses.get_cached<ControlFlowAnalysis> () == *cfg);
  SUBCASE ("definition") {
    REQUIRE ((*m)->define_data (**declared, LabelReference{*label}));
    CHECK (f.revisions ().control_flow == before.control_flow + 1);
    CHECK_FALSE (analyses.get_cached<ControlFlowAnalysis> ());
    auto after = f.revisions (), module_after = (*m)->revisions ();
    CHECK_FALSE ((*m)->define_data (**declared, LabelReference{*label}));
    CHECK (f.revisions () == after);
    CHECK ((*m)->revisions () == module_after);
  }
  SUBCASE ("rejected initializer") {
    auto &foreign = function (**m, "foreign");
    auto base = foreign.create_label (*foreign.blocks ().begin ());
    REQUIRE (base);
    before = f.revisions ();
    module_before = (*m)->revisions ();
    CHECK_FALSE ((*m)->define_data (**declared, LabelReference{*label, *base}));
    CHECK (f.revisions () == before);
    CHECK ((*m)->revisions () == module_before);
    CHECK (analyses.get_cached<ControlFlowAnalysis> () == *cfg);
  }
  SUBCASE ("definition during SSA") {
    FunctionPassManager passes;
    passes.add<BuildSSAPass> ();
    REQUIRE (passes.run (f, analyses));
    before = f.revisions ();
    module_before = (*m)->revisions ();
    auto items = (*m)->items ().size ();
    auto rejected = (*m)->define_data (**declared, LabelReference{*label});
    CHECK_FALSE (rejected);
    CHECK (rejected.error ().message.find ("requires completed register IR") != std::string::npos);
    CHECK (f.revisions () == before);
    CHECK ((*m)->revisions () == module_before);
    CHECK ((*m)->items ().size () == items);
    CHECK_FALSE ((*declared)->data ());
    CHECK (f.label_data_uses ().empty ());
    REQUIRE (f.ssa_form ()->current ());
    REQUIRE (f.ssa_form ()->verify ());
  }
}
TEST_CASE ("existing label data remains valid throughout SSA construction verification and exit") {
  Context context;
  auto m = context.create_module ("module");
  REQUIRE (m);
  auto &f = function (**m);
  auto label = f.create_label (*f.blocks ().begin ());
  REQUIRE (label);
  auto address = (*m)->create_data ("address", LabelReference{*label});
  REQUIRE (address);
  auto difference = (*m)->create_data ("difference", LabelReference{*label, *label, 4});
  REQUIRE (difference);
  FunctionAnalysisManager analyses (f);
  FunctionPassManager build;
  build.add<BuildSSAPass> ();
  REQUIRE (build.run (f, analyses));
  REQUIRE (f.ssa_form ());
  REQUIRE (f.ssa_form ()->current ());
  auto revision = f.revisions ().control_flow;
  REQUIRE (context.verify ());
  CHECK (f.revisions ().control_flow == revision);
  REQUIRE (f.label_data_uses ().size () == 2);
  CHECK (f.label_data_uses ()[0] == *address);
  CHECK (f.label_data_uses ()[1] == *difference);
  FunctionPassManager finish;
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  REQUIRE (finish.run (f, analyses));
  CHECK_FALSE (f.ssa_form ());
  REQUIRE (context.verify ());
  CHECK (f.revisions ().control_flow == revision);
}
