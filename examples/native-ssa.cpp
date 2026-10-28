/* Licensed under the MIT License. Private SSA borrows canonical IR and exits before serialization.
 */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module_builder = builder.create_module ("ssa");
  builder.create_function<std::int64_t (std::int64_t)> (module_builder, "sum");
  auto *loop = builder.create_block ("loop");
  auto *exit = builder.create_block ("exit");
  auto count = builder.create_register (Type::i64 (), "count");
  auto sum = builder.create_register (Type::i64 (), "sum");
  builder.create_assign (count, builder.constant_i64 (0));
  builder.create_assign (sum, builder.constant_i64 (0));
  builder.set_insert_point (loop);
  builder.create_assign (count, builder.create_add (count, builder.constant_i64 (1)));
  builder.create_assign (sum, builder.create_add (sum, count));
  builder.create_cond_branch (builder.create_lt (count, builder.argument (0)), loop);
  builder.set_insert_point (exit);
  builder.create_return (sum);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::PrepareCFGPass> ();
  passes.add<transforms::BuildSSAPass> ();
  passes.add<transforms::ConventionalSSAPass> ();
  passes.add<transforms::DestroySSAPass> ();
  auto optimized = passes.run (context, [] (const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After)
      std::cout << event.pass << '\n' << dump (event.function);
    return {};
  });
  if (!optimized.has_value ()) {
    std::cerr << optimized.error ().message << '\n';
    return 1;
  }
  auto binary = serialization::write_binary (context);
  if (!binary.has_value ()) {
    std::cerr << binary.error ().message << '\n';
    return 1;
  }
}
