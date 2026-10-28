/* Licensed under the MIT License. Checked encoding timing excludes prior phases. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = mirnext::target::TargetMachine::native ();
  if (!(target).has_value ()
      || target.value ().architecture () != mirnext::target::Architecture::AArch64)
    return 1;
  for (unsigned count : {4096, 16384, 65536}) {
    MachineFunction f ("benchmark", "encoding", {{Type::i64 ()}, {}}, (target).value ());
    MachineRewriter rw (f);
    auto &b = mirnext::codegen::detail::access (f).create_block ();
    auto r0 = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
           target.value ().registers ()[0].reg),
         r1 = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
           target.value ().registers ()[1].reg);
    for (unsigned n = 0; n < count; ++n) {
      auto value = mirnext::codegen::detail::NodeAccess::make_MachineOperand (
        std::uint64_t (n % 2 ? 0x123456789abcdef0ull : n & 4095));
      auto r = rw.append (b, n % 2 ? Opcode::Mov : Opcode::Add,
                          n % 2 ? std::vector{r0, value} : std::vector{r0, r1, value});
      if (!(r).has_value ()) return 2;
    }
    if (!(rw.append (b, Opcode::Ret, {r0})).has_value ()) return 3;
    auto state = f.state ();
    state.machinized = state.operands_legalized = true;
    if (!(rw.set_state (state)).has_value ()) return 4;
    auto graph = MachineGraphAnalysis::build (f);
    if (!(graph).has_value ()) return 5;
    auto live = MachineLiveness::analyze (f, (graph).value (), {.optimization_level = 0});
    if (!(live).has_value ()) return 6;
    auto ranges = MachineLiveRanges::build (f, (graph).value (), (live).value ());
    if (!(ranges).has_value ()) return 7;
    auto allocation
      = MachineAllocation::assign (f, (graph).value (), (live).value (), (ranges).value ());
    if (!(allocation).has_value ()) return 8;
    auto rewritten = rewrite_allocated (f, (graph).value (), (live).value (), (ranges).value (),
                                        std::move ((allocation).value ()));
    if (!(rewritten).has_value ()) return 9;
    auto spills = split_allocated_spills (f, std::move ((rewritten).value ()));
    if (!(spills).has_value ()) return 10;
    auto post = optimize_allocated_machine (f, std::move ((spills).value ()));
    if (!(post).has_value ()) return 11;
    auto frame = form_frame (f, std::move ((post).value ()));
    if (!(frame).has_value ()) return 12;
    std::vector<double> native_times;
    std::uint64_t fingerprint = 0;
    std::size_t size = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      auto encoded = encode_machine (f, (frame).value ());
      auto end = std::chrono::steady_clock::now ();
      if (!(encoded).has_value ()) {
        std::cerr << encoded.error ().message << '\n';
        return 13;
      }
      native_times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
      size = encoded.value ().bytes ().size ();
      std::uint64_t hash = 14695981039346656037ull;
      for (auto byte : encoded.value ().bytes ()) hash = (hash ^ byte) * 1099511628211ull;
      if (rep && hash != fingerprint) return 14;
      fingerprint = hash;
    }
    std::ranges::sort (native_times);
    std::cout << "instructions=" << count + 1 << " bytes=" << size
              << " native_ms=" << native_times[3];
    std::cout << " fingerprint=" << fingerprint << '\n';
  }
}
