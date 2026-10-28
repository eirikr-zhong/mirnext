/* Licensed under the MIT License. Owning BBV preparation and block identities. */
#pragma once
#include "mirnext/CodeGen/Compilation.hpp"
#include "mirnext/CodeGen/BlockVersionId.hpp"
namespace mirnext::codegen {
namespace detail {
class NodeAccess;
}
namespace detail {
struct BBVLayout;
struct BBVState;
}  // namespace detail
class BBVBlock;
struct BBVSuccessor {
  enum class Kind { LabelAddress, Branch, SwitchCase, Fallthrough };
  BBVSuccessor (Kind kind, const BBVBlock &block, OptionalRef<const MachineInstruction> instruction,
                std::size_t operand, OptionalRef<const MachineLabel> label)
    : BBVSuccessor (Key{}, std::move (kind), &block, instruction.get (), std::move (operand),
                    label.get ()) {}
  Kind kind;
  const BBVBlock &block () const { return *block_; }
  OptionalRef<const MachineInstruction> instruction () const {
    return ::mirnext::detail::borrow_optional (instruction_);
  }
  std::size_t operand;
  OptionalRef<const MachineLabel> label () const {
    return ::mirnext::detail::borrow_optional (label_);
  }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  BBVSuccessor (Key, Kind kind, const BBVBlock *block, const MachineInstruction *instruction,
                std::size_t operand, const MachineLabel *label)
    : kind (kind), operand (operand), block_ (block), instruction_ (instruction), label_ (label) {}
  const BBVBlock *block_;
  const MachineInstruction *instruction_;
  const MachineLabel *label_;
};
class BBVBlock {
 public:
  const MachineFunction &function () const { return *function_; }
  std::size_t index () const { return index_; }
  ir::ReferenceRange<const MachineInstruction> instructions () const {
    return ir::ReferenceRange<const MachineInstruction> (instructions_);
  }
  ir::ReferenceRange<const MachineLabel> labels () const {
    return ir::ReferenceRange<const MachineLabel> (labels_);
  }
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
  Result<const BBVBlock &> block_for (const MachineInstruction &) const;
  Result<const BBVBlock &> block_for (const MachineLabel &) const;
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
class BBVPropertyLocation {
  using Storage = std::variant<target::PhysicalRegister, const MachineMemoryLocation *>;

 public:
  BBVPropertyLocation (target::PhysicalRegister value) : value_ (value) {}
  BBVPropertyLocation (const MachineMemoryLocation &value) : value_ (&value) {}
  template <class T>
    requires (!std::is_pointer_v<T>)
  OptionalRef<const T> get () const {
    if constexpr (std::is_same_v<T, MachineMemoryLocation>) {
      auto value = std::get_if<const T *> (&value_);
      return value ? ::mirnext::detail::borrow_optional (*value) : OptionalRef<const T>{};
    } else
      return ::mirnext::detail::borrow_optional (std::get_if<T> (&value_));
  }
  friend bool operator== (const BBVPropertyLocation &, const BBVPropertyLocation &) = default;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  BBVPropertyLocation (Key, const MachineMemoryLocation *value) : value_ (value) {}
  Storage value_;
};
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
  OptionalRef<const BBVProperty> find (BBVPropertyLocation) const;
  Result<void> verify () const;

 private:
  friend struct detail::BBVState;
  explicit BBVProperties (const MachineFunction &function) : function_ (&function) {}
  const MachineFunction *function_;
  std::vector<BBVProperty> entries_;
};
class BBVVersion;
struct BBVEmission {
  BBVEmission (const MachineInstruction &instruction, MachineOpcode opcode)
    : BBVEmission (Key{}, &instruction, std::move (opcode)) {}
  const MachineInstruction &instruction () const { return *instruction_; }
  MachineOpcode opcode;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  BBVEmission (Key, const MachineInstruction *instruction, MachineOpcode opcode)
    : opcode (std::move (opcode)), instruction_ (instruction) {}
  const MachineInstruction *instruction_;
};
struct BBVVersionEdge {
  BBVVersionEdge (BBVSuccessor::Kind kind, OptionalRef<const MachineInstruction> instruction,
                  std::size_t operand, const BBVVersion &version, bool newly_created)
    : BBVVersionEdge (Key{}, std::move (kind), instruction.get (), std::move (operand), &version,
                      std::move (newly_created)) {}
  BBVSuccessor::Kind kind;
  OptionalRef<const MachineInstruction> instruction () const {
    return ::mirnext::detail::borrow_optional (instruction_);
  }
  std::size_t operand;
  const BBVVersion &version () const { return *version_; }
  bool newly_created;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  BBVVersionEdge (Key, BBVSuccessor::Kind kind, const MachineInstruction *instruction,
                  std::size_t operand, const BBVVersion *version, bool newly_created)
    : kind (kind),
      operand (operand),
      newly_created (newly_created),
      instruction_ (instruction),
      version_ (version) {}
  const MachineInstruction *instruction_;
  const BBVVersion *version_;
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
  OptionalRef<const BBVSpecialization> specialization () const {
    return ::mirnext::detail::borrow_optional (specialized_.get ());
  }

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
  BBVVersionRequest (const BBVVersion &version, bool created)
    : BBVVersionRequest (Key{}, &version, std::move (created)) {}
  const BBVVersion &version () const { return *version_; }
  bool created;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  BBVVersionRequest (Key, const BBVVersion *version, bool created)
    : created (created), version_ (version) {}
  const BBVVersion *version_;
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
  Result<const BBVSpecialization &> specialize (const BBVVersion &);
  Result<void> verify () const;
  std::string dump () const;

 private:
  explicit BBVProgram (std::unique_ptr<detail::BBVState>);
  std::unique_ptr<detail::BBVState> state_;
};
}  // namespace mirnext::codegen
