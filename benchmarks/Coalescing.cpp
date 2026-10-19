/* Licensed under the MIT License. Owning coalescing excludes construction/CFG. */
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main() {
  using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
  auto target = target::TargetMachine::native(); if (!target) return 1;
  for (unsigned count : {256, 1024, 4096}) {
    std::vector<double> times; std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f("benchmark", "coalescing", {{Type::i64()}, {}}, *target); auto &b = f.create_block(); MachineRewriter rw(f);
      auto a = f.create_register(Type::i64()); if (!a) return 1; auto previous = *a;
      if (!rw.append(b, Opcode::Mov, {MachineOperand(**a), MachineOperand(std::int64_t(42))})) return 2;
      for (unsigned n = 0; n < count; ++n) {
        auto r = f.create_register(Type::i64()); if (!r) return 3;
        if (!rw.append(b, Opcode::Mov, {MachineOperand(**r), MachineOperand(*previous)})) return 4; previous = *r;
      }
      if (!rw.append(b, Opcode::Ret, {MachineOperand(*previous)})) return 5;
      auto graph = MachineGraphAnalysis::build(f); if (!graph) return 6;
      auto start = std::chrono::steady_clock::now(); auto merged = coalesce_registers(f, *graph); auto end = std::chrono::steady_clock::now();
      if (!merged) { std::cerr << merged.error().message << '\n'; return 7; }
      if (merged->coalesced_moves() != count || f.instruction_count() != 2 || !f.verify()) return 8;
      std::uint64_t hash = 14695981039346656037ULL;
      for (auto r : merged->registers()) { auto value = merged->representative(r); if (!value) return 9; hash = (hash ^ (*value == MachineRegister(*a))) * 1099511628211ULL; }
      if (rep && hash != fingerprint) return 10; fingerprint = hash;
      times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(times); std::cout << "moves=" << count << " median_ms=" << times[times.size()/2] << " fingerprint=" << fingerprint << '\n';
  }
}
