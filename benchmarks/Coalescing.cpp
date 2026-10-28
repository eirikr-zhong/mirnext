/* Licensed under the MIT License. Owning coalescing excludes construction/CFG. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return 1;
  for (unsigned count : {256, 1024, 4096}) {
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f ("benchmark", "coalescing", {{Type::i64 ()}, {}}, (target).value ());
      auto &b = mirnext::codegen::detail::access (f).create_block ();
      MachineRewriter rw (f);
      auto a = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
      if (!(a).has_value ()) return 1;
      auto previous = &(a).value ();
      if (!(rw.append (b, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((a).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          std::int64_t (42))}))
             .has_value ())
        return 2;
      for (unsigned n = 0; n < count; ++n) {
        auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
        if (!(r).has_value ()) return 3;
        if (!(rw.append (b, Opcode::Mov,
                         {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
                          mirnext::codegen::detail::NodeAccess::make_MachineOperand (*previous)}))
               .has_value ())
          return 4;
        previous = &(r).value ();
      }
      if (!(rw.append (b, Opcode::Ret,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*previous)}))
             .has_value ())
        return 5;
      auto graph = MachineGraphAnalysis::build (f);
      if (!(graph).has_value ()) return 6;
      auto start = std::chrono::steady_clock::now ();
      auto merged = coalesce_registers (f, (graph).value ());
      auto end = std::chrono::steady_clock::now ();
      if (!(merged).has_value ()) {
        std::cerr << merged.error ().message << '\n';
        return 7;
      }
      if (merged.value ().coalesced_moves () != count || f.instruction_count () != 2
          || !(f.verify ()).has_value ())
        return 8;
      std::uint64_t hash = 14695981039346656037ULL;
      for (auto r : merged.value ().registers ()) {
        auto value = merged.value ().representative (
          mirnext::codegen::detail::NodeAccess::make_MachineRegister (r));
        if (!(value).has_value ()) return 9;
        hash = (hash
                ^ ((value).value ()
                   == mirnext::codegen::detail::NodeAccess::make_MachineRegister (&(a).value ())))
               * 1099511628211ULL;
      }
      if (rep && hash != fingerprint) return 10;
      fingerprint = hash;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "moves=" << count << " median_ms=" << times[times.size () / 2]
              << " fingerprint=" << fingerprint << '\n';
  }
}
