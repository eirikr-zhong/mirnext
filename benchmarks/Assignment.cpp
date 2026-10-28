/* Licensed under the MIT License. Allocation timing excludes IR and analyses. */
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
  for (unsigned count : {128, 512, 1024}) {
    MachineFunction f ("benchmark", "assignment", {{}, {}}, (target).value ());
    MachineRewriter rw (f);
    std::vector<target::PhysicalRegister> globals;
    for (auto &p : target.value ().registers ())
      if (!p.reserved && p.reg.encoding () >= 4) globals.push_back (p.reg);
    if (!(rw.set_global_registers (globals)).has_value ()) return 2;
    auto &entry = mirnext::codegen::detail::access (f).create_block ();
    std::vector<const MachineVirtualRegister *> outer, inner;
    for (unsigned n = 0; n < 4; ++n) {
      auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
      if (!(r).has_value ()) return 3;
      outer.push_back (&(r).value ());
      if (!(rw.append (entry, Opcode::Mov,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((r).value ()),
                        mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                          std::int64_t (42))}))
             .has_value ())
        return 4;
      if (!(rw.append (entry, MachinePseudoOpcode::Use,
                       std::vector<MachineOperand> (256, mirnext::codegen::detail::NodeAccess::
                                                           make_MachineOperand ((r).value ()))))
             .has_value ())
        return 5;
    }
    for (unsigned n = 0; n < count; ++n) {
      auto r = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
      if (!(r).has_value ()) return 6;
      inner.push_back (&(r).value ());
    }
    for (unsigned b = 0; b < 64; ++b) {
      auto &block = mirnext::codegen::detail::access (f).create_block ();
      if (!(mirnext::codegen::detail::access (f).create_label (block)).has_value ()) return 7;
      for (auto r : inner) {
        if (!b
            && !(rw.append (block, Opcode::Mov,
                            {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*r),
                             mirnext::codegen::detail::NodeAccess::make_MachineOperand (
                               std::int64_t (7))}))
                  .has_value ())
          return 8;
        if (!(rw.append (block, MachinePseudoOpcode::Use,
                         {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*r)}))
               .has_value ())
          return 9;
      }
    }
    auto &exit = mirnext::codegen::detail::access (f).create_block ();
    if (!(mirnext::codegen::detail::access (f).create_label (exit)).has_value ()) return 10;
    for (auto r : outer)
      if (!(rw.append (exit, MachinePseudoOpcode::Use,
                       {mirnext::codegen::detail::NodeAccess::make_MachineOperand (*r)}))
             .has_value ())
        return 11;
    if (!(rw.append (exit, Opcode::Ret, {})).has_value ()) return 12;
    auto graph = MachineGraphAnalysis::build (f);
    if (!(graph).has_value ()) return 13;
    auto live = MachineLiveness::analyze (f, (graph).value ());
    if (!(live).has_value ()) return 14;
    auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
    if (!(ranges).has_value ()) return 15;
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      auto result
        = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
      auto end = std::chrono::steady_clock::now ();
      if (!(result).has_value ()) {
        std::cerr << result.error ().message << '\n';
        return 16;
      }
      auto hash = std::uint64_t (14695981039346656037ULL);
      auto mix = [&] (std::size_t v) { hash = (hash ^ v) * 1099511628211ULL; };
      mix (result.value ().stack_bytes ());
      mix (result.value ().gaps ().size ());
      for (auto &r : f.registers ()) {
        auto loc = result.value ().location (r);
        if (!(loc).has_value ()) return 17;
        mix (loc.value ().index ());
        if (auto p = std::get_if<target::PhysicalRegister> (
              mirnext::codegen::detail::NodeAccess::storage (&(loc).value ())))
          mix (p->encoding ());
        else if (auto s = std::get_if<StackLocation> (&(loc).value ())) {
          mix (s->offset);
          mix (s->size);
        }
      }
      for (auto &b : result.value ().spill_blocks ()) {
        b.gen.for_each (mix);
        mix (SIZE_MAX);
        b.kill.for_each (mix);
      }
      for (auto &p : result.value ().used ()) {
        p.physical.for_each (mix);
        mix (SIZE_MAX);
        p.stack.for_each (mix);
      }
      if (rep && hash != fingerprint) return 18;
      fingerprint = hash;
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "registers=" << count << " median_ms=" << times[times.size () / 2]
              << " fingerprint=" << fingerprint << '\n';
  }
}
