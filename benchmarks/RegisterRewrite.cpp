/* Licensed under the MIT License. Rewrite timing excludes IR and allocation. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto t = target::TargetMachine::native ();
  if (!(t).has_value () || t.value ().architecture () != target::Architecture::AArch64) return 1;
  for (unsigned count : {4096, 16384, 32768}) {
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f ("benchmark", "rewrite", {{}, {}}, (t).value ());
      MachineRewriter rw (f);
      auto &b = mirnext::codegen::detail::access (f).create_block ();
      auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
      if (!(r).has_value ()) return 2;
      for (unsigned n = 0; n < count; ++n)
        if (!(rw.append (b, Opcode::Mov,
                         {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
                          mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                            (r).value ())}))
               .has_value ())
          return 3;
      if (!(rw.append (b, Opcode::Ret, {})).has_value ()) return 4;
      auto state = f.state ();
      state.machinized = state.operands_legalized = true;
      if (!(rw.set_state (state)).has_value ()) return 5;
      auto g = MachineGraphAnalysis::build (f);
      if (!(g).has_value ()) return 6;
      auto l = MachineLiveness::analyze (f, (g).value ());
      if (!(l).has_value ()) return 7;
      auto ranges = MachineLiveRanges::build (f, (g).value (), (l).value ());
      if (!(ranges).has_value ()) return 8;
      auto a = MachineAllocation::assign (f, (g).value (), (l).value (), (ranges).value ());
      if (!(a).has_value ()) return 9;
      auto start = std::chrono::steady_clock::now ();
      auto result = rewrite_allocated (f, (g).value (), (l).value (), (ranges).value (),
                                       std::move ((a).value ()));
      auto end = std::chrono::steady_clock::now ();
      if (!(result).has_value ()) {
        std::cerr << result.error ().message << '\n';
        return 10;
      }
      if (result.value ().deleted_moves () != count || f.instruction_count () != 1
          || !(f.verify ()).has_value ())
        return 11;
      auto hash = std::uint64_t (14695981039346656037ULL);
      auto mix = [&] (std::uint64_t v) { hash = (hash ^ v) * 1099511628211ULL; };
      mix (result.value ().deleted_moves ());
      mix (f.instruction_count ());
      mix (result.value ().allocation ().stack_bytes ());
      for (auto &block : f.blocks ())
        for (auto &i : block.instructions ()) {
          mix (i.creation_order ());
          mix (i.operands ().size ());
        }
      for (auto p : result.value ().allocation ().used_registers ()) mix (p.encoding ());
      if (rep && hash != fingerprint) return 12;
      fingerprint = hash;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "moves=" << count << " median_ms=" << times[times.size () / 2]
              << " fingerprint=" << fingerprint << '\n';
  }
}
