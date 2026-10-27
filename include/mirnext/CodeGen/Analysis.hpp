/* Licensed under the MIT License. Analyses over the block-owned machine IR. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/Support/SparseBitSet.hpp"
#include <array>

namespace mirnext::codegen {
inline constexpr std::size_t no_machine_node = SIZE_MAX;
struct MachineLoopNode {
  std::size_t block = no_machine_node, entry = no_machine_node, parent = no_machine_node;
  std::vector<std::size_t> children;
  friend bool operator== (const MachineLoopNode &, const MachineLoopNode &) = default;
};
class MachineGraphAnalysis {
 public:
  // Analyses borrow stable machine nodes. The function must outlive its views;
  // mutations invalidate them, without modifying or caching a second body.
  static Result<MachineGraphAnalysis> build (const MachineFunction &);
  static Result<MachineGraphAnalysis> build (const MachineFunction &, const MachineControlFlow &);
  bool current (const MachineFunction &function) const { return cfg_.current (function); }
  const MachineControlFlow &cfg () const { return cfg_; }
  std::span<const std::size_t> preorder () const { return preorder_; }
  std::span<const std::size_t> reverse_postorder () const { return rpost_; }
  std::span<const unsigned char> back_edges () const { return back_; }
  std::span<const MachineLoopNode> loops () const { return loops_; }
  std::size_t loop_root () const { return root_; }
  std::span<const unsigned> loop_depth () const { return depth_; }
  bool has_loops () const { return has_loops_; }

 private:
  static MachineGraphAnalysis analyze (MachineControlFlow);
  MachineControlFlow cfg_;
  std::vector<std::size_t> preorder_, rpost_;
  std::vector<unsigned char> back_;
  std::vector<MachineLoopNode> loops_;
  std::vector<unsigned> depth_;
  std::size_t root_ = no_machine_node;
  bool has_loops_ = false;
};
struct MachineRegisterEffects {
  // Occurrences retain operand order and duplicates for frequency accounting.
  // Allocated calls can also use typed, non-encoding stack-block dependencies.
  std::vector<MachineRegister> definitions, uses;
};
Result<MachineRegisterEffects> machine_register_effects (const MachineFunction &,
                                                         const MachineInstruction &);
std::array<std::optional<target::PhysicalRegister>, 2> early_clobbers (
  const target::TargetMachine &, MachineOpcode);
struct MachineLivenessOptions {
  unsigned optimization_level = 2;
  bool calculate_frequencies = true;
  // Empty selects all registers. Compact scans retain the coalescer's order.
  std::span<const MachineRegister> scan_registers;
  // Additional compilation-environment globals, unioned with the function's
  // owned module-wide binding set. These spans are consumed during this call.
  std::span<const target::PhysicalRegister> global_registers;
};
struct MachineBlockLiveness {
  SparseBitSet in, out, gen, kill;
  int max_integer_pressure = 0, max_floating_pressure = 0;
};
class MachineLiveness {
 public:
  static Result<MachineLiveness> analyze (const MachineFunction &, const MachineGraphAnalysis &,
                                          MachineLivenessOptions = {});
  bool current (const MachineFunction &function) const {
    return owner_ == &function && revision_ == function.revision ();
  }
  unsigned optimization_level () const { return level_; }
  bool is_compact () const { return compact_; }
  bool has_frequencies () const { return has_frequencies_; }
  std::span<const target::PhysicalRegister> global_registers () const { return globals_; }
  std::span<const MachineBlockLiveness> blocks () const { return blocks_; }
  // Bits index this analysis-local typed universe, never a register-number ABI.
  std::span<const MachineRegister> registers () const { return registers_; }
  Result<std::size_t> index (MachineRegister) const;
  Result<bool> contains (const SparseBitSet &, MachineRegister) const;
  Result<long> frequency (MachineRegister) const;
  Result<std::span<const MachineRegister>> deaths (const MachineInstruction &) const;

 private:
  MachineLiveness () = default;
  friend struct detail::PostAllocationEngine;
  // A private consumer may retain a conservative input topology while deleting
  // property edges. This does not make that graph a current public CFG analysis.
  static Result<MachineLiveness> analyze_retained_topology (const MachineFunction &,
                                                            const MachineGraphAnalysis &,
                                                            MachineLivenessOptions);
  const MachineFunction *owner_ = nullptr;
  std::uint64_t revision_ = 0;
  bool has_dead_info_ = false;
  unsigned level_ = 0;
  bool compact_ = false;
  bool has_frequencies_ = false;
  std::vector<target::PhysicalRegister> globals_;
  std::vector<MachineRegister> registers_;
  std::unordered_map<const MachineVirtualRegister *, std::size_t> virtuals_;
  std::unordered_map<unsigned, std::size_t> physicals_;
  std::unordered_map<const MachineInstruction *, std::vector<MachineRegister>> deaths_;
  std::vector<MachineBlockLiveness> blocks_;
  std::vector<long> frequencies_;
};
}  // namespace mirnext::codegen
