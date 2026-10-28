/* Licensed under the MIT License. Analyses share canonical block/instruction identities. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module_builder = builder.create_module ("analysis");
  builder.create_function<std::int64_t (std::int64_t)> (module_builder, "sum");
  auto *loop = builder.create_block ("loop");
  auto *exit = builder.create_block ("exit");
  auto count = builder.create_register (Type::i64 (), "count");
  auto sum = builder.create_register (Type::i64 (), "sum");
  builder.create_assign (count, builder.constant_i64 (0));
  builder.create_assign (sum, builder.constant_i64 (0));
  builder.set_insert_point (loop);
  builder.create_assign (sum, builder.create_add (sum, builder.constant_i64 (14)));
  builder.create_assign (count, builder.create_add (count, builder.constant_i64 (1)));
  builder.create_cond_branch (builder.create_lt (count, builder.argument (0)), loop);
  builder.set_insert_point (exit);
  builder.create_return (sum);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  auto &module = *context.modules ().begin ();
  auto &function = *module.functions ().begin ();
  analysis::ModuleAnalysisManager module_analyses (module);
  transforms::ModulePassManager passes;
  passes.add_function<transforms::SimplifyPass> ();
  passes.add_function<transforms::PrepareCFGPass> ();
  auto prepared = passes.run (module, module_analyses);
  if (!prepared.has_value ()) {
    std::cerr << prepared.error ().message << '\n';
    return 1;
  }
  auto child = module_analyses.function_analyses (function);
  if (!child.has_value ()) return 1;
  auto &analyses = child.value ();
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  auto dom = analyses.get<analysis::DominatorAnalysis> ();
  auto live = analyses.get<analysis::LivenessAnalysis> ();
  auto loops = analyses.get<analysis::LoopAnalysis> ();
  if (!graph.has_value () || !dom.has_value () || !live.has_value () || !loops.has_value ())
    return 1;
  auto loop_block = function.find_block ("loop");
  auto sum_register = function.find_register ("sum");
  if (!loop_block.has_value () || !sum_register.has_value ()) return 1;
  auto position = graph.value ().index (loop_block.value ());
  if (!position) return 1;
  auto index = *position;
  if (loops.value ().depth (index) != 1 || !live.value ().live_in (index, sum_register.value ())
      || !dom.value ().dominates (0, index))
    return 1;
  std::cout << dump (function) << "loop_depth=" << loops.value ().depth (index)
            << " sum_live=" << live.value ().live_in (index, sum_register.value ())
            << " cached_analyses=" << analyses.cached () << '\n';
}
