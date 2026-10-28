/* Licensed under the MIT License. Canonical DSL through owning machine preparation. */
#include "mirnext/IR/Builder.hpp"
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <iostream>
mirnext::Result<mirnext::codegen::MachineCompilation> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program program;
  MIRBuilder builder (program);
  auto *module = builder.create_module ("machine-example");
  builder.create_function<std::int64_t (std::int64_t)> (module, "entry");
  auto *bias = builder.create_variable (module, "bias", std::int64_t{2});
  builder.create_return (builder.create_add (builder.argument (0), builder.create_load (bias)));
  auto built = builder.finish ();
  if (!built.has_value ()) return built.error ();
  auto optimized = transforms::optimize (std::move (program));
  if (!optimized.has_value ()) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target.has_value ()) return target.error ();
  return codegen::MachineCompilation::create ((optimized.value ().find_function ("machine-example",
                                                                                 "entry"))
                                                .value (),
                                              target.value ());
}
int main () {
  using namespace mirnext::codegen;
  auto compilation = build ();
  if (!compilation.has_value ()) {
    std::cerr << compilation.error ().message << '\n';
    return 1;
  }
  // Source IR is already destroyed; the machine body has its own identity.
  while (compilation.value ().next_stage ()) {
    auto step = compilation.value ().step ();
    if (!step.has_value ()) {
      std::cerr << step.error ().message << '\n';
      return 1;
    }
    std::cout << stage_name (compilation.value ().stage ())
              << " instructions=" << compilation.value ().function ().instruction_count () << '\n';
    if (auto allocation = compilation.value ().allocation (); allocation.has_value ())
      std::cout << "allocated-registers=" << allocation.value ().used_registers ().size ()
                << " spill-area-bytes=" << allocation.value ().stack_bytes () << '\n';
  }
  std::cout << compilation.value ().function ().dump ();
  auto code = compilation.value ().finish ();
  if (!code.has_value ()) {
    std::cerr << code.error ().message << '\n';
    return 1;
  }
  std::cout << "code-bytes=" << code.value ().bytes ().size ()
            << " relocations=" << code.value ().relocations ().size () << '\n';
}
