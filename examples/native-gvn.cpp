/* Licensed under the MIT License. Value numbering on canonical, function-owned SSA. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("values");
  auto *state = builder.create_variable (module, "state", std::int64_t{21});
  builder.create_function<std::int64_t ()> (module, "entry");
  auto a = builder.create_load (state), b = builder.create_load (state);
  builder.create_return (builder.create_add (a, b));
  auto built = builder.finish ();
  if (!built.has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  transforms::OptimizationOptions options;
  options.observer = [] (const transforms::OptimizationSnapshot &stage) -> Result<void> {
    if (stage.phase == transforms::PassEvent::Phase::After) {
      std::cout << transforms::pass_name (stage.statistics.pass)
                << " changes=" << stage.statistics.transformations << '\n';
      std::cout << (stage.ssa ().get () ? dump_ssa (stage.function) : dump (stage.function));
    }
    return {};
  };
  auto result = transforms::optimize (context, options);
  if (!result.has_value ()) {
    std::cerr << result.error ().message << '\n';
    return 1;
  }
  auto encoded = result.value ().write_binary ();
  return encoded.has_value () ? 0 : 1;
}
