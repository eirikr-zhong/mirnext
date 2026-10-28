/* Licensed under the MIT License. Private checked machine storage access. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
#include "mirnext/CodeGen/BlockVersioning.hpp"
#include "mirnext/IR/Access.hpp"
namespace mirnext::codegen::detail {
class MachineAccess {
 public:
  explicit MachineAccess (MachineFunction &value) : value_ (value) {}
  Result<MachineVirtualRegister &> create_register (ir::Type type, std::string name = {},
                                                    bool argument = false,
                                                    bool memory_backed = false,
                                                    std::optional<target::PhysicalRegister> binding
                                                    = {}) {
    return value_.create_register (type, std::move (name), argument, memory_backed, binding);
  }
  Result<MachineStackSlot &> create_stack_slot (std::size_t size, std::size_t alignment) {
    return value_.create_stack_slot (size, alignment);
  }
  const MachineMemoryLocation &create_memory_location () {
    return value_.create_memory_location ();
  }
  MachineBasicBlock &create_block (std::string name = {}) {
    return value_.create_block (std::move (name));
  }
  Result<MachineLabel &> create_label (MachineBasicBlock &block, std::string name = {},
                                       bool address_taken = false,
                                       std::optional<std::size_t> source_ordinal = {},
                                       bool function_entry = false) {
    return value_.create_label (block, std::move (name), address_taken, source_ordinal,
                                function_entry);
  }
  Result<const MachineSymbol &> intern_symbol (MachineSymbol symbol) {
    return value_.intern_symbol (std::move (symbol));
  }

 private:
  MachineFunction &value_;
};
inline MachineAccess access (MachineFunction &value) { return MachineAccess (value); }
class OperandAccess {
 public:
  explicit OperandAccess (const MachineOperand &value) : value_ (value) {}
  template <class T>
  const T *get () const {
    return std::get_if<T> (&value_.storage_);
  }
  const auto &storage () const { return value_.storage_; }

 private:
  const MachineOperand &value_;
};
inline OperandAccess operand_access (const MachineOperand &value) { return OperandAccess (value); }
class NodeAccess {
 public:
  static auto &blocks (MachineLiveRange &range) { return range.blocks_; }
  static const auto &blocks (const MachineLiveRange &range) { return range.blocks_; }
  template <class T>
    requires (!std::is_pointer_v<std::remove_cvref_t<T>>)
  static MachineRegister make_MachineRegister (T &&value) {
    return MachineRegister (std::forward<T> (value));
  }
  template <class T>
  static std::optional<MachineRegister> optional_register (T &&value) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<T>>
                  || std::is_same_v<std::remove_cvref_t<T>, std::nullptr_t>)
      return make_MachineRegister (value);
    else
      return std::optional<MachineRegister> (std::forward<T> (value));
  }
  template <class B = std::optional<MachineRegister>, class I = std::optional<MachineRegister>>
  static MachineMemory make_MachineMemory (ir::Type type, B &&base = {}, I &&index = {},
                                           std::int64_t displacement = 0, unsigned scale = 1,
                                           std::string alias = {}, std::string nonalias = {},
                                           OptionalRef<const MachineMemoryLocation> location = {}) {
    return MachineMemory (type, optional_register (std::forward<B> (base)),
                          optional_register (std::forward<I> (index)), displacement, scale,
                          std::move (alias), std::move (nonalias), location);
  }
  template <class... T>
  static auto &storage (std::variant<T...> &value) {
    return value;
  }
  template <class... T>
  static const auto &storage (const std::variant<T...> &value) {
    return value;
  }
  template <class... T>
  static auto *storage (std::variant<T...> *value) {
    return value;
  }
  template <class... T>
  static const auto *storage (const std::variant<T...> *value) {
    return value;
  }
  template <class T>
  static MachineOperand make_MachineOperand (T &&value) {
    if constexpr (std::is_pointer_v<std::remove_cvref_t<T>>) {
      MachineOperand result (std::int64_t (0));
      result.storage_ = value;
      return result;
    } else
      return MachineOperand (std::forward<T> (value));
  }
  static MachineRegister make_MachineRegister (const MachineVirtualRegister *value) {
    return MachineRegister (MachineRegister::Key{}, value);
  }
  static auto &storage (MachineRegister &value) { return value.value_; }
  static auto *storage (MachineRegister *value) { return value ? &value->value_ : nullptr; }
  static const auto &storage (const MachineRegister &value) { return value.value_; }
  static const auto *storage (const MachineRegister *value) {
    return value ? &value->value_ : nullptr;
  }
  static BBVPropertyLocation make_BBVPropertyLocation (const MachineMemoryLocation *value) {
    return BBVPropertyLocation (BBVPropertyLocation::Key{}, value);
  }
  static auto &storage (BBVPropertyLocation &value) { return value.value_; }
  static auto *storage (BBVPropertyLocation *value) { return value ? &value->value_ : nullptr; }
  static const auto &storage (const BBVPropertyLocation &value) { return value.value_; }
  static const auto *storage (const BBVPropertyLocation *value) {
    return value ? &value->value_ : nullptr;
  }

  static MachineSymbolReference make_MachineSymbolReference (const MachineSymbol *symbol = nullptr,
                                                             std::int64_t displacement = 0) {
    return MachineSymbolReference (MachineSymbolReference::Key{}, symbol, std::move (displacement));
  }
  static const MachineSymbol *symbol (const MachineSymbolReference &value) { return value.symbol_; }
  static const MachineSymbol *&symbol (MachineSymbolReference &value) { return value.symbol_; }
  static StackSlotReference make_StackSlotReference (const MachineStackSlot *slot = nullptr,
                                                     std::int64_t displacement = 0) {
    return StackSlotReference (StackSlotReference::Key{}, slot, std::move (displacement));
  }
  static const MachineStackSlot *slot (const StackSlotReference &value) { return value.slot_; }
  static const MachineStackSlot *&slot (StackSlotReference &value) { return value.slot_; }
  static CallArgumentDependency make_CallArgumentDependency (std::size_t operand,
                                                             const MachineVirtualRegister *value) {
    return CallArgumentDependency (CallArgumentDependency::Key{}, std::move (operand), value);
  }
  static const MachineVirtualRegister *value (const CallArgumentDependency &value) {
    return value.value_;
  }
  static const MachineVirtualRegister *&value (CallArgumentDependency &value) {
    return value.value_;
  }
  static MachineAllocationGap make_MachineAllocationGap (target::PhysicalRegister physical,
                                                         const MachineVirtualRegister *reg,
                                                         MachineLiveRange range) {
    return MachineAllocationGap (MachineAllocationGap::Key{}, std::move (physical), reg,
                                 std::move (range));
  }
  static const MachineVirtualRegister *reg (const MachineAllocationGap &value) {
    return value.reg_;
  }
  static const MachineVirtualRegister *&reg (MachineAllocationGap &value) { return value.reg_; }
  static MachineSpillPlacement make_MachineSpillPlacement (const MachineVirtualRegister *reg,
                                                           const MachineBasicBlock *block,
                                                           bool end = false, bool store = false) {
    return MachineSpillPlacement (MachineSpillPlacement::Key{}, reg, block, std::move (end),
                                  std::move (store));
  }
  static const MachineVirtualRegister *reg (const MachineSpillPlacement &value) {
    return value.reg_;
  }
  static const MachineVirtualRegister *&reg (MachineSpillPlacement &value) { return value.reg_; }
  static const MachineBasicBlock *block (const MachineSpillPlacement &value) {
    return value.block_;
  }
  static const MachineBasicBlock *&block (MachineSpillPlacement &value) { return value.block_; }
  static MachineEdgeSplit make_MachineEdgeSplit (const MachineBasicBlock *block,
                                                 MachineControlFlow cfg) {
    return MachineEdgeSplit (MachineEdgeSplit::Key{}, block, std::move (cfg));
  }
  static const MachineBasicBlock *block (const MachineEdgeSplit &value) { return value.block_; }
  static const MachineBasicBlock *&block (MachineEdgeSplit &value) { return value.block_; }
  static CodeSymbolRequest make_CodeSymbolRequest (const MachineSymbol *symbol = nullptr,
                                                   std::optional<Builtin> builtin = {}) {
    return CodeSymbolRequest (CodeSymbolRequest::Key{}, symbol, std::move (builtin));
  }
  static const MachineSymbol *symbol (const CodeSymbolRequest &value) { return value.symbol_; }
  static const MachineSymbol *&symbol (CodeSymbolRequest &value) { return value.symbol_; }
  static BBVSuccessor make_BBVSuccessor (BBVSuccessor::Kind kind, const BBVBlock *block,
                                         const MachineInstruction *instruction, std::size_t operand,
                                         const MachineLabel *label) {
    return BBVSuccessor (BBVSuccessor::Key{}, std::move (kind), block, instruction,
                         std::move (operand), label);
  }
  static const BBVBlock *block (const BBVSuccessor &value) { return value.block_; }
  static const BBVBlock *&block (BBVSuccessor &value) { return value.block_; }
  static const MachineInstruction *instruction (const BBVSuccessor &value) {
    return value.instruction_;
  }
  static const MachineInstruction *&instruction (BBVSuccessor &value) { return value.instruction_; }
  static const MachineLabel *label (const BBVSuccessor &value) { return value.label_; }
  static const MachineLabel *&label (BBVSuccessor &value) { return value.label_; }
  static BBVEmission make_BBVEmission (const MachineInstruction *instruction,
                                       MachineOpcode opcode) {
    return BBVEmission (BBVEmission::Key{}, instruction, std::move (opcode));
  }
  static const MachineInstruction *instruction (const BBVEmission &value) {
    return value.instruction_;
  }
  static const MachineInstruction *&instruction (BBVEmission &value) { return value.instruction_; }
  static BBVVersionEdge make_BBVVersionEdge (BBVSuccessor::Kind kind,
                                             const MachineInstruction *instruction,
                                             std::size_t operand, const BBVVersion *version,
                                             bool newly_created) {
    return BBVVersionEdge (BBVVersionEdge::Key{}, std::move (kind), instruction,
                           std::move (operand), version, std::move (newly_created));
  }
  static const MachineInstruction *instruction (const BBVVersionEdge &value) {
    return value.instruction_;
  }
  static const MachineInstruction *&instruction (BBVVersionEdge &value) {
    return value.instruction_;
  }
  static const BBVVersion *version (const BBVVersionEdge &value) { return value.version_; }
  static const BBVVersion *&version (BBVVersionEdge &value) { return value.version_; }
  static BBVVersionRequest make_BBVVersionRequest (const BBVVersion *version, bool created) {
    return BBVVersionRequest (BBVVersionRequest::Key{}, version, std::move (created));
  }
  static const BBVVersion *version (const BBVVersionRequest &value) { return value.version_; }
  static const BBVVersion *&version (BBVVersionRequest &value) { return value.version_; }
  static const MachineMemoryLocation *location (const MachineMemory &value) {
    return value.location_;
  }
  static const MachineMemoryLocation *&location (MachineMemory &value) { return value.location_; }
  static auto &blocks (MachineControlFlow &value) { return value.blocks_; }
  static const auto &blocks (const MachineControlFlow &value) { return value.blocks_; }
};

}  // namespace mirnext::codegen::detail
