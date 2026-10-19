/* Licensed under the MIT License. Analyses share canonical block/instruction identities. */
#include "mirnext/Analysis/FunctionGraph.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("analysis");
  if (!module) return 1;
  auto function = (*module)->create_function ("sum", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return 1;
  auto entry = (*function)->create_block ("entry"), loop = (*function)->create_block ("loop"),
       exit = (*function)->create_block ("exit");
  if (!entry || !loop || !exit) return 1;
  auto label = (*function)->create_label (**loop, "loop");
  if (!label) return 1;
  auto count = (*function)->create_register (Type::i64 (), "count"),
       sum = (*function)->create_register (Type::i64 (), "sum");
  if (!count || !sum) return 1;
  IRBuilder begin (**entry);
  if (!begin.assign (**count, 0) || !begin.assign (**sum, 0)) return 1;
  IRBuilder body (**loop);
  if (!body.assign (**sum, body.expression (**sum) + 14)
      || !body.assign (**count, body.expression (**count) + 1)
      || !body.branch (Comparison::Less, body.expression (**count),
                       body.expression (*(*function)->arguments ()[0]), **label))
    return 1;
  if (!IRBuilder (**exit).ret (Operand (**sum))) return 1;
  analysis::ModuleAnalysisManager module_analyses (**module);
  transforms::ModulePassManager passes;
  passes.add_function<transforms::SimplifyPass> ();
  passes.add_function<transforms::PrepareCFGPass> ();
  auto prepared = passes.run (**module, module_analyses);
  if (!prepared) {
    std::cerr << prepared.error ().message << '\n';
    return 1;
  }
  auto child = module_analyses.function_analyses (**function);
  if (!child) return 1;
  auto &analyses = **child;
  auto graph = analyses.get<analysis::FunctionGraphAnalysis> ();
  auto dom = analyses.get<analysis::DominatorAnalysis> ();
  auto live = analyses.get<analysis::LivenessAnalysis> ();
  auto loops = analyses.get<analysis::LoopAnalysis> ();
  if (!graph || !dom || !live || !loops) return 1;
  auto index = (*graph)->indices.at (&(*label)->block ());
  if ((*loops)->depth (index) != 1 || !(*live)->live_in (index, **sum)
      || !(*dom)->dominates (0, index))
    return 1;
  std::cout << dump (**function) << "loop_depth=" << (*loops)->depth (index)
            << " sum_live=" << (*live)->live_in (index, **sum)
            << " cached_analyses=" << analyses.cached () << '\n';
}
