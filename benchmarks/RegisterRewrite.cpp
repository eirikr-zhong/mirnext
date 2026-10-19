/* Licensed under the MIT License. Rewrite timing excludes IR and allocation. */
#include "mirnext/CodeGen/RegisterRewrite.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main() {
  using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
  auto t = target::TargetMachine::native(); if (!t || t->architecture() != target::Architecture::AArch64) return 1;
  for (unsigned count : {4096, 16384, 32768}) {
    std::vector<double> times; std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      MachineFunction f("benchmark", "rewrite", {{}, {}}, *t); MachineRewriter rw(f); auto &b = f.create_block();
      auto r = f.create_register(Type::i64()); if (!r) return 2;
      for (unsigned n = 0; n < count; ++n) if (!rw.append(b, Opcode::Mov, {MachineOperand(**r), MachineOperand(**r)})) return 3;
      if (!rw.append(b, Opcode::Ret, {})) return 4;
      auto state = f.state(); state.machinized = state.operands_legalized = true; if (!rw.set_state(state)) return 5;
      auto g = MachineGraphAnalysis::build(f); if (!g) return 6;
      auto l = MachineLiveness::analyze(f, *g); if (!l) return 7;
      auto ranges = MachineLiveRanges::build(f, *g, *l); if (!ranges) return 8;
      auto a = MachineAllocation::assign(f, *g, *l, *ranges); if (!a) return 9;
      auto start = std::chrono::steady_clock::now(); auto result = rewrite_allocated(f, *g, *l, *ranges, std::move(*a)); auto end = std::chrono::steady_clock::now();
      if (!result) { std::cerr << result.error().message << '\n'; return 10; }
      if (result->deleted_moves() != count || f.instruction_count() != 1 || !f.verify()) return 11;
      auto hash = std::uint64_t(14695981039346656037ULL);
      auto mix = [&](std::uint64_t v) { hash = (hash ^ v) * 1099511628211ULL; };
      mix(result->deleted_moves()); mix(f.instruction_count()); mix(result->allocation().stack_bytes());
      for (auto &block : f.blocks()) for (auto &i : block.instructions()) { mix(i.creation_order()); mix(i.operands().size()); }
      for (auto p : result->allocation().used_registers()) mix(p.encoding());
      if (rep && hash != fingerprint) return 12; fingerprint = hash;
      times.push_back(std::chrono::duration<double, std::milli>(end-start).count());
    }
    std::ranges::sort(times); std::cout << "moves=" << count << " median_ms=" << times[times.size()/2] << " fingerprint=" << fingerprint << '\n';
  }
}
