/* Licensed under the MIT License. Private SSA borrows canonical IR and exits before serialization. */
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <iostream>
int main() {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context; auto module = context.create_module("ssa"); if (!module) return 1;
  auto function = (*module)->create_function("sum", {{Type::i64()}, {Type::i64()}}); if (!function) return 1;
  auto entry = (*function)->create_block(), loop = (*function)->create_block(), exit = (*function)->create_block();
  if (!entry || !loop || !exit) return 1;
  auto label = (*function)->create_label(**loop, "loop"); if (!label) return 1;
  auto i = (*function)->create_register(Type::i64(), "i"), sum = (*function)->create_register(Type::i64(), "sum");
  if (!i || !sum) return 1;
  IRBuilder begin(**entry); if (!begin.assign(**i, 0) || !begin.assign(**sum, 0)) return 1;
  IRBuilder body(**loop);
  if (!body.assign(**i, body.expression(**i) + 1) || !body.assign(**sum, body.expression(**sum) + body.expression(**i))
      || !body.branch(Comparison::Less, body.expression(**i), body.expression(*(*function)->arguments()[0]), **label)) return 1;
  if (!IRBuilder(**exit).ret(Operand(**sum))) return 1;
  transforms::FunctionPassManager passes;
  passes.add<transforms::SimplifyPass>(); passes.add<transforms::PrepareCFGPass>();
  passes.add<transforms::BuildSSAPass>(); passes.add<transforms::ConventionalSSAPass>(); passes.add<transforms::DestroySSAPass>();
  auto optimized = passes.run(context, [](const transforms::PassEvent &event) -> Result<void> {
    if (event.phase == transforms::PassEvent::Phase::After) std::cout << event.pass << '\n' << dump(event.function);
    return {};
  });
  if (!optimized) { std::cerr << optimized.error().message << '\n'; return 1; }
  auto binary = serialization::write_binary(context);
  if (!binary) { std::cerr << binary.error().message << '\n'; return 1; }
}
