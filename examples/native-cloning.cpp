/* Licensed under the MIT License. Hot block cloning on the canonical IR. */
#include "mirnext/Transforms/BlockCloning.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("cloning");
  if (!module) return 1;
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  if (!function) return 1;
  auto entry = (*function)->create_block (), hot = (*function)->create_block (),
       done = (*function)->create_block (), cold = (*function)->create_block ();
  if (!entry || !hot || !done || !cold) return 1;
  auto hot_label = (*function)->create_label (**hot, "hot"),
       cold_label = (*function)->create_label (**cold, "cold");
  auto value = (*function)->create_register (Type::i64 (), "value");
  if (!hot_label || !cold_label || !value) return 1;
  IRBuilder body (**entry);
  if (!body.assign (**value, 0) || !body.branch_if (Operand (1), **cold_label)) return 1;
  if (!body.set_insert_point (**hot) || !body.assign (**value, body.expression (**value) + 1))
    return 1;
  if (!body.branch (Comparison::Less, body.expression (**value), body.literal (14), **hot_label))
    return 1;
  if (!body.set_insert_point (**done) || !body.ret (Operand (**value))) return 1;
  if (!body.set_insert_point (**cold) || !body.assign (**value, 10) || !body.jump (**hot_label))
    return 1;
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
  if (!result) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  auto encoded = serialization::write_binary (context);
  return encoded ? 0 : 1;
}
