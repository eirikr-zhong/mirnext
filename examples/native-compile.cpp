/* Licensed under the MIT License. Builder to owned code and native publication. */
#include <mirnext/CodeGen/Compilation.hpp>
#include <mirnext/Execution/CodeMemory.hpp>
#include <mirnext/Transforms/Optimization.hpp>
#include <iostream>
mirnext::Result<mirnext::codegen::CompiledFunction> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("native");
  if (!module) return module.error ();
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return function.error ();
  auto block = (*function)->create_block ();
  if (!block) return block.error ();
  IRBuilder body (**block);
  auto returned = body.ret (body.expression (*(*function)->arguments ()[0]) * 3 + 20);
  if (!returned) return returned.error ();
  auto program = transforms::optimize (context);
  if (!program) return program.error ();
  auto target = target::TargetMachine::native ();
  if (!target) return target.error ();
  return codegen::compile_function (*program->find_function ("native", "entry"), *target,
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
  if (!target) {
    std::cerr << target.error ().message << '\n';
    return 1;
  }
  auto compiled = build ();
  if (!compiled) {
    std::cerr << compiled.error ().message << '\n';
    return 1;
  }
  // The Builder, optimized program and machine nodes have all been destroyed.
  auto function = mirnext::execution::CodeMemory ().publish (std::move (*compiled));
  if (!function) {
    std::cerr << function.error ().message << '\n';
    return 1;
  }
  // The host signature is known here; this is an ABI boundary, not a generic
  // dynamic call facility. The executable handle owns its mapped code pages.
  auto entry = reinterpret_cast<std::int64_t (*) (std::int64_t)> (function->address ());
  auto value = entry (7);
  std::cout << "entry(7) = " << value << '\n';
  return value == 41 ? 0 : 1;
}
