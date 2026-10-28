/* Licensed under the MIT License. Native module analysis and pass lifecycles. */
#include "mirnext/IR/Construction.hpp"
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
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
  auto f = mirnext::ir::detail::access (module).create_function (name, {{Type::i64 ()}, {}});
  REQUIRE ((f).has_value ());
  auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  REQUIRE ((InstructionBuilder ((b).value ()).ret (Operand (1))).has_value ());
  return (f).value ();
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
      if (!(child).has_value ()) return child.error ();
      auto graph = child.value ().get<ControlFlowAnalysis> ();
      if (!(graph).has_value ()) return graph.error ();
      blocks += mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size ();
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
    if (!(source).has_value ()) return source.error ();
    return source.value ().symbols;
  }
};
struct Recursive {
  using Result = unsigned;
  static constexpr Dependency dependencies = Dependency::All;
  static mirnext::Result<Result> run (const Module &, ModuleAnalysisManager &manager) {
    auto value = manager.get<Recursive> ();
    if (!(value).has_value ()) return value.error ();
    return (value).value ();
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
    if (!(before).has_value ()) return before.error ();
    auto symbol = &module.functions ().begin ()->symbol ();
    context.builder.export_symbol (symbol);
    auto preserved = PreservedAnalyses::all ();
    // A preserved entry still expires when one of its dependencies changes.
    return PassResult{1, std::move (preserved)};
  }
};
struct PreserveBodies final : ModulePass {
  std::string_view name () const noexcept override { return "preserve-bodies"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
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
  mirnext::Result<void> initialize (Program &) override {
    ++initialized;
    return {};
  }
  void finalize () noexcept override { ++finalized; }
  mirnext::Result<PassResult> run (MIRBuilder &edit, FunctionAnalysisManager &analyses) override {
    auto &f = *edit.function ();

    events.push_back ("function:" + std::string (f.module ().name ()) + "/"
                      + std::string (f.symbol ().name ()));
    auto graph = analyses.get<ControlFlowAnalysis> ();
    if (!(graph).has_value ()) return graph.error ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    edit.replace_operand (&ret, 0, Operand (42));
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
struct AddFunction final : ModulePass {
  std::vector<std::string> &events;
  explicit AddFunction (std::vector<std::string> &events) : events (events) {}
  std::string_view name () const noexcept override { return "add-function"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    events.push_back ("module:" + std::string (module.name ()));
    context.builder.create_function<std::int64_t ()> (&module, "created");
    context.builder.create_return (context.builder.constant_i64 (1));
    return PassResult{1, PreservedAnalyses::none ()};
  }
};
struct Failing final : ModulePass {
  unsigned &finalized;
  bool initialize_failure;
  explicit Failing (unsigned &finalized, bool initialize_failure = false)
    : finalized (finalized), initialize_failure (initialize_failure) {}
  std::string_view name () const noexcept override { return "fail-module"; }
  mirnext::Result<void> initialize (Program &) override {
    if (initialize_failure) return Error{ErrorCode::InvalidArgument, "initialization failed"};
    return {};
  }
  void finalize () noexcept override { ++finalized; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    auto source = context.analyses.get<Bodies> ();
    if (!(source).has_value ()) return source.error ();
    auto &f = *module.functions ().begin ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    auto changed = InstructionRewriter (f).replace_operand (ret, 0, Operand (7));
    if (!(changed).has_value ()) return changed.error ();
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
      if (!(symbols).has_value ()) return symbols.error ();
      auto bodies = context.analyses.get<Bodies> ();
      if (!(bodies).has_value ()) return bodies.error ();
    } else if (module.name () == "a") {
      auto other = const_cast<Module *> (module.program ().find_module ("b").get ());
      REQUIRE (other);
      auto result = mirnext::ir::detail::access (*(other)).export_symbol (
        other->functions ().begin ()->symbol ());
      if (!(result).has_value ()) return result.error ();
      CHECK_FALSE ((managers.at (other)->get_cached<Symbols> ()).has_value ());
      CHECK ((managers.at (other)->get_cached<Bodies> ()).has_value ());
    } else {
      CHECK_FALSE ((context.analyses.get_cached<Symbols> ()).has_value ());
      CHECK ((context.analyses.get_cached<Bodies> ()).has_value ());
    }
    return PassResult{mutate ? 1u : 0u, PreservedAnalyses::all ()};
  }
};
struct AppendHelper final : FunctionPass {
  std::string_view name () const noexcept override { return "append-helper"; }
  mirnext::Result<PassResult> run (MIRBuilder &edit, FunctionAnalysisManager &) override {
    auto &f = *edit.function ();

    function (f.module (), "helper");
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
struct BreakFunction final : ModulePass {
  std::string_view name () const noexcept override { return "break-function"; }
  mirnext::Result<PassResult> run (Module &module, ModulePassContext &context) override {
    auto &f = *module.functions ().begin ();
    auto &ret = *f.blocks ().begin ()->instructions ().begin ();
    auto result = InstructionRewriter (f).erase (ret);
    if (!(result).has_value ()) return result.error ();
    return PassResult{1, PreservedAnalyses::all ()};
  }
};
}  // namespace
TEST_CASE ("module analyses track bodies registers symbols and data independently") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  ModuleAnalysisManager analyses ((m).value ());
  Bodies::runs = Flow::runs = Registers::runs = Symbols::runs = Data::runs = 0;
  REQUIRE ((analyses.get<Bodies> ()).has_value ());
  REQUIRE ((analyses.get<Flow> ()).has_value ());
  REQUIRE ((analyses.get<Registers> ()).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  REQUIRE ((analyses.get<Data> ()).has_value ());
  REQUIRE ((analyses.get<Dependent> ()).has_value ());
  auto &ret = *f.blocks ().begin ()->instructions ().begin ();
  REQUIRE ((InstructionRewriter (f).replace_operand (ret, 0, Operand (42))).has_value ());
  CHECK_FALSE ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK ((analyses.get_cached<Flow> ()).has_value ());
  CHECK ((analyses.get_cached<Registers> ()).has_value ());
  CHECK ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK ((analyses.get_cached<Data> ()).has_value ());
  REQUIRE ((analyses.get<Bodies> ()).has_value ());
  CHECK (Bodies::runs == 2);
  CHECK (Flow::runs == 1);
  auto bound = mirnext::ir::detail::access (f).create_register (Type::i64 (), "bound", "r5");
  REQUIRE ((bound).has_value ());
  CHECK_FALSE ((analyses.get_cached<Registers> ()).has_value ());
  REQUIRE ((analyses.get<Registers> ()).has_value ());
  auto alias = mirnext::ir::detail::access (f).create_register (Type::i64 (), "alias", "r5");
  REQUIRE ((alias).has_value ());
  CHECK (&(bound).value () == &(alias).value ());
  CHECK_FALSE ((analyses.get_cached<Registers> ()).has_value ());
  REQUIRE ((analyses.get<Registers> ()).has_value ());
  CHECK (Registers::runs == 3);
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).export_symbol (f.symbol ())).has_value ());
  CHECK_FALSE ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Dependent> ()).has_value ());
  CHECK ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK ((analyses.get_cached<Data> ()).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).rename_symbol (f.symbol (), "renamed"))
             .has_value ());
  CHECK_FALSE ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK ((analyses.get_cached<Data> ()).has_value ());
  CHECK ((analyses.get_cached<Flow> ()).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ()).create_data ("bytes", ZeroData{8})).has_value ());
  CHECK_FALSE ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Data> ()).has_value ());
  CHECK ((analyses.get_cached<Bodies> ()).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  REQUIRE ((analyses.get<Data> ()).has_value ());
  auto declaration
    = mirnext::ir::detail::access (((m)).value ()).declare_function ("later", {{Type::i64 ()}, {}});
  REQUIRE ((declaration).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  auto definition
    = mirnext::ir::detail::access (((m)).value ()).define_function ((declaration).value ());
  REQUIRE ((definition).has_value ());
  CHECK_FALSE ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Flow> ()).has_value ());
  auto b = mirnext::ir::detail::access (((definition)).value ()).create_block ();
  REQUIRE ((b).has_value ());
  REQUIRE ((InstructionBuilder ((b).value ()).ret (Operand (0))).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("module analyses own results detect cycles and reject foreign functions") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  ModuleAnalysisManager analyses ((m).value ());
  auto value = analyses.get<Owned> ();
  REQUIRE ((value).has_value ());
  CHECK (*(value).value () == 42);
  auto taken = analyses.take_cached<Owned> ();
  REQUIRE ((taken).has_value ());
  REQUIRE ((taken).value ());
  CHECK (**(taken).value () == 42);
  CHECK_FALSE ((analyses.get_cached<Owned> ()).has_value ());
  REQUIRE ((analyses.set<Owned> (std::make_unique<unsigned> (7))).has_value ());
  CHECK (*(analyses.get<Owned> ()).value () == 7);
  auto recursive = analyses.get<Recursive> ();
  CHECK_FALSE ((recursive).has_value ());
  CHECK (recursive.error ().message.find ("module: analysis dependency cycle")
         != std::string::npos);
  REQUIRE ((analyses.set<Recursive> (3)).has_value ());
  REQUIRE ((analyses.get<Recursive> ()).has_value ());
  CHECK ((analyses.get<Recursive> ()).value () == 3);
  auto child = analyses.function_analyses (f);
  REQUIRE ((child).has_value ());
  REQUIRE ((child.value ().get<ControlFlowAnalysis> ()).has_value ());
  REQUIRE ((analyses.get<ModuleBlocks> ()).has_value ());
  CHECK ((analyses.get<ModuleBlocks> ()).value () == 1);
  analyses.clear ();
  CHECK (analyses.cached () == 0);
  CHECK (child.value ().cached () == 0);
  auto again = analyses.function_analyses (f);
  REQUIRE ((again).has_value ());
  auto first_cfg = child.value ().get<ControlFlowAnalysis> ();
  auto second_cfg = again.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((first_cfg).has_value ());
  REQUIRE ((second_cfg).has_value ());
  CHECK (&(first_cfg).value () == &(second_cfg).value ());
  auto copied_manager = (child).value ();
  CHECK (copied_manager.get_cached<ControlFlowAnalysis> ().get () == &(first_cfg).value ());
  auto other = mirnext::ir::detail::access (context).create_module ("other");
  REQUIRE ((other).has_value ());
  auto &foreign = function ((other).value ());
  CHECK_FALSE ((analyses.function_analyses (foreign)).has_value ());
}
TEST_CASE ("module pass pipeline preserves global phase order and observes nested functions") {
  Program context;
  auto a = mirnext::ir::detail::access (context).create_module ("a"),
       b = mirnext::ir::detail::access (context).create_module ("b");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  function ((a).value ());
  function ((b).value ());
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
  REQUIRE ((result).has_value ());
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
      CHECK ((f.blocks ().begin ()->instructions ().begin ()->operands ()[0].get<std::int64_t> ())
               .value ()
             == 42);
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("module preservation retains current analyses and expires changed dependencies") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  ModuleAnalysisManager analyses ((m).value ());
  REQUIRE ((analyses.get<Bodies> ()).has_value ());
  REQUIRE ((analyses.get<Symbols> ()).has_value ());
  auto child = analyses.function_analyses (f);
  REQUIRE ((child).has_value ());
  REQUIRE ((child.value ().get<ControlFlowAnalysis> ()).has_value ());
  ModulePassManager passes;
  passes.add<AddExport> ();
  REQUIRE ((passes.run ((m).value (), analyses)).has_value ());
  CHECK ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Symbols> ()).has_value ());
  CHECK ((child.value ().get_cached<ControlFlowAnalysis> ()).has_value ());
  std::vector<std::string> events;
  unsigned initialized = 0, finalized = 0;
  ModulePassManager rewrite;
  rewrite.add_function<RewriteReturns> (events, initialized, finalized);
  REQUIRE ((rewrite.run ((m).value (), analyses)).has_value ());
  CHECK_FALSE ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK ((child.value ().get_cached<ControlFlowAnalysis> ()).has_value ());
  REQUIRE ((analyses.get<Bodies> ()).has_value ());
  REQUIRE ((analyses.get<Flow> ()).has_value ());
  ModulePassManager preserve;
  preserve.add<PreserveBodies> ();
  REQUIRE ((preserve.run ((m).value (), analyses)).has_value ());
  CHECK ((analyses.get_cached<Bodies> ()).has_value ());
  CHECK_FALSE ((analyses.get_cached<Flow> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<ControlFlowAnalysis> ()).has_value ());
  auto other = mirnext::ir::detail::access (context).create_module ("other");
  REQUIRE ((other).has_value ());
  CHECK_FALSE ((passes.run ((other).value (), analyses)).has_value ());
}
TEST_CASE ("module failures finalize once clear caches and carry stage diagnostics") {
  for (bool initialize_failure : {false, true}) {
    Program context;
    auto m = mirnext::ir::detail::access (context).create_module ("module");
    REQUIRE ((m).has_value ());
    auto &f = function ((m).value ());
    ModuleAnalysisManager analyses ((m).value ());
    REQUIRE ((analyses.get<Bodies> ()).has_value ());
    auto child = analyses.function_analyses (f);
    REQUIRE ((child).has_value ());
    REQUIRE ((child.value ().get<ControlFlowAnalysis> ()).has_value ());
    unsigned finalized = 0;
    ModulePassManager passes;
    passes.add<Failing> (finalized, initialize_failure);
    auto result = passes.run ((m).value (), analyses);
    CHECK_FALSE ((result).has_value ());
    CHECK (finalized == 1);
    CHECK (analyses.cached () == 0);
    CHECK (child.value ().cached () == 0);
    CHECK (result.error ().message.find ("[fail-module]") != std::string::npos);
    CHECK (result.error ().message.find (initialize_failure
                                           ? "initialization failed"
                                           : "module [fail-module]: transformation failed")
           != std::string::npos);
    REQUIRE ((context.verify ()).has_value ());
  }
  for (auto phase : {PassEvent::Phase::Before, PassEvent::Phase::After}) {
    Program context;
    auto m = mirnext::ir::detail::access (context).create_module ("observe");
    REQUIRE ((m).has_value ());
    function ((m).value ());
    ModuleAnalysisManager analyses ((m).value ());
    REQUIRE ((analyses.get<Bodies> ()).has_value ());
    ModulePassManager passes;
    passes.add<AddExport> ();
    auto result = passes.run ((m).value (), analyses,
                              [&] (const ModulePassEvent &event) -> mirnext::Result<void> {
                                if (event.phase == phase)
                                  return Error{ErrorCode::InvalidArgument, "observer stopped"};
                                return {};
                              });
    CHECK_FALSE ((result).has_value ());
    CHECK (analyses.cached () == 0);
    CHECK (result.error ().message.find ("observe [add-export]: observer stopped")
           != std::string::npos);
    std::vector<std::string> events;
    unsigned initialized = 0, finalized = 0;
    ModulePassManager nested;
    nested.add_function<RewriteReturns> (events, initialized, finalized);
    result = nested.run ((m).value (), analyses, {},
                         [&] (const PassEvent &event) -> mirnext::Result<void> {
                           if (event.phase == phase)
                             return Error{ErrorCode::InvalidArgument, "function observer stopped"};
                           return {};
                         });
    CHECK_FALSE ((result).has_value ());
    CHECK (initialized == 1);
    CHECK (finalized == 1);
    CHECK (analyses.cached () == 0);
    CHECK (
      result.error ().message.find ("observe/entry [rewrite-returns]: function observer stopped")
      != std::string::npos);
  }
  Program empty;
  ModulePassManager invalid;
  invalid.add<FunctionPassAdaptor> (std::unique_ptr<FunctionPass>{});
  auto rejected = invalid.run (empty);
  CHECK_FALSE ((rejected).has_value ());
  CHECK (rejected.error ().message.find ("requires a pass") != std::string::npos);
}
TEST_CASE ("cross-module mutations invalidate the changed owner immediately") {
  Program context;
  auto a = mirnext::ir::detail::access (context).create_module ("a"),
       b = mirnext::ir::detail::access (context).create_module ("b");
  REQUIRE ((a).has_value ());
  REQUIRE ((b).has_value ());
  function ((a).value ());
  function ((b).value ());
  std::unordered_map<const Module *, ModuleAnalysisManager *> managers;
  ModulePassManager passes;
  passes.add<CrossModule> (managers, false);
  passes.add<CrossModule> (managers, true);
  REQUIRE ((passes.run (context)).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("adapted function traversal accepts helpers without revisiting the current pass") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  function ((m).value ());
  std::vector<std::string> events;
  unsigned initialized = 0, finalized = 0;
  ModulePassManager passes;
  passes.add_function<AppendHelper> ();
  passes.add_function<RewriteReturns> (events, initialized, finalized);
  ModuleAnalysisManager analyses ((m).value ());
  REQUIRE ((passes.run ((m).value (), analyses)).has_value ());
  CHECK (events.size () == 2);
  CHECK (events[1] == "function:module/helper");
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("module verification rejects broken results before after-stage observation") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  function ((m).value ());
  ModuleAnalysisManager analyses ((m).value ());
  REQUIRE ((analyses.get<Bodies> ()).has_value ());
  ModulePassManager passes;
  passes.add<BreakFunction> ();
  unsigned after = 0;
  auto result = passes.run ((m).value (), analyses,
                            [&] (const ModulePassEvent &event) -> mirnext::Result<void> {
                              after += event.phase == PassEvent::Phase::After;
                              return {};
                            });
  CHECK_FALSE ((result).has_value ());
  CHECK (after == 0);
  CHECK (analyses.cached () == 0);
  CHECK (result.error ().message.find ("module [break-function]") != std::string::npos);
}
TEST_CASE ("symbol-only mutations invalidate symbol analyses while retaining SSA def-use") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  ModuleAnalysisManager analyses ((m).value ());
  auto child = analyses.function_analyses (f);
  REQUIRE ((child).has_value ());
  FunctionPassManager passes;
  passes.add<BuildSSAPass> ();
  REQUIRE ((passes.run (f, (child).value ())).has_value ());
  auto form = f.ssa_form ().get ();
  REQUIRE (form);
  REQUIRE (form->current ());
  auto before = f.revisions ();
  REQUIRE ((child.value ().get<FunctionSymbols> ()).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).export_symbol (f.symbol ())).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).prototype ("unrelated", {})).has_value ());
  REQUIRE ((mirnext::ir::detail::access (((m)).value ()).rename_symbol (f.symbol (), "renamed"))
             .has_value ());
  CHECK (f.revisions () == before);
  CHECK_FALSE ((child.value ().get_cached<FunctionSymbols> ()).has_value ());
  CHECK (form->current ());
  REQUIRE ((form->verify ()).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE (
  "ordinary data changes retain function topology and liveness while expiring data analyses") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ()), &sibling = function ((m).value (), "sibling");
  ModuleAnalysisManager module ((m).value ());
  auto child = module.function_analyses (f), other = module.function_analyses (sibling);
  REQUIRE ((child).has_value ());
  REQUIRE ((other).has_value ());
  auto cfg = child.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((cfg).has_value ());
  auto graph = child.value ().get<FunctionGraphAnalysis> ();
  REQUIRE ((graph).has_value ());
  auto dominators = child.value ().get<DominatorAnalysis> ();
  REQUIRE ((dominators).has_value ());
  auto loops = child.value ().get<LoopAnalysis> ();
  REQUIRE ((loops).has_value ());
  auto live = child.value ().get<LivenessAnalysis> ();
  REQUIRE ((live).has_value ());
  auto uses = child.value ().get<RegisterUseAnalysis> ();
  REQUIRE ((uses).has_value ());
  auto other_cfg = other.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((other_cfg).has_value ());
  auto bodies = module.get<Bodies> ();
  REQUIRE ((bodies).has_value ());
  auto flow = module.get<Flow> ();
  REQUIRE ((flow).has_value ());
  REQUIRE ((module.get<Data> ()).has_value ());
  REQUIRE ((child.value ().get<FunctionData> ()).has_value ());
  REQUIRE ((other.value ().get<FunctionData> ()).has_value ());
  REQUIRE ((child.value ().get<FunctionSymbols> ()).has_value ());
  auto before = f.revisions ();
  DataInitializer initial = ZeroData{8};
  SUBCASE ("BSS") {}
  SUBCASE ("initialized scalar") { initial = InitializedData{Type::i64 (), {Operand (42)}}; }
  SUBCASE ("function reference") {
    initial = mirnext::ir::detail::NodeAccess::make_SymbolReference (&f.symbol (), 0);
  }
  SUBCASE ("expression") {
    initial = mirnext::ir::detail::NodeAccess::make_ExpressionData (&sibling);
  }
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ()).create_data ("new_data", std::move (initial)))
      .has_value ());
  CHECK (f.revisions ().instructions == before.instructions);
  CHECK (f.revisions ().control_flow == before.control_flow);
  CHECK (f.revisions ().registers == before.registers);
  CHECK (f.revisions ().module_data > before.module_data);
  CHECK (child.value ().get_cached<ControlFlowAnalysis> ().get () == &(cfg).value ());
  CHECK (other.value ().get_cached<ControlFlowAnalysis> ().get () == &(other_cfg).value ());
  CHECK (child.value ().get_cached<FunctionGraphAnalysis> ().get () == &(graph).value ());
  CHECK (child.value ().get_cached<DominatorAnalysis> ().get () == &(dominators).value ());
  CHECK (child.value ().get_cached<LoopAnalysis> ().get () == &(loops).value ());
  CHECK (child.value ().get_cached<LivenessAnalysis> ().get () == &(live).value ());
  CHECK (child.value ().get_cached<RegisterUseAnalysis> ().get () == &(uses).value ());
  CHECK (module.get_cached<Bodies> ().get () == &(bodies).value ());
  CHECK (module.get_cached<Flow> ().get () == &(flow).value ());
  CHECK_FALSE ((module.get_cached<Data> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<FunctionData> ()).has_value ());
  CHECK_FALSE ((other.value ().get_cached<FunctionData> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<FunctionSymbols> ()).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("ordinary data changes retain SSA def-use and its checked editing phase") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto created = mirnext::ir::detail::access (((m)).value ())
                   .create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  REQUIRE ((created).has_value ());
  auto &f = (created).value ();
  auto entry = mirnext::ir::detail::access (f).create_block (),
       no = mirnext::ir::detail::access (f).create_block (),
       yes = mirnext::ir::detail::access (f).create_block (),
       merge = mirnext::ir::detail::access (f).create_block ();
  REQUIRE ((entry).has_value ());
  REQUIRE ((no).has_value ());
  REQUIRE ((yes).has_value ());
  REQUIRE ((merge).has_value ());
  auto selected = mirnext::ir::detail::access (f).create_label ((yes).value ()),
       done = mirnext::ir::detail::access (f).create_label ((merge).value ());
  REQUIRE ((selected).has_value ());
  REQUIRE ((done).has_value ());
  auto value = mirnext::ir::detail::access (f).create_register (Type::i64 ());
  REQUIRE ((value).has_value ());
  REQUIRE (
    (InstructionBuilder ((entry).value ())
       .branch_if (Operand (*mirnext::ir::detail::ReferenceAccess::pointers (f.arguments ())[0]),
                   (selected).value ()))
      .has_value ());
  InstructionBuilder cold ((no).value ());
  REQUIRE ((cold.emit (Opcode::Mov, {Operand ((value).value ()), Operand (1)})).has_value ());
  REQUIRE ((cold.jump ((done).value ())).has_value ());
  REQUIRE ((InstructionBuilder ((yes).value ())
              .emit (Opcode::Mov, {Operand ((value).value ()), Operand (42)}))
             .has_value ());
  REQUIRE ((InstructionBuilder ((merge).value ()).ret (Operand ((value).value ()))).has_value ());
  auto &sibling = function ((m).value (), "sibling");
  FunctionAnalysisManager child (f), other (sibling);
  FunctionPassManager passes;
  passes.add<BuildSSAPass> ();
  REQUIRE ((passes.run (f, child)).has_value ());
  REQUIRE ((passes.run (sibling, other)).has_value ());
  auto form = f.ssa_form ().get (), sibling_form = sibling.ssa_form ().get ();
  REQUIRE (form);
  REQUIRE (sibling_form);
  REQUIRE (std::ranges::distance (form->phis ()) > 0);
  auto before = dump_ssa (f);
  DataInitializer initial = ZeroData{8};
  SUBCASE ("BSS") {}
  SUBCASE ("initialized scalar") { initial = InitializedData{Type::i64 (), {Operand (42)}}; }
  SUBCASE ("function reference") {
    initial = mirnext::ir::detail::NodeAccess::make_SymbolReference (&f.symbol (), 0);
  }
  SUBCASE ("expression") {
    initial = mirnext::ir::detail::NodeAccess::make_ExpressionData (&sibling);
  }
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ()).create_data ("new_data", std::move (initial)))
      .has_value ());
  CHECK (form->current ());
  CHECK (sibling_form->current ());
  CHECK ((form->verify ()).has_value ());
  CHECK ((sibling_form->verify ()).has_value ());
  CHECK (dump_ssa (f) == before);
  auto writable = SSAUpdater::get (f);
  REQUIRE ((writable).has_value ());
  if (!(writable).has_value ()) return;
  FunctionPassManager finish;
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  REQUIRE ((finish.run (f, child)).has_value ());
  CHECK_FALSE ((f.ssa_form ()).has_value ());
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("label data updates only its owner's topology and SSA dependencies") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ()), &sibling = function ((m).value (), "sibling");
  auto target = mirnext::ir::detail::access (f).create_block ();
  REQUIRE ((target).has_value ());
  REQUIRE ((InstructionBuilder ((target).value ()).ret (Operand (7))).has_value ());
  auto label = mirnext::ir::detail::access (f).create_label ((target).value ());
  REQUIRE ((label).has_value ());
  ModuleAnalysisManager module ((m).value ());
  auto child = module.function_analyses (f), other = module.function_analyses (sibling);
  REQUIRE ((child).has_value ());
  REQUIRE ((other).has_value ());
  bool ssa = false;
  SUBCASE ("register IR") {}
  SUBCASE ("SSA") {
    ssa = true;
    FunctionPassManager passes;
    passes.add<BuildSSAPass> ();
    REQUIRE ((passes.run (f, (child).value ())).has_value ());
    REQUIRE ((passes.run (sibling, (other).value ())).has_value ());
  }
  auto cfg = child.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((cfg).has_value ());
  CHECK_FALSE (cfg.value ().reachable[1]);
  REQUIRE ((child.value ().get<FunctionGraphAnalysis> ()).has_value ());
  REQUIRE ((child.value ().get<DominatorAnalysis> ()).has_value ());
  REQUIRE ((child.value ().get<LoopAnalysis> ()).has_value ());
  const RegisterUseIndex *uses = nullptr;
  if (!ssa) {
    REQUIRE ((child.value ().get<LivenessAnalysis> ()).has_value ());
    auto result = child.value ().get<RegisterUseAnalysis> ();
    REQUIRE ((result).has_value ());
    uses = &(result).value ();
  }
  auto other_cfg = other.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((other_cfg).has_value ());
  const RegisterLiveness *other_live = nullptr;
  if (!ssa) {
    auto result = other.value ().get<LivenessAnalysis> ();
    REQUIRE ((result).has_value ());
    other_live = &(result).value ();
  }
  auto bodies = module.get<Bodies> ();
  REQUIRE ((bodies).has_value ());
  REQUIRE ((module.get<Flow> ()).has_value ());
  auto before = f.revisions (), sibling_before = sibling.revisions (),
       module_before = m.value ().revisions ();
  if (ssa) {
    auto items = m.value ().items ().size ();
    auto count = std::ranges::distance (m.value ().symbols ());
    auto rejected
      = mirnext::ir::detail::access (((m)).value ())
          .create_data ("label_data",
                        mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                              &(label).value (),
                                                                              4));
    CHECK_FALSE ((rejected).has_value ());
    CHECK (rejected.error ().message.find ("requires completed register IR") != std::string::npos);
    CHECK (f.revisions () == before);
    CHECK (m.value ().revisions () == module_before);
    CHECK (sibling.revisions () == sibling_before);
    CHECK (m.value ().items ().size () == items);
    CHECK (std::ranges::distance (m.value ().symbols ()) == count);
    CHECK_FALSE ((m.value ().find_symbol ("label_data")).has_value ());
    CHECK (f.label_data_uses ().empty ());
    CHECK (child.value ().get_cached<ControlFlowAnalysis> ().get () == &(cfg).value ());
    CHECK (other.value ().get_cached<ControlFlowAnalysis> ().get () == &(other_cfg).value ());
    REQUIRE (f.ssa_form ().value ().current ());
    REQUIRE ((f.ssa_form ().value ().verify ()).has_value ());
    REQUIRE (sibling.ssa_form ().value ().current ());
    REQUIRE ((sibling.ssa_form ().value ().verify ()).has_value ());
    return;
  }
  REQUIRE (
    (mirnext::ir::detail::access (((m)).value ())
       .create_data ("label_data",
                     mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                           &(label).value (), 4)))
      .has_value ());
  CHECK (f.revisions ().control_flow == before.control_flow + 1);
  CHECK (f.revisions ().instructions == before.instructions);
  CHECK (m.value ().revisions ().control_flow == module_before.control_flow + 1);
  CHECK (m.value ().revisions ().instructions == module_before.instructions);
  CHECK (sibling.revisions ().control_flow == sibling_before.control_flow);
  CHECK_FALSE ((child.value ().get_cached<ControlFlowAnalysis> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<FunctionGraphAnalysis> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<DominatorAnalysis> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<LoopAnalysis> ()).has_value ());
  CHECK_FALSE ((child.value ().get_cached<LivenessAnalysis> ()).has_value ());
  if (!ssa) CHECK (child.value ().get_cached<RegisterUseAnalysis> ().get () == uses);
  CHECK (other.value ().get_cached<ControlFlowAnalysis> ().get () == &(other_cfg).value ());
  if (!ssa) CHECK (other.value ().get_cached<LivenessAnalysis> ().get () == other_live);
  CHECK (module.get_cached<Bodies> ().get () == &(bodies).value ());
  CHECK_FALSE ((module.get_cached<Flow> ()).has_value ());
  module.invalidate (PreservedAnalyses::all ());
  CHECK (other.value ().get_cached<ControlFlowAnalysis> ().get () == &(other_cfg).value ());
  auto rebuilt = child.value ().get<ControlFlowAnalysis> ();
  REQUIRE ((rebuilt).has_value ());
  CHECK (rebuilt.value ().reachable[1]);
  CHECK (rebuilt.value ().entry_roots.size () == 2);
  REQUIRE ((context.verify ()).has_value ());
}
TEST_CASE ("deferred and rejected label data publish control flow changes atomically") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  auto label = mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ());
  REQUIRE ((label).has_value ());
  FunctionAnalysisManager analyses (f);
  auto cfg = analyses.get<ControlFlowAnalysis> ();
  REQUIRE ((cfg).has_value ());
  auto before = f.revisions (), module_before = m.value ().revisions ();
  auto declared = mirnext::ir::detail::access (((m)).value ()).declare_data ("deferred");
  REQUIRE ((declared).has_value ());
  CHECK (f.revisions ().control_flow == before.control_flow);
  CHECK (analyses.get_cached<ControlFlowAnalysis> ().get () == &(cfg).value ());
  SUBCASE ("definition") {
    REQUIRE (
      (mirnext::ir::detail::access (((m)).value ())
         .define_data ((declared).value (),
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ())))
        .has_value ());
    CHECK (f.revisions ().control_flow == before.control_flow + 1);
    CHECK_FALSE ((analyses.get_cached<ControlFlowAnalysis> ()).has_value ());
    auto after = f.revisions (), module_after = m.value ().revisions ();
    CHECK_FALSE (
      (mirnext::ir::detail::access (((m)).value ())
         .define_data ((declared).value (),
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ())))
        .has_value ());
    CHECK (f.revisions () == after);
    CHECK (m.value ().revisions () == module_after);
  }
  SUBCASE ("rejected initializer") {
    auto &foreign = function ((m).value (), "foreign");
    auto base = mirnext::ir::detail::access (foreign).create_label (*foreign.blocks ().begin ());
    REQUIRE ((base).has_value ());
    before = f.revisions ();
    module_before = m.value ().revisions ();
    CHECK_FALSE (
      (mirnext::ir::detail::access (((m)).value ())
         .define_data ((declared).value (),
                       mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                             &(base).value ())))
        .has_value ());
    CHECK (f.revisions () == before);
    CHECK (m.value ().revisions () == module_before);
    CHECK (analyses.get_cached<ControlFlowAnalysis> ().get () == &(cfg).value ());
  }
  SUBCASE ("definition during SSA") {
    FunctionPassManager passes;
    passes.add<BuildSSAPass> ();
    REQUIRE ((passes.run (f, analyses)).has_value ());
    before = f.revisions ();
    module_before = m.value ().revisions ();
    auto items = m.value ().items ().size ();
    auto rejected
      = mirnext::ir::detail::access (((m)).value ())
          .define_data ((declared).value (),
                        mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ()));
    CHECK_FALSE ((rejected).has_value ());
    CHECK (rejected.error ().message.find ("requires completed register IR") != std::string::npos);
    CHECK (f.revisions () == before);
    CHECK (m.value ().revisions () == module_before);
    CHECK (m.value ().items ().size () == items);
    CHECK_FALSE ((declared.value ().data ()).has_value ());
    CHECK (f.label_data_uses ().empty ());
    REQUIRE (f.ssa_form ().value ().current ());
    REQUIRE ((f.ssa_form ().value ().verify ()).has_value ());
  }
}
TEST_CASE ("existing label data remains valid throughout SSA construction verification and exit") {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("module");
  REQUIRE ((m).has_value ());
  auto &f = function ((m).value ());
  auto label = mirnext::ir::detail::access (f).create_label (*f.blocks ().begin ());
  REQUIRE ((label).has_value ());
  auto address
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("address",
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value ()));
  REQUIRE ((address).has_value ());
  auto difference
    = mirnext::ir::detail::access (((m)).value ())
        .create_data ("difference",
                      mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                            &(label).value (), 4));
  REQUIRE ((difference).has_value ());
  FunctionAnalysisManager analyses (f);
  FunctionPassManager build;
  build.add<BuildSSAPass> ();
  REQUIRE ((build.run (f, analyses)).has_value ());
  REQUIRE ((f.ssa_form ()).has_value ());
  REQUIRE (f.ssa_form ().value ().current ());
  auto revision = f.revisions ().control_flow;
  REQUIRE ((context.verify ()).has_value ());
  CHECK (f.revisions ().control_flow == revision);
  REQUIRE (f.label_data_uses ().size () == 2);
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[0]
         == &(address).value ());
  CHECK (mirnext::ir::detail::ReferenceAccess::pointers (f.label_data_uses ())[1]
         == &(difference).value ());
  FunctionPassManager finish;
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  REQUIRE ((finish.run (f, analyses)).has_value ());
  CHECK_FALSE ((f.ssa_form ()).has_value ());
  REQUIRE ((context.verify ()).has_value ());
  CHECK (f.revisions ().control_flow == revision);
}
