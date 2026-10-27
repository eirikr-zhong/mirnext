#include <mirnext/IR/IR.hpp>
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
#include <mirnext/Execution/Interpreter.hpp>
#include <mirnext/Execution/Invocation.hpp>
int main () {
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("consumer");
  if (!module) return 1;
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {}});
  if (!function) return 2;
  auto block = (*function)->create_block ();
  if (!block) return 3;
  IRBuilder builder (**block);
  auto scope = builder.begin_stack_scope ("temporary");
  if (!scope) return 14;
  auto variable = (*module)->create_variable ("value", Type::i64 (), 40);
  if (!variable) return 18;
  auto loaded = builder.load (**variable);
  if (!loaded) return 19;
  auto sum = builder.expression (**loaded) + 2;
  if (!sum.is_valid ()) return 20;
  if (!builder.store (**variable, sum)) return 21;
  auto imported = (*module)->import ("host", Signature{{Type::i64 ()}, {Type::i64 ()}});
  if (!imported) return 16;
  auto call = builder.call (**imported, {sum});
  if (!call) {
    std::cerr << call.error ().message << '\n';
    return 17;
  }
  auto value = call->value ();
  if (!value) return 4;
  auto local = (*function)->create_register (Type::i64 ());
  if (!local || !builder.assign (**local, Operand (**value))) return 31;
  auto pointer = builder.address_of (**local);
  if (!pointer) return 32;
  auto indirect = builder.load (Memory{Type::i64 (), *pointer});
  if (!indirect) return 33;
  auto ret = builder.ret (builder.expression (**indirect));
  if (!ret) return 5;
  auto &source = const_cast<BasicBlock &> ((*ret)->block ());
  IRRewriter rewriter (**function);
  auto suffix = rewriter.split_before (**ret, "return");
  if (!suffix) return 22;
  if ((*function)->find_block ("return") != *suffix || !rewriter.merge_fallthrough (source))
    return 23;
  if ((*function)->find_block ("return") || !builder.end_stack_scope (**scope)
      || !context.verify ())
    return 24;
  mirnext::analysis::FunctionAnalysisManager analyses (**function);
  auto graph = analyses.get<mirnext::analysis::ControlFlowAnalysis> ();
  if (!graph || (*graph)->blocks.size () != 2) return 6;
  auto reference
    = (*module)->create_data ("value_reference", SymbolReference{&(*variable)->symbol (), 0});
  if (!reference || (*variable)->symbol ().reference_data_uses ().size () != 1
      || (*variable)->symbol ().reference_data_uses ()[0] != *reference)
    return 125;
  if (analyses.get_cached<mirnext::analysis::ControlFlowAnalysis> () != *graph) return 127;
  if (!(*function)->label_data_uses ().empty () || !(*function)->expression_data_uses ().empty ())
    return 126;
  auto copied = clone (context);
  if (!copied || dump (**copied) != dump (context)) return 7;
  auto &copied_function = *(*copied)->modules ().begin ()->functions ().begin ();
  mirnext::analysis::FunctionAnalysisManager copied_analyses (copied_function);
  mirnext::transforms::FunctionPassManager passes;
  passes.add<mirnext::transforms::MaterializeStackScopesPass> ();
  if (!passes.run (copied_function, copied_analyses) || needs_materialization (**copied)) return 15;
  mirnext::transforms::FunctionPassManager simplify;
  simplify.add<mirnext::transforms::SimplifyPass> ();
  if (!simplify.run (copied_function, copied_analyses) || !(*copied)->verify ()) return 25;
  mirnext::analysis::ModuleAnalysisManager module_analyses (copied_function.module ());
  mirnext::transforms::ModulePassManager inline_pass;
  inline_pass.add_function<mirnext::transforms::InlinePass> ();
  if (!inline_pass.run (copied_function.module (), module_analyses)) return 26;
  mirnext::transforms::FunctionPassManager cfg_pass;
  cfg_pass.add<mirnext::transforms::PrepareCFGPass> ();
  if (!cfg_pass.run (copied_function, copied_analyses)) return 27;
  mirnext::transforms::FunctionPassManager cloning;
  cloning.add<mirnext::transforms::BlockCloningPass> ();
  if (!cloning.run (copied_function, copied_analyses)) return 34;
  auto dominators = copied_analyses.get<mirnext::analysis::DominatorAnalysis> ();
  auto liveness = copied_analyses.get<mirnext::analysis::LivenessAnalysis> ();
  auto loops = copied_analyses.get<mirnext::analysis::LoopAnalysis> ();
  if (!dominators || !liveness || !loops) return 28;
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
  if (!ssa.run (copied_function, copied_analyses) || copied_function.ssa_form ()) return 29;
  if (!mirnext::serialization::write_binary (**copied)) return 30;
  auto optimized = mirnext::transforms::optimize (**module);
  if (!optimized || !optimized->verify () || !optimized->find_function ("consumer", "entry"))
    return 35;
  if (!optimized->write_text () || !optimized->encode_binary ()) return 36;
  auto target = mirnext::target::TargetMachine::native ();
  if (!target) return 37;
  auto machine
    = mirnext::codegen::prepare_machine_ir (*optimized->find_function ("consumer", "entry"),
                                            *target);
  if (!machine || !(*machine)->verify () || (*machine)->dump ().empty ()) return 38;
  if (!mirnext::codegen::MachineControlFlow::build (**machine)) return 39;
  auto machine_graph = mirnext::codegen::MachineGraphAnalysis::build (**machine);
  if (!machine_graph) return 40;
  if (!mirnext::codegen::MachineLiveness::analyze (**machine, *machine_graph)) return 41;
  auto lowered = mirnext::codegen::machinize (**machine);
  if (!lowered || !(*machine)->state ().machinized || machine_graph->current (**machine)) return 42;
  if (!mirnext::codegen::legalize_operands (**machine) || !(*machine)->state ().operands_legalized)
    return 46;
  auto lowered_graph = mirnext::codegen::MachineGraphAnalysis::build (**machine);
  if (!lowered_graph) return 43;
  auto coalesced = mirnext::codegen::coalesce_registers (**machine, *lowered_graph);
  if (!coalesced) return 47;
  lowered_graph = mirnext::codegen::MachineGraphAnalysis::build (**machine);
  if (!lowered_graph) return 48;
  auto lowered_live = mirnext::codegen::MachineLiveness::analyze (**machine, *lowered_graph);
  if (!lowered_live) return 44;
  auto ranges
    = mirnext::codegen::MachineLiveRanges::build (**machine, *lowered_graph, *lowered_live);
  if (!ranges) return 49;
  auto allocation = mirnext::codegen::MachineAllocation::assign (**machine, *lowered_graph,
                                                                 *lowered_live, *ranges);
  if (!allocation) return 50;
  auto rewritten = mirnext::codegen::rewrite_allocated (**machine, *lowered_graph, *lowered_live,
                                                        *ranges, std::move (*allocation));
  if (!rewritten || !rewritten->current (**machine) || !(*machine)->state ().operands_rewritten
      || !(*machine)->verify ())
    return 51;
  auto spills = mirnext::codegen::split_allocated_spills (**machine, std::move (*rewritten));
  if (!spills || !spills->current (**machine) || !spills->cfg ().verify (**machine)
      || !(*machine)->state ().edge_spills_inserted || !(*machine)->verify ())
    return 52;
  auto post = mirnext::codegen::optimize_allocated_machine (**machine, std::move (*spills));
  if (!post || !post->current (**machine) || !post->cfg ().verify (**machine)
      || !(*machine)->state ().post_allocation_optimized || !(*machine)->verify ())
    return 53;
  {
    auto frame = mirnext::codegen::form_frame (**machine, std::move (*post));
    if (!frame || !frame->current (**machine) || !frame->cfg ().verify (**machine)
        || !(*machine)->state ().frame_formed || !(*machine)->verify ())
      return 54;
    auto compiled = mirnext::codegen::encode_machine (**machine, *frame);
    if (!compiled || !compiled->verify () || compiled->bytes ().empty ()) return 55;
    // This consumer checks relocation without publishing executable code.
    auto relocated
      = compiled->relocate (0,
                            [] (const mirnext::codegen::CodeSymbolRequest &request)
                              -> mirnext::Result<std::uint64_t> {
                              if (request.symbol && request.symbol->module == "consumer") {
                                if (request.symbol->kind
                                      == mirnext::codegen::MachineSymbol::Kind::Data
                                    && request.symbol->name == "value")
                                  return std::uint64_t{0x1000};
                                if (request.symbol->kind
                                      == mirnext::codegen::MachineSymbol::Kind::Import
                                    && request.symbol->name == "host")
                                  return std::uint64_t{0x2000};
                              }
                              return mirnext::Error{mirnext::ErrorCode::InvalidOperand,
                                                    "unexpected consumer code symbol"};
                            });
    if (!relocated) {
      std::cerr << relocated.error ().message << '\n';
      return 56;
    }
    auto fresh = (*module)->create_function ("native_constant", {{Type::i64 ()}, {}});
    if (!fresh) return 57;
    auto fresh_block = (*fresh)->create_block ();
    if (!fresh_block || !IRBuilder (**fresh_block).ret (42)) return 58;
    auto fresh_program = mirnext::transforms::optimize (**module);
    if (!fresh_program) return 59;
    auto bbv = mirnext::codegen::BBVPreparation::prepare (*fresh_program
                                                             ->find_function ("consumer",
                                                                              "native_constant"),
                                                          *target);
    if (!bbv) return 90;
    auto versions = mirnext::codegen::BBVProgram::create (std::move (*bbv));
    if (!versions) return 91;
    auto properties
      = mirnext::codegen::BBVProperties::create (versions->preparation ().function ());
    if (!properties) return 92;
    auto first_version
      = versions->request_version (*versions->preparation ().blocks ().begin (), *properties, true);
    if (!first_version) return 93;
    if (!versions->specialize (*first_version->version) || !versions->verify ()) return 94;
    auto fragment = mirnext::codegen::encode_bbv (*versions, *first_version->version);
    if (!fragment || !fragment->verify ()) return 95;
    std::uintptr_t destination = 4;
    auto gate
      = mirnext::codegen::make_block_entry (*target,
                                            {reinterpret_cast<std::uintptr_t> (&destination),
                                             reinterpret_cast<std::uintptr_t> (&destination), 4});
    if (!gate || gate->size () != mirnext::codegen::block_entry_bytes) return 96;
    if (!mirnext::codegen::make_block_resolver (*target, 4, 4)) return 97;
    auto native
      = mirnext::codegen::compile_function (*fresh_program->find_function ("consumer",
                                                                           "native_constant"),
                                            *target, {.preserve_host_registers = true});
    if (!native) return 60;
    auto executable = mirnext::execution::CodeMemory ().publish (std::move (*native));
    if (!executable) return 61;
    if (reinterpret_cast<std::int64_t (*) ()> (executable->address ()) () != 42) return 62;
    auto provider = mirnext::execution::native_code_memory_provider ();
    auto arena = mirnext::execution::CodeArena::create (2 * provider->page_size (), provider);
    if (!arena) return 98;
    for (unsigned batch = 0; batch < 2; ++batch) {
      auto size = executable->artifact ().bytes ().size ();
      auto slots = arena->reserve ({&size, 1});
      if (!slots) return 99;
      auto address = slots->address (0);
      if (!address) return 100;
      auto bytes = executable->artifact ().relocate (*address);
      if (!bytes) return 101;
      std::span<const std::uint8_t> view = *bytes;
      auto published = slots->publish ({&view, 1});
      if (!published) return 102;
      if (reinterpret_cast<std::int64_t (*) ()> (published->front ().address ()) () != 42)
        return 103;
    }
    if (arena->remaining ()) return 104;
    Context lazy_source;
    auto lm = lazy_source.create_module ("lazy");
    if (!lm) return 78;
    auto lf = (*lm)->create_function ("constant", {{Type::i64 ()}, {}});
    if (!lf) return 79;
    auto lb = (*lf)->create_block ();
    if (!lb || !IRBuilder (**lb).ret (42)) return 80;
    auto lazy = mirnext::execution::ExecutionSession::
      create (lazy_source, {.link = {.compilation = mirnext::execution::JITCompilation::Lazy}});
    if (!lazy) return 81;
    auto lazy_function = lazy->find_function ("lazy", "constant");
    if (!lazy_function || lazy_function->artifact () || lazy->statistics ().compiled_functions)
      return 82;
    auto lazy_entry = lazy_function->native_entry ();
    if (!lazy_entry) return 83;
    auto delayed = lazy_function->call_i64 ();
    if (!delayed || *delayed != 42 || !lazy_function->artifact () || !lazy_function->compile ())
      return 84;
    auto warm_entry = lazy_function->native_entry ();
    if (!warm_entry || warm_entry->address () != lazy_entry->address ()
        || lazy->statistics ().compiled_functions != 1)
      return 85;
    auto block_session = mirnext::execution::ExecutionSession::
      create (lazy_source,
              {.link = {.compilation = mirnext::execution::JITCompilation::BlockVersioning}});
    if (!block_session) return 105;
    auto bf = block_session->find_function ("lazy", "constant");
    if (!bf || !bf->block_statistics () || bf->block_statistics ()->compiled_blocks
        || bf->artifact ())
      return 106;
    auto block_value = bf->call_i64 ();
    if (!block_value || *block_value != 42 || bf->artifact ()
        || !bf->block_statistics ()->compiled_blocks)
      return 107;
    auto block_entry = bf->native_entry ();
    if (!block_entry) return 108;
    mirnext::execution::ExecutionInvocation block_invocation;
    if (reinterpret_cast<std::int64_t (*) ()> (block_entry->address ()) () != 42
        || !block_invocation.status ())
      return 109;
    auto mixed_pointer
      = (*lm)->create_variable ("target", Type::pointer (), Operand ((*lf)->symbol ()));
    if (!mixed_pointer) return 110;
    auto mixed_caller = (*lm)->create_function ("mixed", {{Type::i64 ()}, {}});
    if (!mixed_caller) return 111;
    auto mixed_block = (*mixed_caller)->create_block ();
    if (!mixed_block) return 112;
    IRBuilder mixed_builder (**mixed_block);
    auto mixed_target = mixed_builder.load (**mixed_pointer);
    if (!mixed_target) return 113;
    auto mixed_call = mixed_builder.call_indirect ((*lf)->symbol (), Operand (**mixed_target), {});
    if (!mixed_call) return 114;
    auto mixed_value = mixed_call->value ();
    if (!mixed_value || !mixed_builder.ret (Operand (**mixed_value))) return 115;
    auto mixed = mirnext::execution::ExecutionSession::
      create (lazy_source,
              {.select_engine
               = [] (const Function &f) -> mirnext::Result<mirnext::execution::ExecutionEngine> {
                return f.symbol ().name () == "constant"
                         ? mirnext::execution::ExecutionEngine::Interpreter
                         : mirnext::execution::ExecutionEngine::LazyJIT;
              }});
    if (!mixed || mixed->mode () != mirnext::execution::ExecutionMode::Mixed) return 116;
    auto mixed_entry = mixed->find_function ("lazy", "mixed");
    if (!mixed_entry) return 117;
    auto mixed_result = mixed_entry->call_i64 ();
    if (!mixed_result || *mixed_result != 42) return 118;
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
    if (!session) {
      std::cerr << session.error ().message << '\n';
      return 63;
    }
    auto entry = session->find_function ("consumer", "entry");
    if (!entry || !entry->artifact () || !entry->artifact ()->verify ()) return 64;
    for (std::int64_t expected : {42, 44}) {
      auto result = expected == 42 ? session->call_i64 (*entry) : entry->call_i64 ();
      if (!result || *result != expected) return 65;
    }
    if (session->host_entry_count () != 1) return 66;
    auto values = entry->call ();
    if (!values || values->size () != 1) return 70;
    auto generic_result = (*values)[0].as_i64 ();
    if (!generic_result || *generic_result != 46 || session->host_entry_count () != 2) return 71;
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
                 return std::vector<mirnext::execution::ScalarValue>{*args[0].scalar ()};
               };
               return host;
             }}});
  if (!interpreted) {
    std::cerr << interpreted.error ().message << '\n';
    return 67;
  }
  auto interpreted_entry = interpreted->find_function ("consumer", "entry");
  if (!interpreted_entry || interpreted_entry->artifact ()) return 68;
  for (std::int64_t expected : {42, 44}) {
    auto result = expected == 42 ? interpreted->call_i64 (*interpreted_entry)
                                 : interpreted_entry->call_i64 ();
    if (!result || *result != expected) return 69;
  }
  auto values = interpreted_entry->call ();
  if (!values || values->size () != 1) return 72;
  auto generic_result = (*values)[0].as_i64 ();
  if (!generic_result || *generic_result != 46 || interpreted->host_entry_count ()) return 73;
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
                   return std::vector<mirnext::execution::ScalarValue>{*args[0].scalar ()};
                 };
                 return symbol;
               }},
               .native_entries = true});
    if (!callbacks) {
      std::cerr << callbacks.error ().message << '\n';
      return 74;
    }
    auto f = callbacks->find_function ("consumer", "entry");
    if (!f) return 75;
    auto e = callbacks->native_entry (*f);
    if (!e) return 76;
    mirnext::execution::ExecutionInvocation invocation;
    if (reinterpret_cast<std::int64_t (*) ()> (e->address ()) () != 42 || !invocation.status ())
      return 77;
  }
  auto text = mirnext::serialization::write_text (context);
  if (!text) return 8;
  auto parsed = mirnext::serialization::parse_text (*text);
  if (!parsed || !(*parsed)->verify ()) return 9;
  auto second = mirnext::serialization::write_text (**parsed);
  if (!second || *second != *text) return 10;
  auto binary = mirnext::serialization::write_binary (context);
  if (!binary) return 11;
  auto decoded = mirnext::serialization::read_binary (*binary);
  if (!decoded || !(*decoded)->verify ()) return 12;
  auto encoded = mirnext::serialization::write_binary (**decoded);
  if (!encoded || *encoded != *binary) return 13;
}
