/* Licensed under the MIT License. Owning BBV diagnostic specialization. */
#include <mirnext/CodeGen/BlockVersioning.hpp>
#include <mirnext/CodeGen/CompiledBlock.hpp>
#include <mirnext/Transforms/Optimization.hpp>
#include <deque>
#include <iostream>
#include <set>
mirnext::Result<mirnext::codegen::BBVProgram> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("bbv");
  if (!module) return module.error ();
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return function.error ();
  auto entry = (*function)->create_block ("entry"), cold = (*function)->create_block ("cold"),
       hot = (*function)->create_block ("hot");
  if (!entry) return entry.error ();
  if (!cold) return cold.error ();
  if (!hot) return hot.error ();
  auto label = (*function)->create_label (**hot, "hot");
  if (!label) return label.error ();
  auto argument = (*function)->arguments ()[0];
  IRBuilder builder (**entry);
  // This is an instruction-level diagnostic example. PRSET/PRBEQ specialize
  // metadata during BBV generation; they do not compare runtime scalar values.
  auto set = builder.emit (Opcode::PrSet, {Operand (*argument), 7});
  if (!set) return set.error ();
  auto branch = builder.emit (Opcode::PrBeq, {Operand (**label), Operand (*argument), 7});
  if (!branch) return branch.error ();
  auto returned = IRBuilder (**cold).ret (17);
  if (!returned) return returned.error ();
  returned = IRBuilder (**hot).ret (42);
  if (!returned) return returned.error ();
  auto optimized = transforms::optimize (context);
  if (!optimized) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target) return target.error ();
  auto prepared
    = codegen::BBVPreparation::prepare (*optimized->find_function ("bbv", "entry"), *target);
  if (!prepared) return prepared.error ();
  return codegen::BBVProgram::create (std::move (*prepared));
}
int main () {
  using namespace mirnext::codegen;
  auto program = build ();
  if (!program) {
    std::cerr << program.error ().message << '\n';
    return 1;
  }
  auto properties = BBVProperties::create (program->preparation ().function ());
  if (!properties) return 1;
  auto entry
    = program->request_version (*program->preparation ().blocks ().begin (), *properties, true);
  if (!entry) return 1;
  std::deque<const BBVVersion *> work{entry->version};
  std::set<const BBVVersion *> seen;
  while (!work.empty ()) {
    auto version = work.front ();
    work.pop_front ();
    if (!seen.insert (version).second) continue;
    auto plan = program->specialize (*version);
    if (!plan) {
      std::cerr << plan.error ().message << '\n';
      return 1;
    }
    auto compiled = encode_bbv (*program, *version);
    if (!compiled) {
      std::cerr << compiled.error ().message << '\n';
      return 1;
    }
    std::cout << "bb" << compiled->version ().index () << ": " << compiled->bytes ().size ()
              << " bytes, " << compiled->successors ().size () << " successor relocations\n";
    for (auto &successor : (*plan)->successors ()) work.push_back (successor.version);
  }
  if (!program->verify ()) return 1;
  // This diagnostic example publishes no executable memory. See bbv-session.cpp
  // for demand-driven execution through ExecutionSession.
  std::cout << program->dump ();
  return 0;
}
