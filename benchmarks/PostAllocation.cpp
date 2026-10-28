/* Licensed under the MIT License. Post-allocation timing excludes prior stages. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/CodeGen/PostAllocation.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!(target).has_value () || target.value ().architecture () != target::Architecture::AArch64)
    return 1;
  for (unsigned count : {2048, 8192, 16384}) {
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f ("benchmark", "post", {{Type::i64 ()}, {}}, (target).value ());
      MachineRewriter rw (f);
      auto &b = mirnext::codegen::detail::access (f).create_block ();
      auto reg = [&] (unsigned n) {
        return mirnext::codegen::detail::NodeAccess::make_MachineOperand (
          target.value ().registers ()[n].reg);
      };
      // Dead overflow producers followed by an unchanged, observable MOV suffix.
      // Each original forward flag search traverses the same surviving suffix.
      for (unsigned n = 0; n < count; ++n)
        if (!(rw.append (b, Opcode::Addo, {reg (0), reg (1), reg (2)})).has_value ()) return 2;
      for (unsigned n = 0; n < count; ++n)
        if (!(rw.append (b, Opcode::Mov, {reg (29), reg (3)})).has_value ()) return 3;
      if (!(rw.append (b, Opcode::Ret, {reg (7)})).has_value ()) return 4;
      auto state = f.state ();
      state.machinized = state.operands_legalized = true;
      if (!(rw.set_state (state)).has_value ()) return 5;
      auto graph = MachineGraphAnalysis::build (f);
      if (!(graph).has_value ()) return 6;
      auto live = MachineLiveness::analyze (f, (graph).value ());
      if (!(live).has_value ()) return 7;
      auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
      if (!(ranges).has_value ()) return 8;
      auto allocation
        = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
      if (!(allocation).has_value ()) return 9;
      auto rewritten = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                          std::move ((allocation).value ()));
      if (!(rewritten).has_value ()) return 10;
      auto spill = split_allocated_spills (f, std::move ((rewritten).value ()));
      if (!(spill).has_value ()) return 11;
      auto start = std::chrono::steady_clock::now ();
      auto post = optimize_allocated_machine (f, std::move ((spill).value ()));
      auto end = std::chrono::steady_clock::now ();
      if (!(post).has_value ()) {
        std::cerr << post.error ().message << '\n';
        return 12;
      }
      if (!post.value ().current (f) || !(post.value ().cfg ().verify (f)).has_value ()
          || f.instruction_count () != count + 1
          || post.value ().statistics ().dead_instructions != count
          || post.value ().statistics ().substitutions)
        return 13;
      auto hash = std::uint64_t (14695981039346656037ULL);
      auto mix = [&] (std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
      mix (f.instruction_count ());
      mix (post.value ().statistics ().dead_instructions);
      mix (post.value ().allocation ().stack_bytes ());
      for (auto &block : f.blocks ())
        for (auto &i : block.instructions ()) {
          mix (i.creation_order ());
          for (auto &op : i.operands ())
            mix (mirnext::codegen::detail::operand_access (op)
                   .get<target::PhysicalRegister> ()
                   ->encoding ());
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
