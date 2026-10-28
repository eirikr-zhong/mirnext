/* Licensed under the MIT License. Hot block cloning on the canonical IR. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("cloning");
  builder.create_function<std::int64_t ()> (module, "entry");
  auto *hot = builder.create_block ("hot");
  auto *done = builder.create_block ("done");
  auto *cold = builder.create_block ("cold");
  auto value = builder.create_register (Type::i64 (), "value");
  builder.create_assign (value, builder.constant_i64 (0));
  builder.create_cond_branch (builder.constant_i64 (1), cold);
  builder.set_insert_point (hot);
  builder.create_assign (value, builder.create_add (value, builder.constant_i64 (1)));
  builder.create_cond_branch (builder.create_lt (value, builder.constant_i64 (14)), hot);
  builder.set_insert_point (done);
  builder.create_return (value);
  builder.set_insert_point (cold);
  builder.create_assign (value, builder.constant_i64 (10));
  builder.create_jump (hot);
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass> ();
  passes.add<transforms::PrepareCFGPass> ();
  passes.add<transforms::BlockCloningPass> ();
  passes.add<transforms::BuildSSAPass> ();
  passes.add<transforms::ConventionalSSAPass> ();
  passes.add<transforms::DestroySSAPass> ();
  auto result = passes.run (context, [] (const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After)
      std::cout << event.pass << " changes=" << event.transformations << '\n'
                << dump (event.function);
    return {};
  });
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  auto encoded = serialization::write_binary (context);
  return encoded.has_value () ? 0 : 1;
}
