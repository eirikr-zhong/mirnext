/* Licensed under the MIT License. Owning BBV preparation and block identities. */
#pragma once
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/BlockVersionId.hpp"
namespace mirnext::codegen {
namespace detail {
struct BBVLayout;
struct BBVState;
}  // namespace detail
class BBVBlock;
struct BBVSuccessor {
  enum class Kind { LabelAddress, Branch, SwitchCase, Fallthrough };
  Kind kind;
  const BBVBlock *block;
  const MachineInstruction *instruction;
  std::size_t operand;
  const MachineLabel *label;
};
class BBVBlock {
 public:
  const MachineFunction &function () const { return *function_; }
  std::size_t index () const { return index_; }
  std::span<const MachineInstruction *const> instructions () const { return instructions_; }
  std::span<const MachineLabel *const> labels () const { return labels_; }
  std::span<const BBVSuccessor> successors () const { return successors_; }

 private:
  friend struct detail::BBVLayout;
  BBVBlock (const MachineFunction &function, std::size_t index)
    : function_ (&function), index_ (index) {}
  const MachineFunction *function_;
  std::size_t index_;
  std::vector<const MachineInstruction *> instructions_;
  std::vector<const MachineLabel *> labels_;
  std::vector<BBVSuccessor> successors_;
};
// This is preparation, not an executable BBV engine. It retains one authoritative
// framed machine body; blocks only index stable instructions/label anchors.
// Views remain valid across owner moves and until the owning preparation dies.
class BBVPreparation {
 public:
  static Result<BBVPreparation> prepare (const ir::Function &, const target::TargetMachine &,
                                         CompilationOptions = {});
  static Result<BBVPreparation> create (PreparedMachineFunction);
  BBVPreparation (BBVPreparation &&) noexcept = default;
  BBVPreparation &operator= (BBVPreparation &&) noexcept = default;
  BBVPreparation (const BBVPreparation &) = delete;
  BBVPreparation &operator= (const BBVPreparation &) = delete;
  const PreparedMachineFunction &prepared () const { return prepared_; }
  const MachineFunction &function () const { return prepared_.function (); }
  auto blocks () const { return ir::nodes (blocks_); }
  Result<const BBVBlock *> block_for (const MachineInstruction &) const;
  Result<const BBVBlock *> block_for (const MachineLabel &) const;
  Result<void> verify () const;
  std::string dump () const;

 private:
  friend struct detail::BBVLayout;
  explicit BBVPreparation (PreparedMachineFunction prepared) : prepared_ (std::move (prepared)) {}
  PreparedMachineFunction prepared_;
  std::vector<std::unique_ptr<BBVBlock>> blocks_;
  std::unordered_map<const MachineInstruction *, const BBVBlock *> instructions_;
  std::unordered_map<const MachineLabel *, const BBVBlock *> labels_;
};
using BBVPropertyLocation = std::variant<target::PhysicalRegister, const MachineMemoryLocation *>;
struct BBVProperty {
  BBVPropertyLocation location;
  std::uint32_t value;
  // Retain the last memory access's alias facts, without borrowing an operand
  // vector or an SSA definition. Register properties have no memory access.
  std::optional<MachineMemory> memory;
};
class BBVProperties {
 public:
  static Result<BBVProperties> create (const MachineFunction &, std::span<const BBVProperty> = {});
  const MachineFunction &function () const { return *function_; }
  std::span<const BBVProperty> entries () const { return entries_; }
  const BBVProperty *find (BBVPropertyLocation) const;
  Result<void> verify () const;

 private:
  friend struct detail::BBVState;
  explicit BBVProperties (const MachineFunction &function) : function_ (&function) {}
  const MachineFunction *function_;
  std::vector<BBVProperty> entries_;
};
class BBVVersion;
struct BBVEmission {
  const MachineInstruction *instruction;
  // PRBEQ/PRBNE become JMP in the emission plan. The authoritative machine
  // body and its stable identities are never rewritten during specialization.
  MachineOpcode opcode;
};
struct BBVVersionEdge {
  BBVSuccessor::Kind kind;
  const MachineInstruction *instruction;
  std::size_t operand;
  const BBVVersion *version;
  bool newly_created;
};
class BBVSpecialization {
 public:
  std::span<const BBVEmission> emissions () const { return emissions_; }
  std::span<const BBVVersionEdge> successors () const { return successors_; }
  const BBVProperties &outgoing () const { return outgoing_; }

 private:
  friend struct detail::BBVState;
  explicit BBVSpecialization (BBVProperties properties) : outgoing_ (std::move (properties)) {}
  std::vector<BBVEmission> emissions_;
  std::vector<BBVVersionEdge> successors_;
  BBVProperties outgoing_;
};
class BBVVersion {
 public:
  BlockVersionId id () const { return id_; }
  const BBVBlock &block () const { return *block_; }
  const BBVProperties &incoming () const { return incoming_; }
  bool call_entry () const { return call_entry_; }
  const BBVSpecialization *specialization () const { return specialized_.get (); }

 private:
  friend struct detail::BBVState;
  BBVVersion (const detail::BBVState &owner, BlockVersionId id, const BBVBlock &block,
              BBVProperties properties, bool call)
    : id_ (std::move (id)),
      owner_ (&owner),
      block_ (&block),
      incoming_ (std::move (properties)),
      call_entry_ (call) {}
  BlockVersionId id_;
  const detail::BBVState *owner_;
  const BBVBlock *block_;
  BBVProperties incoming_;
  bool call_entry_;
  std::unique_ptr<BBVSpecialization> specialized_;
};
struct BBVVersionRequest {
  const BBVVersion *version;
  bool created;
};
// Owns stable versions and specialization plans, not executable memory. The
// current MIR policy reuses the first version of each block, ignoring later
// incoming properties.
class BBVProgram {
 public:
  static Result<BBVProgram> create (BBVPreparation);
  BBVProgram (BBVProgram &&) noexcept;
  BBVProgram &operator= (BBVProgram &&) noexcept;
  ~BBVProgram ();
  BBVProgram (const BBVProgram &) = delete;
  BBVProgram &operator= (const BBVProgram &) = delete;
  const BBVPreparation &preparation () const;
  Result<BBVVersionRequest> request_version (const BBVBlock &, const BBVProperties &,
                                             bool call_entry = false);
  Result<const BBVSpecialization *> specialize (const BBVVersion &);
  Result<void> verify () const;
  std::string dump () const;

 private:
  explicit BBVProgram (std::unique_ptr<detail::BBVState>);
  std::unique_ptr<detail::BBVState> state_;
};
}  // namespace mirnext::codegen
