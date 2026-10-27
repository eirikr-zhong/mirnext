/* Licensed under the MIT License. Post-allocation timing excludes prior stages. */
#include "mirnext/CodeGen/PostAllocation.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!target || target->architecture () != target::Architecture::AArch64) return 1;
  for (unsigned count : {2048, 8192, 16384}) {
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f ("benchmark", "post", {{Type::i64 ()}, {}}, *target);
      MachineRewriter rw (f);
      auto &b = f.create_block ();
      auto reg = [&] (unsigned n) { return MachineOperand (target->registers ()[n].reg); };
      // Dead overflow producers followed by an unchanged, observable MOV suffix.
      // Each original forward flag search traverses the same surviving suffix.
      for (unsigned n = 0; n < count; ++n)
        if (!rw.append (b, Opcode::Addo, {reg (0), reg (1), reg (2)})) return 2;
      for (unsigned n = 0; n < count; ++n)
        if (!rw.append (b, Opcode::Mov, {reg (29), reg (3)})) return 3;
      if (!rw.append (b, Opcode::Ret, {reg (7)})) return 4;
      auto state = f.state ();
      state.machinized = state.operands_legalized = true;
      if (!rw.set_state (state)) return 5;
      auto graph = MachineGraphAnalysis::build (f);
      if (!graph) return 6;
      auto live = MachineLiveness::analyze (f, *graph);
      if (!live) return 7;
      auto ranges = MachineLiveRanges::build (f, *graph, *live);
      if (!ranges) return 8;
      auto allocation = MachineAllocation::assign (f, *graph, *live, *ranges);
      if (!allocation) return 9;
      auto rewritten = rewrite_allocated (f, *graph, *live, *ranges, std::move (*allocation));
      if (!rewritten) return 10;
      auto spill = split_allocated_spills (f, std::move (*rewritten));
      if (!spill) return 11;
      auto start = std::chrono::steady_clock::now ();
      auto post = optimize_allocated_machine (f, std::move (*spill));
      auto end = std::chrono::steady_clock::now ();
      if (!post) {
        std::cerr << post.error ().message << '\n';
        return 12;
      }
      if (!post->current (f) || !post->cfg ().verify (f) || f.instruction_count () != count + 1
          || post->statistics ().dead_instructions != count || post->statistics ().substitutions)
        return 13;
      auto hash = std::uint64_t (14695981039346656037ULL);
      auto mix = [&] (std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
      mix (f.instruction_count ());
      mix (post->statistics ().dead_instructions);
      mix (post->allocation ().stack_bytes ());
      for (auto &block : f.blocks ())
        for (auto &i : block.instructions ()) {
          mix (i.creation_order ());
          for (auto &op : i.operands ()) mix (op.get<target::PhysicalRegister> ()->encoding ());
        }
      if (rep && hash != fingerprint) return 14;
      fingerprint = hash;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "producers=" << count << " suffix=" << count
              << " median_ms=" << times[times.size () / 2] << " fingerprint=" << fingerprint
              << '\n';
  }
}
