#include <mirnext/IR/Builder.hpp>
#include <mirnext/Serialization/Format.hpp>
#include <iostream>
#include <mirnext/Analysis/ControlFlow.hpp>
#include <mirnext/Analysis/FunctionGraph.hpp>
#include <mirnext/Transforms/Materialize.hpp>
#include <mirnext/Transforms/Simplify.hpp>
#include <mirnext/Transforms/Inline.hpp>
#include <mirnext/Transforms/PrepareCFG.hpp>
#include <mirnext/Transforms/SSA.hpp>
#include <mirnext/Transforms/Address.hpp>
#include <mirnext/Transforms/BlockCloning.hpp>
#include <mirnext/Transforms/GVN.hpp>
#include <mirnext/Transforms/CopyPropagation.hpp>
#include <mirnext/Transforms/DeadStores.hpp>
#include <mirnext/Transforms/DeadCode.hpp>
#include <mirnext/Transforms/Motion.hpp>
#include <mirnext/Transforms/SSACombine.hpp>
#include <mirnext/Transforms/JumpOptimization.hpp>
#include <mirnext/Transforms/Optimization.hpp>
#include <mirnext/CodeGen/MachineIR.hpp>
#include <mirnext/CodeGen/Analysis.hpp>
#include <mirnext/CodeGen/Lowering.hpp>
#include <mirnext/CodeGen/RegisterAllocation.hpp>
#include <mirnext/CodeGen/RegisterRewrite.hpp>
#include <mirnext/CodeGen/EdgeSpills.hpp>
#include <mirnext/CodeGen/PostAllocation.hpp>
#include <mirnext/CodeGen/Frame.hpp>
#include <mirnext/CodeGen/CompiledFunction.hpp>
#include <mirnext/CodeGen/Compilation.hpp>
#include <mirnext/CodeGen/BlockVersioning.hpp>
#include <mirnext/CodeGen/CompiledBlock.hpp>
#include <mirnext/CodeGen/BlockEntry.hpp>
#include <mirnext/Execution/CodeMemory.hpp>
#include <mirnext/Execution/Session.hpp>
#include <mirnext/Execution/Invocation.hpp>
int main () {
  using namespace mirnext::ir;
  Program context;
  MIRBuilder construction (context);
  auto *module_builder = construction.create_module ("consumer");
  construction.create_function<std::int64_t ()> (module_builder, "entry");
  auto *scope = construction.begin_stack_scope ("temporary");
  auto *variable = construction.create_variable (module_builder, "value", std::int64_t{40});
  auto sum
    = construction.create_add (construction.create_load (variable), construction.constant_i64 (2));
  construction.create_store (variable, sum);
  auto *host = construction.create_import<std::int64_t (std::int64_t)> (module_builder, "host");
  auto local = construction.create_register (Type::i64 ());
  construction.create_assign (local, construction.create_call (host, {sum}).value ());
  auto pointer = construction.create_address_of (local);
  construction.create_return (
    construction.create_load (construction.memory (Type::i64 (), pointer)));
  construction.end_stack_scope (scope);
  construction.create_data (module_builder, "value_reference",
                            SymbolReference (variable->symbol ()));
  auto built = construction.finish ();
  if (!(built).has_value ()) {
    std::cerr << built.error ().message << '\n';
    return 1;
  }
  auto &module = *context.modules ().begin ();
  auto &function = *module.functions ().begin ();
  {
    MIRBuilder edit (function);
    mirnext::OptionalRef<const Instruction> ret;
    for (auto &block : function.blocks ())
      for (auto &instruction : block.instructions ())
        if (instruction.opcode () == Opcode::Ret) ret = instruction;
    if (!ret.has_value ()) return 22;
    auto *source = &ret.value ().block ();
    auto *suffix = edit.split_before (&ret.value (), "return");
    if (!suffix || function.find_block ("return").get () != suffix) return 22;
    edit.merge_fallthrough (source);
    if (!edit.finish ().has_value () || function.find_block ("return").has_value ()) return 24;
  }
  mirnext::analysis::FunctionAnalysisManager analyses (function);
  auto graph = analyses.get<mirnext::analysis::ControlFlowAnalysis> ();
  if (!(graph).has_value () || graph.value ().blocks ().size () != 2) return 6;
  auto reference = module.find_symbol ("value_reference").get ();
  auto source_symbol = module.find_symbol ("value").get ();
  if (!reference || !source_symbol || source_symbol->reference_data_uses ().size () != 1
      || &source_symbol->reference_data_uses ()[0] != reference->data ().get ())
    return 125;
  if (analyses.get_cached<mirnext::analysis::ControlFlowAnalysis> ().get () != &(graph).value ())
    return 127;
  if (!function.label_data_uses ().empty () || !function.expression_data_uses ().empty ())
    return 126;
  auto copied = clone (context);
  if (!(copied).has_value () || dump ((copied).value ()) != dump (context)) return 7;
  auto &copied_function = *copied.value ().modules ().begin ()->functions ().begin ();
  mirnext::analysis::FunctionAnalysisManager copied_analyses (copied_function);
  mirnext::transforms::FunctionPassManager passes;
  passes.add<mirnext::transforms::MaterializeStackScopesPass> ();
  if (!(passes.run (copied_function, copied_analyses)).has_value ()
      || needs_materialization ((copied).value ()))
    return 15;
  mirnext::transforms::FunctionPassManager simplify;
  simplify.add<mirnext::transforms::SimplifyPass> ();
  if (!(simplify.run (copied_function, copied_analyses)).has_value ()
      || !(copied.value ().verify ()).has_value ())
    return 25;
  mirnext::analysis::ModuleAnalysisManager module_analyses (copied_function.module ());
  mirnext::transforms::ModulePassManager inline_pass;
  inline_pass.add_function<mirnext::transforms::InlinePass> ();
  if (!(inline_pass.run (copied_function.module (), module_analyses)).has_value ()) return 26;
  mirnext::transforms::FunctionPassManager cfg_pass;
  cfg_pass.add<mirnext::transforms::PrepareCFGPass> ();
  if (!(cfg_pass.run (copied_function, copied_analyses)).has_value ()) return 27;
  mirnext::transforms::FunctionPassManager cloning;
  cloning.add<mirnext::transforms::BlockCloningPass> ();
  if (!(cloning.run (copied_function, copied_analyses)).has_value ()) return 34;
  auto dominators = copied_analyses.get<mirnext::analysis::DominatorAnalysis> ();
  auto liveness = copied_analyses.get<mirnext::analysis::LivenessAnalysis> ();
  auto loops = copied_analyses.get<mirnext::analysis::LoopAnalysis> ();
  if (!(dominators).has_value () || !(liveness).has_value () || !(loops).has_value ()) return 28;
  mirnext::transforms::FunctionPassManager ssa;
  ssa.add<mirnext::transforms::BuildSSAPass> (false);
  ssa.add<mirnext::transforms::AddressTransformPass> ();
  ssa.add<mirnext::transforms::BuildSSAPass> ();
  ssa.add<mirnext::transforms::GVNPass> ();
  ssa.add<mirnext::transforms::CopyPropagationPass> ();
  ssa.add<mirnext::transforms::DeadStoresPass> ();
  ssa.add<mirnext::transforms::SSADeadCodePass> ();
  ssa.add<mirnext::transforms::LICMPass> ();
  ssa.add<mirnext::transforms::PressureReliefPass> ();
  ssa.add<mirnext::transforms::ConventionalSSAPass> ();
  ssa.add<mirnext::transforms::SSACombinePass> ();
  ssa.add<mirnext::transforms::DestroySSAPass> ();
  ssa.add<mirnext::transforms::JumpOptimizationPass> ();
  if (!(ssa.run (copied_function, copied_analyses)).has_value ()
      || copied_function.ssa_form ().get ())
    return 29;
  if (!(mirnext::serialization::write_binary ((copied).value ())).has_value ()) return 30;
  auto optimized = mirnext::transforms::optimize (module);
  if (!(optimized).has_value () || !(optimized.value ().verify ()).has_value ()
      || !optimized.value ().find_function ("consumer", "entry").get ())
    return 35;
  if (!(optimized.value ().write_text ()).has_value ()
      || !(optimized.value ().write_binary ()).has_value ())
    return 36;
  auto target = mirnext::target::TargetMachine::native ();
  if (!(target).has_value ()) return 37;
  auto machine
    = mirnext::codegen::MachineCompilation::create ((optimized.value ().find_function ("consumer",
                                                                                       "entry"))
                                                      .value (),
                                                    (target).value ());
  if (!(machine).has_value () || !(machine.value ().function ().verify ()).has_value ()
      || machine.value ().function ().dump ().empty ())
    return 38;
  auto machine_graph = mirnext::codegen::MachineGraphAnalysis::build (machine.value ().function ());
  if (!(machine_graph).has_value ()) return 40;
  if (!(mirnext::codegen::MachineLiveness::analyze (machine.value ().function (),
                                                    (machine_graph).value ()))
         .has_value ())
    return 41;
  if (!(machine.value ().run_until (mirnext::codegen::CompilationStage::Allocate)).has_value ())
    return 42;
  if (!machine.value ().function ().state ().machinized
      || !machine.value ().function ().state ().operands_legalized
      || machine_graph.value ().current (machine.value ().function ()))
    return 46;
  if (!(machine.value ().graph ()).has_value () || !(machine.value ().liveness ()).has_value ()
      || !(machine.value ().live_ranges ()).has_value ()
      || !(machine.value ().allocation ()).has_value ())
    return 47;
  if (!(machine.value ().run_until (mirnext::codegen::CompilationStage::Rewrite)).has_value ()
      || !machine.value ().function ().state ().operands_rewritten
      || !(machine.value ().function ().verify ()).has_value ())
    return 51;
  if ((machine.value ().graph ()).has_value () || (machine.value ().liveness ()).has_value ()
      || (machine.value ().live_ranges ()).has_value ()
      || (machine.value ().allocation ()).has_value ())
    return 50;
  if (!(machine.value ().run_until (mirnext::codegen::CompilationStage::Frame)).has_value ()
      || !(machine.value ().frame ()).has_value ()
      || !machine.value ().frame ().value ().current (machine.value ().function ())
      || !(machine.value ().frame ().value ().cfg ().verify (machine.value ().function ()))
            .has_value ())
    return 54;
  {
    auto compiled = machine.value ().finish ();
    if (!(compiled).has_value () || !(compiled.value ().verify ()).has_value ()
        || compiled.value ().bytes ().empty ())
      return 55;
    // This consumer checks relocation without publishing executable code.
    auto relocated
      = compiled.value ().relocate (0,
                                    [] (const mirnext::codegen::CodeSymbolRequest &request)
                                      -> mirnext::Result<std::uint64_t> {
                                      if ((request.symbol ()).has_value ()
                                          && request.symbol ().value ().module == "consumer") {
                                        if (request.symbol ().value ().kind
                                              == mirnext::codegen::MachineSymbol::Kind::Data
                                            && request.symbol ().value ().name == "value")
                                          return std::uint64_t{0x1000};
                                        if (request.symbol ().value ().kind
                                              == mirnext::codegen::MachineSymbol::Kind::Import
                                            && request.symbol ().value ().name == "host")
                                          return std::uint64_t{0x2000};
                                      }
                                      return mirnext::Error{mirnext::ErrorCode::InvalidOperand,
                                                            "unexpected consumer code symbol"};
                                    });
    if (!(relocated).has_value ()) {
      std::cerr << relocated.error ().message << '\n';
      return 56;
    }
    {
      MIRBuilder edit (context);
      edit.create_function<std::int64_t ()> (&module, "native_constant");
      edit.create_return (edit.constant_i64 (42));
      if (!edit.finish ().has_value ()) return 58;
    }
    auto fresh_program = mirnext::transforms::optimize (module);
    if (!(fresh_program).has_value ()) return 59;
    auto bbv
      = mirnext::codegen::BBVPreparation::prepare (*fresh_program.value ()
                                                      .find_function ("consumer", "native_constant")
                                                      .get (),
                                                   (target).value ());
    if (!(bbv).has_value ()) return 90;
    auto versions = mirnext::codegen::BBVProgram::create (std::move ((bbv).value ()));
    if (!(versions).has_value ()) return 91;
    auto properties
      = mirnext::codegen::BBVProperties::create (versions.value ().preparation ().function ());
    if (!(properties).has_value ()) return 92;
    auto first_version
      = versions.value ().request_version (*versions.value ().preparation ().blocks ().begin (),
                                           (properties).value (), true);
    if (!(first_version).has_value ()) return 93;
    if (!(versions.value ().specialize (first_version.value ().version ())).has_value ()
        || !(versions.value ().verify ()).has_value ())
      return 94;
    auto fragment
      = mirnext::codegen::encode_bbv ((versions).value (), first_version.value ().version ());
    if (!(fragment).has_value () || !(fragment.value ().verify ()).has_value ()) return 95;
    std::uintptr_t destination = 4;
    auto gate
      = mirnext::codegen::make_block_entry ((target).value (),
                                            {reinterpret_cast<std::uintptr_t> (&destination),
                                             reinterpret_cast<std::uintptr_t> (&destination), 4});
    if (!(gate).has_value () || gate.value ().size () != mirnext::codegen::block_entry_bytes)
      return 96;
    if (!(mirnext::codegen::make_block_resolver ((target).value (), 4, 4)).has_value ()) return 97;
    auto native
      = mirnext::codegen::compile_function (*fresh_program.value ()
                                               .find_function ("consumer", "native_constant")
                                               .get (),
                                            (target).value (), {.preserve_host_registers = true});
    if (!(native).has_value ()) return 60;
    auto executable = mirnext::execution::CodeMemory ().publish (std::move ((native).value ()));
    if (!(executable).has_value ()) return 61;
    if (reinterpret_cast<std::int64_t (*) ()> (executable.value ().address ()) () != 42) return 62;
    auto provider = mirnext::execution::native_code_memory_provider ();
    auto arena = mirnext::execution::CodeArena::create (2 * provider->page_size (), provider);
    if (!(arena).has_value ()) return 98;
    for (unsigned batch = 0; batch < 2; ++batch) {
      auto size = executable.value ().artifact ().bytes ().size ();
      auto slots = arena.value ().reserve ({&size, 1});
      if (!(slots).has_value ()) return 99;
      auto address = slots.value ().address (0);
      if (!(address).has_value ()) return 100;
      auto bytes = executable.value ().artifact ().relocate ((address).value ());
      if (!(bytes).has_value ()) return 101;
      std::span<const std::uint8_t> view = (bytes).value ();
      auto published = slots.value ().publish ({&view, 1});
      if (!(published).has_value ()) return 102;
      if (reinterpret_cast<std::int64_t (*) ()> (published.value ().front ().address ()) () != 42)
        return 103;
    }
    if (arena.value ().remaining ()) return 104;
    Program lazy_source;
    MIRBuilder lazy_builder (lazy_source);
    auto *lazy_module = lazy_builder.create_module ("lazy");
    lazy_builder.create_function<std::int64_t ()> (lazy_module, "constant");
    lazy_builder.create_return (lazy_builder.constant_i64 (42));
    if (!lazy_builder.finish ().has_value ()) return 80;
    auto lazy = mirnext::execution::ExecutionSession::
      create (lazy_source, {.link = {.compilation = mirnext::execution::JITCompilation::Lazy}});
    if (!(lazy).has_value ()) return 81;
    auto lazy_function = lazy.value ().find_function ("lazy", "constant");
    if (!(lazy_function).has_value () || lazy_function.value ().artifact ().get ()
        || lazy.value ().statistics ().compiled_functions)
      return 82;
    auto lazy_entry = lazy_function.value ().native_entry ();
    if (!(lazy_entry).has_value ()) return 83;
    auto delayed = lazy_function.value ().call_i64 ();
    if (!(delayed).has_value () || (delayed).value () != 42
        || !lazy_function.value ().artifact ().get ()
        || !(lazy_function.value ().compile ()).has_value ())
      return 84;
    auto warm_entry = lazy_function.value ().native_entry ();
    if (!(warm_entry).has_value ()
        || warm_entry.value ().address () != lazy_entry.value ().address ()
        || lazy.value ().statistics ().compiled_functions != 1)
      return 85;
    auto block_session = mirnext::execution::ExecutionSession::
      create (lazy_source,
              {.link = {.compilation = mirnext::execution::JITCompilation::BlockVersioning}});
    if (!(block_session).has_value ()) return 105;
    auto bf = block_session.value ().find_function ("lazy", "constant");
    if (!(bf).has_value () || !bf.value ().block_statistics ()
        || bf.value ().block_statistics ()->compiled_blocks || bf.value ().artifact ().get ())
      return 106;
    auto block_value = bf.value ().call_i64 ();
    if (!(block_value).has_value () || (block_value).value () != 42
        || bf.value ().artifact ().get () || !bf.value ().block_statistics ()->compiled_blocks)
      return 107;
    auto block_entry = bf.value ().native_entry ();
    if (!(block_entry).has_value ()) return 108;
    mirnext::execution::ExecutionInvocation block_invocation;
    if (reinterpret_cast<std::int64_t (*) ()> (block_entry.value ().address ()) () != 42
        || !(block_invocation.status ()).has_value ())
      return 109;
    {
      MIRBuilder editor (lazy_source);
      auto *symbol = lazy_module->find_symbol ("constant").get ();
      auto *target
        = editor.create_variable (lazy_module, "target", Type::pointer (), Operand (*symbol));
      editor.create_function<std::int64_t ()> (lazy_module, "mixed");
      auto call = editor.create_indirect_call (symbol, editor.create_load (target));
      editor.create_return (call.value ());
      if (!editor.finish ().has_value ()) return 115;
    }
    auto mixed = mirnext::execution::ExecutionSession::
      create (lazy_source,
              {.select_engine
               = [] (const Function &f) -> mirnext::Result<mirnext::execution::ExecutionEngine> {
                return f.symbol ().name () == "constant"
                         ? mirnext::execution::ExecutionEngine::Interpreter
                         : mirnext::execution::ExecutionEngine::LazyJIT;
              }});
    if (!(mixed).has_value () || mixed.value ().mode () != mirnext::execution::ExecutionMode::Mixed)
      return 116;
    auto mixed_entry = mixed.value ().find_function ("lazy", "mixed");
    if (!(mixed_entry).has_value ()) return 117;
    auto mixed_result = mixed_entry.value ().call_i64 ();
    if (!(mixed_result).has_value () || (mixed_result).value () != 42) return 118;
    auto session = mirnext::execution::ExecutionSession::
      create (context,
              {.link
               = {.resolve
                  = [] (auto &request) -> mirnext::Result<mirnext::execution::ExternalSymbol> {
                   auto host = +[] (std::int64_t value) { return value; };
                   if (request.name == "host")
                     return mirnext::execution::ExternalSymbol{reinterpret_cast<std::uintptr_t> (
                                                                 host),
                                                               Signature{{Type::i64 ()},
                                                                         {Type::i64 ()}},
                                                               {}};
                   return mirnext::Error{mirnext::ErrorCode::UnknownName,
                                         "unexpected consumer import"};
                 }}});
    if (!(session).has_value ()) {
      std::cerr << session.error ().message << '\n';
      return 63;
    }
    auto entry = session.value ().find_function ("consumer", "entry");
    if (!(entry).has_value () || !entry.value ().artifact ().get ()
        || !(entry.value ().artifact ().get ()->verify ()).has_value ())
      return 64;
    for (std::int64_t expected : {42, 44}) {
      auto result = expected == 42 ? session.value ().call_i64 ((entry).value ())
                                   : entry.value ().call_i64 ();
      if (!(result).has_value () || (result).value () != expected) return 65;
    }
    if (session.value ().host_entry_count () != 1) return 66;
    auto values = entry.value ().call ();
    if (!(values).has_value () || values.value ().size () != 1) return 70;
    auto generic_result = ((values).value ())[0].as_i64 ();
    if (!(generic_result).has_value () || (generic_result).value () != 46
        || session.value ().host_entry_count () != 2)
      return 71;
  }
  auto interpreted = mirnext::execution::ExecutionSession::
    create (context,
            {.mode = mirnext::execution::ExecutionMode::Interpreter,
             .link = {.resolve
                      = [] (auto &request) -> mirnext::Result<mirnext::execution::ExternalSymbol> {
               if (request.name != "host")
                 return mirnext::Error{mirnext::ErrorCode::UnknownName,
                                       "unexpected interpreted import"};
               mirnext::execution::ExternalSymbol host{0,
                                                       Signature{{Type::i64 ()}, {Type::i64 ()}},
                                                       {}};
               host.interpret
                 = [] (auto args) -> mirnext::Result<std::vector<mirnext::execution::ScalarValue>> {
                 return std::vector<mirnext::execution::ScalarValue>{args[0].scalar ().value ()};
               };
               return host;
             }}});
  if (!(interpreted).has_value ()) {
    std::cerr << interpreted.error ().message << '\n';
    return 67;
  }
  auto interpreted_entry = interpreted.value ().find_function ("consumer", "entry");
  if (!(interpreted_entry).has_value () || interpreted_entry.value ().artifact ().get ()) return 68;
  for (std::int64_t expected : {42, 44}) {
    auto result = expected == 42 ? interpreted.value ().call_i64 ((interpreted_entry).value ())
                                 : interpreted_entry.value ().call_i64 ();
    if (!(result).has_value () || (result).value () != expected) return 69;
  }
  auto values = interpreted_entry.value ().call ();
  if (!(values).has_value () || values.value ().size () != 1) return 72;
  auto generic_result = ((values).value ())[0].as_i64 ();
  if (!(generic_result).has_value () || (generic_result).value () != 46
      || interpreted.value ().host_entry_count ())
    return 73;
  {
    auto callbacks = mirnext::execution::ExecutionSession::
      create (context,
              {.mode = mirnext::execution::ExecutionMode::Interpreter,
               .link = {.resolve = [] (auto &request)
                          -> mirnext::Result<mirnext::execution::ExternalSymbol> {
                 if (request.name != "host")
                   return mirnext::Error{mirnext::ErrorCode::UnknownName,
                                         "unexpected callback import"};
                 mirnext::execution::ExternalSymbol symbol{0,
                                                           Signature{{Type::i64 ()},
                                                                     {Type::i64 ()}},
                                                           {}};
                 symbol.interpret =
                   [] (auto args) -> mirnext::Result<std::vector<mirnext::execution::ScalarValue>> {
                   return std::vector<mirnext::execution::ScalarValue>{args[0].scalar ().value ()};
                 };
                 return symbol;
               }},
               .native_entries = true});
    if (!(callbacks).has_value ()) {
      std::cerr << callbacks.error ().message << '\n';
      return 74;
    }
    auto f = callbacks.value ().find_function ("consumer", "entry");
    if (!(f).has_value ()) return 75;
    auto e = callbacks.value ().native_entry ((f).value ());
    if (!(e).has_value ()) return 76;
    mirnext::execution::ExecutionInvocation invocation;
    if (reinterpret_cast<std::int64_t (*) ()> (e.value ().address ()) () != 42
        || !(invocation.status ()).has_value ())
      return 77;
  }
  auto text = mirnext::serialization::write_text (context);
  if (!(text).has_value ()) return 8;
  auto parsed = mirnext::serialization::parse_text ((text).value ());
  if (!(parsed).has_value () || !(parsed.value ().verify ()).has_value ()) return 9;
  auto second = mirnext::serialization::write_text ((parsed).value ());
  if (!(second).has_value () || (second).value () != (text).value ()) return 10;
  auto binary = mirnext::serialization::write_binary (context);
  if (!(binary).has_value ()) return 11;
  auto decoded = mirnext::serialization::read_binary ((binary).value ());
  if (!(decoded).has_value () || !(decoded.value ().verify ()).has_value ()) return 12;
  auto encoded = mirnext::serialization::write_binary ((decoded).value ());
  if (!(encoded).has_value () || (encoded).value () != (binary).value ()) return 13;
}
