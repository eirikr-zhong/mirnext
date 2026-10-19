/* Licensed under the MIT License. Value numbering on canonical, function-owned SSA. */
#include "mirnext/Transforms/Optimization.hpp"
#include <iostream>
int main() {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context; auto module = context.create_module("values"); if (!module) return 1;
  auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return 1;
  auto block = (*function)->create_block(); if (!block) return 1;
  auto state = (*module)->create_variable("state", Type::i64(), 21); if (!state) return 1;
  IRBuilder body(**block);
  auto a = body.load(**state), b = body.load(**state); if (!a || !b) return 1;
  if (!body.ret(body.expression(**a) + body.expression(**b))) return 1;
  transforms::OptimizationOptions options;
  options.observer = [](const transforms::OptimizationSnapshot &stage) -> Result<void> {
    if (stage.phase == transforms::PassEvent::Phase::After) {
      std::cout << transforms::pass_name(stage.statistics.pass) << " changes=" << stage.statistics.transformations << '\n';
      std::cout << (stage.ssa() ? dump_ssa(stage.function) : dump(stage.function));
    }
    return {};
  };
  auto result = transforms::optimize(context, options);
  if (!result) { std::cerr << result.error().message << '\n'; return 1; }
  auto encoded = result->encode_binary(); return encoded ? 0 : 1;
}
