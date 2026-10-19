/* Licensed under the MIT License. MIR register-liveness transfer functions.
   Reference algorithms Copyright (C) 2018-2024 Vladimir Makarov. */
#include "mirnext/CodeGen/Analysis.hpp"
#include "mirnext/CodeGen/Detail/RegisterEffects.hpp"
#include <algorithm>
#include <climits>
#include <numeric>

namespace mirnext::codegen {
namespace {
Error invalid(const MachineFunction &f, std::string why) {
  return {ErrorCode::InvalidOperand, std::string(f.module_name()) + "/" + std::string(f.name())
    + " [" + std::string(f.target().name()) + ", machine-liveness]: " + std::move(why)};
}
using detail::is_call;
using detail::visit_registers;
bool integer(const target::TargetMachine &target, MachineRegister reg) {
  if (auto v = std::get_if<const MachineVirtualRegister *>(&reg)) return (*v)->type().is_integer();
  return target.register_info(std::get<target::PhysicalRegister>(reg))->bank == target::RegisterBank::Integer;
}
}
Result<MachineRegisterEffects> machine_register_effects(const MachineFunction &f, const MachineInstruction &i) {
  if (&i.block().function() != &f) return invalid(f, "foreign instruction");
  MachineRegisterEffects result;
  visit_registers(i, true, [&](auto r) { result.definitions.push_back(r); });
  visit_registers(i, false, [&](auto r) { result.uses.push_back(r); }); return result;
}
std::array<std::optional<target::PhysicalRegister>, 2> early_clobbers(const target::TargetMachine &target, MachineOpcode code) {
  std::array<std::optional<target::PhysicalRegister>, 2> result;
  auto op = std::get_if<ir::Opcode>(&code); if (!op) return result;
  using O = ir::Opcode;
  auto physical = [&](unsigned r) { return target.registers()[r].reg; };
  if (target.architecture() == target::Architecture::AArch64) {
    if (*op == O::Mod || *op == O::Mods || *op == O::UMod || *op == O::UMods) result[0] = physical(8);
    else if (*op == O::Mulo || *op == O::UMulo) result[0] = physical(11);
  } else {
    switch (*op) {
    case O::Div: case O::UDiv: case O::Divs: case O::UDivs: case O::Mod: case O::UMod:
    case O::Mods: case O::UMods: case O::UMulo: case O::UMulos: result[0] = physical(2); break;
    case O::FEq: case O::FNe: case O::DEq: case O::DNe: case O::LDEq: case O::LDNe:
      result = {physical(0), physical(2)}; break;
    case O::FLt: case O::FLe: case O::FGt: case O::FGe: case O::DLt: case O::DLe: case O::DGt:
    case O::DGe: case O::LDLt: case O::LDLe: case O::LDGt: case O::LDGe: result[0] = physical(0); break;
    default: break;
    }
  } return result;
}
Result<std::size_t> MachineLiveness::index(MachineRegister reg) const {
  if (!current(*owner_)) return invalid(*owner_, "stale analysis");
  if (auto v = std::get_if<const MachineVirtualRegister *>(&reg)) {
    if (auto at = virtuals_.find(*v); at != virtuals_.end()) return at->second;
  } else {
    auto p = std::get<target::PhysicalRegister>(reg);
    if (owner_->target().register_info(p)) if (auto at = physicals_.find(p.encoding()); at != physicals_.end()) return at->second;
  }
  return invalid(*owner_, "register is foreign or outside the selected scan");
}
Result<bool> MachineLiveness::contains(const SparseBitSet &set, MachineRegister reg) const {
  auto p = index(reg); if (!p) return p.error(); return set.contains(*p);
}
Result<long> MachineLiveness::frequency(MachineRegister reg) const {
  auto p = index(reg); if (!p) return p.error(); return frequencies_[*p];
}
Result<std::span<const MachineRegister>> MachineLiveness::deaths(const MachineInstruction &i) const {
  if (!current(i.block().function())) return invalid(*owner_, "stale analysis or foreign instruction");
  if (auto at = deaths_.find(&i); at != deaths_.end()) return std::span<const MachineRegister>(at->second);
  if (has_dead_info_) return std::span<const MachineRegister>{};
  return invalid(*owner_, "dead-variable metadata requires O1 or higher");
}
Result<MachineLiveness> MachineLiveness::analyze(const MachineFunction &f, const MachineGraphAnalysis &graph, MachineLivenessOptions options) {
  if (options.optimization_level > 3 || !graph.current(f)) return invalid(f, "invalid optimization level or stale/foreign CFG");
  return analyze_retained_topology(f, graph, options);
}
Result<MachineLiveness> MachineLiveness::analyze_retained_topology(const MachineFunction &f, const MachineGraphAnalysis &graph, MachineLivenessOptions options) {
  MachineLiveness result; result.owner_ = &f; result.revision_ = f.revision();
  result.level_ = options.optimization_level; result.compact_ = !options.scan_registers.empty();
  result.has_frequencies_ = options.calculate_frequencies;
  std::vector<MachineRegister> all;
  for (auto &r : f.target().registers()) all.emplace_back(r.reg);
  for (auto &r : f.registers()) all.emplace_back(&r);
  auto add = [&](MachineRegister r) -> Result<void> {
    auto p = result.registers_.size();
    if (auto v = std::get_if<const MachineVirtualRegister *>(&r)) {
      if (!*v || &(*v)->function() != &f || !result.virtuals_.emplace(*v, p).second) return invalid(f, "foreign or repeated scan register");
    } else {
      auto physical = std::get<target::PhysicalRegister>(r);
      if (!f.target().register_info(physical) || !result.physicals_.emplace(physical.encoding(), p).second) return invalid(f, "foreign or repeated physical scan register");
    }
    result.registers_.push_back(r); return {};
  };
  for (auto r : options.scan_registers.empty() ? std::span<const MachineRegister>(all) : options.scan_registers) {
    auto added = add(r); if (!added) return added.error();
  }
  const bool compact = !options.scan_registers.empty(), advanced = options.optimization_level != 0;
  result.has_dead_info_ = advanced;
  const auto &cfg = graph.cfg(); result.blocks_.resize(cfg.blocks.size()); result.frequencies_.resize(result.registers_.size());
  SparseBitSet clobbered, globals;
  for (auto r : f.global_registers()) if (!compact) globals.insert(*result.index(r));
  for (auto r : options.global_registers) {
    if (!f.target().register_info(r)) return invalid(f, "foreign global hardware register");
    if (!compact) globals.insert(*result.index(r));
  }
  globals.for_each([&](auto r) { result.globals_.push_back(std::get<target::PhysicalRegister>(result.registers_[r])); });
  if (!compact) for (auto &r : f.target().registers()) if (f.target().call_clobbers(r.reg, ir::Type(ir::Type::Kind::Undef))) clobbered.insert(*result.index(r.reg));
  auto bit = [&](MachineRegister r) -> std::optional<std::size_t> {
    if (auto v = std::get_if<const MachineVirtualRegister *>(&r)) {
      auto at = result.virtuals_.find(*v); if (at != result.virtuals_.end()) return at->second;
    } else {
      auto at = result.physicals_.find(std::get<target::PhysicalRegister>(r).encoding()); if (at != result.physicals_.end()) return at->second;
    }
    return std::nullopt;
  };
  auto increase = [&](MachineBlockLiveness &bb, MachineRegister r, int &ints, int &fps) {
    if (integer(f.target(), r)) bb.max_integer_pressure = std::max(bb.max_integer_pressure, ++ints);
    else bb.max_floating_pressure = std::max(bb.max_floating_pressure, ++fps);
  };
  for (std::size_t b = 2; b < cfg.blocks.size(); ++b) {
    auto &bb = result.blocks_[b]; long weight = 1;
    if (advanced && options.calculate_frequencies) for (auto depth = graph.loop_depth()[b]; depth > 0 && weight < LONG_MAX / 8; --depth) weight *= 5;
    auto reference = [&](MachineRegister r, std::size_t p) {
      if (options.calculate_frequencies && std::holds_alternative<const MachineVirtualRegister *>(r)) {
        auto &frequency = result.frequencies_[p]; frequency = frequency < LONG_MAX - weight ? frequency + weight : LONG_MAX;
      }
    };
    int ints = 0, fps = 0; auto body = cfg.blocks[b]->instructions();
    for (auto at = body.end(); at != body.begin();) {
      const auto &i = *--at;
      if (is_call(i.opcode()) && !compact) { bb.kill.union_with(clobbered); bb.gen.subtract(clobbered); bb.gen.union_with(globals); }
      visit_registers(i, true, [&](auto r) {
        if (auto p = bit(r)) {
          if (advanced && bb.gen.contains(*p)) (integer(f.target(), r) ? --ints : --fps);
          bb.gen.erase(*p); bb.kill.insert(*p); reference(r, *p);
        }
      });
      visit_registers(i, false, [&](auto r) {
        if (auto p = bit(r)) {
          if (advanced && !bb.gen.contains(*p)) increase(bb, r, ints, fps);
          bb.gen.insert(*p); reference(r, *p);
        }
      });
      if (compact) continue;
      for (auto r : early_clobbers(f.target(), i.opcode())) if (r) {
        auto p = *result.index(*r); if (advanced) increase(bb, *r, ints, fps);
        bb.gen.erase(p); bb.kill.insert(p); if (advanced) (integer(f.target(), *r) ? --ints : --fps);
      }
      if (is_call(i.opcode())) for (auto r : i.call_arguments()) bb.gen.insert(*result.index(r));
    }
  }
  if (!compact) result.blocks_[1].out = globals;
  std::vector<std::size_t> work(cfg.blocks.size()), pending; std::iota(work.begin(), work.end(), 0);
  std::vector<std::size_t> scheduled(cfg.blocks.size()); std::size_t epoch = 0; bool first = true;
  while (!work.empty()) {
    pending.clear(); ++epoch;
    std::sort(work.begin(), work.end(), [&](auto a, auto b) { return graph.reverse_postorder()[a] > graph.reverse_postorder()[b]; });
    for (auto b : work) {
      auto &bb = result.blocks_[b]; bool changed = first;
      for (auto e : cfg.outgoing[b]) changed |= bb.out.union_with(result.blocks_[cfg.edges[e].destination].in);
      if (!changed) continue;
      auto in = bb.out; in.subtract(bb.kill); in.union_with(bb.gen); if (in == bb.in) continue; bb.in = std::move(in);
      for (auto e : cfg.incoming[b]) { auto p = cfg.edges[e].source; if (scheduled[p] != epoch) { scheduled[p] = epoch; pending.push_back(p); } }
    }
    first = false; work.swap(pending);
  }
  if (advanced) for (auto &bb : result.blocks_) {
    int ints = bb.max_integer_pressure, fps = bb.max_floating_pressure;
    // The reference classifies compact bit positions in its complete register
    // universe here; preserve this heuristic, including its unusual numbering.
    bb.out.for_each([&](auto p) { increase(bb, all[p], ints, fps); });
  }
  if (advanced) for (std::size_t b = 2; b < cfg.blocks.size(); ++b) {
    auto live = result.blocks_[b].out; auto body = cfg.blocks[b]->instructions();
    for (auto at = body.end(); at != body.begin();) {
      auto &i = *--at; std::vector<MachineRegister> dead;
      visit_registers(i, true, [&](auto r) { if (auto p = bit(r)) live.erase(*p); });
      if (is_call(i.opcode()) && !compact) { live.subtract(clobbered); live.union_with(globals); }
      visit_registers(i, false, [&](auto r) { if (auto p = bit(r)) { if (!live.contains(*p)) dead.push_back(r); live.insert(*p); } });
      if (!dead.empty()) result.deaths_.emplace(&i, std::move(dead));
      if (compact) continue;
      for (auto r : early_clobbers(f.target(), i.opcode())) if (r) live.erase(*result.index(*r));
      if (is_call(i.opcode())) for (auto r : i.call_arguments()) live.insert(*result.index(r));
    }
  }
  return result;
}
}
