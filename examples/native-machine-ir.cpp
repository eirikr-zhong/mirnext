/* Licensed under the MIT License. Canonical DSL through owning machine preparation. */
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include "mirnext/CodeGen/EdgeSpills.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include "mirnext/CodeGen/Frame.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/Transforms/Optimization.hpp"
#include <iostream>
mirnext::Result<std::unique_ptr<mirnext::codegen::MachineFunction>> build () {
  using namespace mirnext;
  using namespace mirnext::ir;
  Context context;
  auto module = context.create_module ("machine-example");
  if (!module) return module.error ();
  auto function = (*module)->create_function ("entry", {{Type::i64 ()}, {Type::i64 ()}});
  if (!function) return function.error ();
  auto block = (*function)->create_block ();
  if (!block) return block.error ();
  IRBuilder body (**block);
  auto bias = (*module)->create_variable ("bias", Type::i64 (), 2);
  if (!bias) return bias.error ();
  auto value = body.load (**bias);
  if (!value) return value.error ();
  auto returned
    = body.ret (body.expression (*(*function)->arguments ()[0]) + body.expression (**value));
  if (!returned) return returned.error ();
  auto optimized = transforms::optimize (context);
  if (!optimized) return optimized.error ();
  auto target = target::TargetMachine::native ();
  if (!target) return target.error ();
  return codegen::prepare_machine_ir (*optimized->find_function ("machine-example", "entry"),
                                      *target);
}
int main () {
  auto machine = build ();
  if (!machine) {
    std::cerr << machine.error ().message << '\n';
    return 1;
  }
  // Both the Builder Context and the optimized program have been destroyed.
  {
    auto lowered = mirnext::codegen::machinize (**machine);
    if (!lowered) {
      std::cerr << lowered.error ().message << '\n';
      return 1;
    }
    auto legalized = mirnext::codegen::legalize_operands (**machine);
    if (!legalized) {
      std::cerr << legalized.error ().message << '\n';
      return 1;
    }
    std::cout << (*machine)->target ().name () << " target lowering and operand legalization\n";
  }
  std::cout << (*machine)->dump ();
  auto graph = mirnext::codegen::MachineGraphAnalysis::build (**machine);
  if (!graph) {
    std::cerr << graph.error ().message << '\n';
    return 1;
  }
  {
    auto coalesced = mirnext::codegen::coalesce_registers (**machine, *graph);
    if (!coalesced) {
      std::cerr << coalesced.error ().message << '\n';
      return 1;
    }
    std::cout << "coalesced-moves=" << coalesced->coalesced_moves () << '\n';
    graph = mirnext::codegen::MachineGraphAnalysis::build (**machine);
    if (!graph) {
      std::cerr << graph.error ().message << '\n';
      return 1;
    }
  }
  auto live = mirnext::codegen::MachineLiveness::analyze (**machine, *graph);
  if (!live) {
    std::cerr << live.error ().message << '\n';
    return 1;
  }
  auto ranges = mirnext::codegen::MachineLiveRanges::build (**machine, *graph, *live);
  if (!ranges) {
    std::cerr << ranges.error ().message << '\n';
    return 1;
  }
  {
    auto allocation
      = mirnext::codegen::MachineAllocation::assign (**machine, *graph, *live, *ranges);
    if (!allocation) {
      std::cerr << allocation.error ().message << '\n';
      return 1;
    }
    std::cout << "allocated-registers=" << allocation->used_registers ().size ()
              << " spill-area-bytes=" << allocation->stack_bytes () << '\n';
    auto rewritten = mirnext::codegen::rewrite_allocated (**machine, *graph, *live, *ranges,
                                                          std::move (*allocation));
    if (!rewritten) {
      std::cerr << rewritten.error ().message << '\n';
      return 1;
    }
    std::cout << "rewritten machine operands; deleted-moves=" << rewritten->deleted_moves ()
              << " spill-area-bytes=" << rewritten->allocation ().stack_bytes () << '\n';
    auto spills = mirnext::codegen::split_allocated_spills (**machine, std::move (*rewritten));
    if (!spills) {
      std::cerr << spills.error ().message << '\n';
      return 1;
    }
    std::cout << "edge-preservations=" << spills->placements ().size ()
              << " spill-area-bytes=" << spills->allocation ().stack_bytes () << '\n';
    auto post
      = mirnext::codegen::optimize_allocated_machine (**machine, std::move (*spills),
                                                      {.observer = [] (auto stage, const auto &view)
                                                         -> mirnext::Result<void> {
                                                        std::cout
                                                          << mirnext::codegen::stage_name (stage)
                                                          << " instructions="
                                                          << view.function ().instruction_count ()
                                                          << '\n';
                                                        return {};
                                                      }});
    if (!post) {
      std::cerr << post.error ().message << '\n';
      return 1;
    }
    std::cout << "combined-deletions=" << post->statistics ().deleted
              << " dead-definitions=" << post->statistics ().dead_instructions << '\n';
    auto frame = mirnext::codegen::form_frame (**machine, std::move (*post));
    if (!frame) {
      std::cerr << frame.error ().message << '\n';
      return 1;
    }
    std::cout << "frame-bytes=" << frame->layout ().frame_bytes
              << " aggregate-bytes=" << frame->layout ().aggregate_bytes
              << " callee-saves=" << frame->layout ().callee_saves.size () << '\n'
              << (*machine)->dump ();
    auto compiled = mirnext::codegen::encode_machine (**machine, *frame);
    if (!compiled) {
      std::cerr << compiled.error ().message << '\n';
      return 1;
    }
    std::cout << "code-bytes=" << compiled->bytes ().size ()
              << " relocations=" << compiled->relocations ().size () << '\n';
  }
  std::cout << "machine CFG blocks=" << graph->cfg ().blocks.size ()
            << " edges=" << graph->cfg ().edges.size () << " loops=" << graph->has_loops ()
            << " live-registers=" << live->registers ().size ()
            << " live-range-points=" << ranges->point_count () << '\n';
}
