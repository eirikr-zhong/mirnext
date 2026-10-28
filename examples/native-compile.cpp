/* Licensed under the MIT License. Builder to owned code and native publication. */
#include "mirnext/IR/Builder.hpp"
#include <mirnext/CodeGen/Compilation.hpp>
#include <mirnext/Execution/CodeMemory.hpp>
#include <mirnext/Transforms/Optimization.hpp>
#include <iostream>
mirnext::Result<mirnext::codegen::CompiledFunction> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("native");
  builder.create_function<std::int64_t (std::int64_t)> (module, "entry");
  auto tripled = builder.create_mul (builder.argument (0), builder.constant_i64 (3));
  builder.create_return (builder.create_add (tripled, builder.constant_i64 (20)));
  auto built = builder.finish ();
  if (!built.has_value ()) return built.error ();
  auto program = transforms::optimize (std::move (context));
  if (!program.has_value ()) return program.error ();
  auto target = target::TargetMachine::native ();
  if (!target.has_value ()) return target.error ();
  return codegen::compile_function (*program.value ().find_function ("native", "entry").get (),
                                    target.value (),
                                    {.preserve_host_registers = true,
                                     .observer = [] (const codegen::CompilationSnapshot &view)
                                       -> Result<void> {
                                       std::cout << codegen::stage_name (view.stage) << ": "
                                                 << view.instruction_count () << " instructions\n";
                                       return {};
                                     }});
}
int main () {
  auto target = mirnext::target::TargetMachine::native ();
  if (!target.has_value ()) {
    std::cerr << target.error ().message << '\n';
    return 1;
  }
  auto compiled = build ();
  if (!compiled.has_value ()) {
    std::cerr << compiled.error ().message << '\n';
    return 1;
  }
  // The Builder, optimized program and machine nodes have all been destroyed.
  auto function = mirnext::execution::CodeMemory ().publish (std::move (compiled.value ()));
  if (!function.has_value ()) {
    std::cerr << function.error ().message << '\n';
    return 1;
  }
  // The host signature is known here; this is an ABI boundary, not a generic
  // dynamic call facility. The executable handle owns its mapped code pages.
  auto entry = reinterpret_cast<std::int64_t (*) (std::int64_t)> (function.value ().address ());
  auto value = entry (7);
  std::cout << "entry(7) = " << value << '\n';
  return value == 41 ? 0 : 1;
}
