/* Licensed under the MIT License. Owning BBV diagnostic specialization. */
#include "mirnext/IR/Builder.hpp"
#include <mirnext/CodeGen/BlockVersioning.hpp>
#include <mirnext/CodeGen/CompiledBlock.hpp>
#include <mirnext/Transforms/Optimization.hpp>
#include <deque>
#include <iostream>
#include <set>
mirnext::Result<mirnext::codegen::BBVProgram> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Program context;
  MIRBuilder builder (context);
  auto *module = builder.create_module ("bbv");
  builder.create_function<std::int64_t (std::int64_t)> (module, "entry");
  auto *cold = builder.create_block ("cold");
  auto *hot = builder.create_block ("hot");
  auto *label = builder.create_label (hot, "hot");
  // Property instructions use the precise opcode entry point.
  builder.create_instruction (Opcode::PrSet,
                              {builder.argument (0).operand ().value (), Operand (7)});
  builder.create_instruction (Opcode::PrBeq,
                              {Operand (*label), builder.argument (0).operand ().value (),
                               Operand (7)});
  builder.set_insert_point (cold);
  builder.create_return (builder.constant_i64 (17));
  builder.set_insert_point (hot);
  builder.create_return (builder.constant_i64 (42));
  auto built = builder.finish ();
  if (!built.has_value ()) return built.error ();
  auto optimized = transforms::optimize (std::move (context));
  if (!optimized.has_value ()) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target.has_value ()) return target.error ();
  auto prepared
    = codegen::BBVPreparation::prepare (*optimized.value ().find_function ("bbv", "entry").get (),
                                        target.value ());
  if (!prepared.has_value ()) return prepared.error ();
  return codegen::BBVProgram::create (std::move (prepared.value ()));
}
int main () {
  using namespace mirnext::codegen;
  auto program = build ();
  if (!program.has_value ()) {
    std::cerr << program.error ().message << '\n';
    return 1;
  }
  auto properties = BBVProperties::create (program.value ().preparation ().function ());
  if (!properties.has_value ()) return 1;
  auto entry
    = program.value ().request_version (*program.value ().preparation ().blocks ().begin (),
                                        properties.value (), true);
  if (!entry.has_value ()) return 1;
  std::deque<const BBVVersion *> work{&entry.value ().version ()};
  std::set<const BBVVersion *> seen;
  while (!work.empty ()) {
    auto version = work.front ();
    work.pop_front ();
    if (!seen.insert (version).second) continue;
    auto plan = program.value ().specialize (*version);
    if (!plan.has_value ()) {
      std::cerr << plan.error ().message << '\n';
      return 1;
    }
    auto compiled = encode_bbv (program.value (), *version);
    if (!compiled.has_value ()) {
      std::cerr << compiled.error ().message << '\n';
      return 1;
    }
    std::cout << "bb" << compiled.value ().version ().index () << ": "
              << compiled.value ().bytes ().size () << " bytes, "
              << compiled.value ().successors ().size () << " successor relocations\n";
    for (auto &successor : plan.value ().successors ()) work.push_back (&successor.version ());
  }
  if (!(program.value ().verify ()).has_value ()) return 1;
  // This diagnostic example publishes no executable memory. See bbv-session.cpp
  // for demand-driven execution through ExecutionSession.
  std::cout << program.value ().dump ();
  return 0;
}
