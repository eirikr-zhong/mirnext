/* Licensed under the MIT License. Local analysis indices, never register identities. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen::detail {
struct RegisterIndex {
  std::vector<MachineRegister> values;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtuals;
  std::unordered_map<unsigned, std::size_t> physicals;
  explicit RegisterIndex(const MachineFunction &f) {
    for (auto &p : f.target().registers()) { physicals.emplace(p.reg.encoding(), values.size()); values.emplace_back(p.reg); }
    for (auto &r : f.registers()) { virtuals.emplace(&r, values.size()); values.emplace_back(&r); }
  }
  std::optional<std::size_t> find(MachineRegister r) const {
    if (auto v = std::get_if<const MachineVirtualRegister *>(&r)) {
      auto at = virtuals.find(*v); if (at != virtuals.end()) return at->second;
    } else {
      auto p = std::get<target::PhysicalRegister>(r);
      auto at = physicals.find(p.encoding());
      if (at != physicals.end() && std::get<target::PhysicalRegister>(values[at->second]) == p) return at->second;
    }
    return {};
  }
};
}
