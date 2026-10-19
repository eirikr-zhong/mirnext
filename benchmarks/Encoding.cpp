/* Licensed under the MIT License. Checked encoding timing excludes prior phases. */
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = mirnext::target::TargetMachine::native ();
  if (!target || target->architecture () != mirnext::target::Architecture::AArch64) return 1;
  for (unsigned count : {4096, 16384, 65536}) {
    MachineFunction f ("benchmark", "encoding", {{Type::i64 ()}, {}}, *target);
    MachineRewriter rw (f);
    auto &b = f.create_block ();
    auto r0 = MachineOperand (target->registers ()[0].reg),
         r1 = MachineOperand (target->registers ()[1].reg);
    for (unsigned n = 0; n < count; ++n) {
      auto value = MachineOperand (std::uint64_t (n % 2 ? 0x123456789abcdef0ull : n & 4095));
      auto r = rw.append (b, n % 2 ? Opcode::Mov : Opcode::Add,
                          n % 2 ? std::vector{r0, value} : std::vector{r0, r1, value});
      if (!r) return 2;
    }
    if (!rw.append (b, Opcode::Ret, {r0})) return 3;
    auto state = f.state ();
    state.machinized = state.operands_legalized = true;
    if (!rw.set_state (state)) return 4;
    auto graph = MachineGraphAnalysis::build (f);
    if (!graph) return 5;
    auto live = MachineLiveness::analyze (f, *graph, {.optimization_level = 0});
    if (!live) return 6;
    auto ranges = MachineLiveRanges::build (f, *graph, *live);
    if (!ranges) return 7;
    auto allocation = MachineAllocation::assign (f, *graph, *live, *ranges);
    if (!allocation) return 8;
    auto rewritten = rewrite_allocated (f, *graph, *live, *ranges, std::move (*allocation));
    if (!rewritten) return 9;
    auto spills = split_allocated_spills (f, std::move (*rewritten));
    if (!spills) return 10;
    auto post = optimize_allocated_machine (f, std::move (*spills));
    if (!post) return 11;
    auto frame = form_frame (f, std::move (*post));
    if (!frame) return 12;
    std::vector<double> native_times;
    std::uint64_t fingerprint = 0;
    std::size_t size = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      auto encoded = encode_machine (f, *frame);
      auto end = std::chrono::steady_clock::now ();
      if (!encoded) {
        std::cerr << encoded.error ().message << '\n';
        return 13;
      }
      native_times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
      size = encoded->bytes ().size ();
      std::uint64_t hash = 14695981039346656037ull;
      for (auto byte : encoded->bytes ()) hash = (hash ^ byte) * 1099511628211ull;
      if (rep && hash != fingerprint) return 14;
      fingerprint = hash;
    }
    std::ranges::sort (native_times);
    std::cout << "instructions=" << count + 1 << " bytes=" << size
              << " native_ms=" << native_times[3];
    std::cout << " fingerprint=" << fingerprint << '\n';
  }
}
