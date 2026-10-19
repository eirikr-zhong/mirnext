/* Licensed under the MIT License. Allocation timing excludes IR and analyses. */
#include "mirnext/CodeGen/RegisterAllocation.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main() {
  using namespace mirnext; using namespace mirnext::ir; using namespace mirnext::codegen;
  auto target = target::TargetMachine::native(); if (!target) return 1;
  for (unsigned count : {128, 512, 1024}) {
    MachineFunction f("benchmark", "assignment", {{}, {}}, *target); MachineRewriter rw(f);
    std::vector<target::PhysicalRegister> globals;
    for (auto &p : target->registers()) if (!p.reserved && p.reg.encoding() >= 4) globals.push_back(p.reg);
    if (!rw.set_global_registers(globals)) return 2;
    auto &entry = f.create_block(); std::vector<const MachineVirtualRegister *> outer, inner;
    for (unsigned n = 0; n < 4; ++n) {
      auto r = f.create_register(Type::i64()); if (!r) return 3; outer.push_back(*r);
      if (!rw.append(entry, Opcode::Mov, {MachineOperand(**r), MachineOperand(std::int64_t(42))})) return 4;
      if (!rw.append(entry, MachinePseudoOpcode::Use, std::vector<MachineOperand>(256, MachineOperand(**r)))) return 5;
    }
    for (unsigned n = 0; n < count; ++n) { auto r = f.create_register(Type::i64()); if (!r) return 6; inner.push_back(*r); }
    for (unsigned b = 0; b < 64; ++b) {
      auto &block = f.create_block(); if (!f.create_label(block)) return 7;
      for (auto r : inner) {
        if (!b && !rw.append(block, Opcode::Mov, {MachineOperand(*r), MachineOperand(std::int64_t(7))})) return 8;
        if (!rw.append(block, MachinePseudoOpcode::Use, {MachineOperand(*r)})) return 9;
      }
    }
    auto &exit = f.create_block(); if (!f.create_label(exit)) return 10;
    for (auto r : outer) if (!rw.append(exit, MachinePseudoOpcode::Use, {MachineOperand(*r)})) return 11;
    if (!rw.append(exit, Opcode::Ret, {})) return 12;
    auto graph = MachineGraphAnalysis::build(f); if (!graph) return 13;
    auto live = MachineLiveness::analyze(f, *graph); if (!live) return 14;
    auto ranges = MachineLiveRanges::build(f, *graph, *live); if (!ranges) return 15;
    std::vector<double> times; std::uint64_t fingerprint = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now(); auto result = MachineAllocation::assign(f, *graph, *live, *ranges); auto end = std::chrono::steady_clock::now();
      if (!result) { std::cerr << result.error().message << '\n'; return 16; }
      auto hash = std::uint64_t(14695981039346656037ULL);
      auto mix = [&](std::size_t v) { hash = (hash ^ v) * 1099511628211ULL; };
      mix(result->stack_bytes()); mix(result->gaps().size());
      for (auto &r : f.registers()) {
        auto loc = result->location(r); if (!loc) return 17;
        mix(loc->index());
        if (auto p = std::get_if<target::PhysicalRegister>(&*loc)) mix(p->encoding());
        else if (auto s = std::get_if<StackLocation>(&*loc)) { mix(s->offset); mix(s->size); }
      }
      for (auto &b : result->spill_blocks()) { b.gen.for_each(mix); mix(SIZE_MAX); b.kill.for_each(mix); }
      for (auto &p : result->used()) { p.physical.for_each(mix); mix(SIZE_MAX); p.stack.for_each(mix); }
      if (rep && hash != fingerprint) return 18; fingerprint = hash;
      times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(times); std::cout << "registers=" << count << " median_ms=" << times[times.size()/2] << " fingerprint=" << fingerprint << '\n';
  }
}
