/* Licensed under the MIT License. Sparse death metadata in native machine liveness. */
#include "mirnext/CodeGen/Analysis.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
namespace { bool measuring = false; std::size_t allocations = 0; }
void *operator new(std::size_t size) {
  if (measuring) ++allocations;
  if (auto p = std::malloc(size ? size : 1)) return p;
  std::abort();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
int main() {
  using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
  auto target = target::TargetMachine::native(); if (!target) return 1;
  for (unsigned count : {32768, 131072, 524288}) {
    MachineFunction f("machine-analysis", "repeated-definitions", {{Type::i64()}, {}}, *target);
    auto &b = f.create_block(); auto x = f.create_register(Type::i64()); if (!x) return 1; MachineRewriter rewrite(f);
    for (unsigned n = 0; n < count; ++n) if (!rewrite.append(b, Opcode::Mov, {MachineOperand(**x), MachineOperand(std::int64_t(n))})) return 1;
    if (!rewrite.append(b, Opcode::Ret, {MachineOperand(**x)})) return 1;
    auto graph = MachineGraphAnalysis::build(f); if (!graph) return 1;
    std::vector<double> times; std::vector<std::size_t> counts; std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      allocations = 0; measuring = true; auto start = std::chrono::steady_clock::now();
      auto live = MachineLiveness::analyze(f, *graph); auto end = std::chrono::steady_clock::now(); measuring = false;
      if (!live) return 1; counts.push_back(allocations);
      std::uint64_t hash = 14695981039346656037ULL;
      auto mix = [&](std::uint64_t value) { hash = (hash ^ value) * 1099511628211ULL; };
      for (auto r : live->registers()) { auto frequency = live->frequency(r); if (!frequency) return 1; mix(*frequency); }
      for (auto &bb : live->blocks()) {
        const SparseBitSet *sets[] = {&bb.in, &bb.out, &bb.gen, &bb.kill};
        for (auto set : sets) { mix(UINT64_MAX); set->for_each([&](auto bit) { mix(bit); }); }
        mix(bb.max_integer_pressure); mix(bb.max_floating_pressure);
      }
      for (auto &insn : b.instructions()) { auto dead = live->deaths(insn); if (!dead) return 1; mix(dead->size()); }
      if (rep && hash != fingerprint) return 2; fingerprint = hash;
      times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(times); std::ranges::sort(counts);
    std::cout << "instructions=" << count + 1 << " median_ms=" << times[3] << " allocations=" << counts[3] << " fingerprint=" << fingerprint << '\n';
  }
}
