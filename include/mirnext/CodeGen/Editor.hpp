/* Licensed under the MIT License. First-error machine mutation batches. */
#pragma once
#include "mirnext/CodeGen/MachineIR.hpp"
namespace mirnext::codegen {
namespace detail {
struct MachineBatch;
Result<void> machine_handle_status (const MachineBatch *, const void *, std::uint64_t);
}  // namespace detail
class MachineEditor;
template <class T>
class MachineHandle {
 public:
  Result<const T &> get () const {
    auto valid = detail::machine_handle_status (batch_, value_, revision_);
    if (!valid.has_value ()) return valid.error ();
    return *value_;
  }

 private:
  friend class MachineEditor;
  template <class>
  friend class MachineNode;
  MachineHandle (detail::MachineBatch *batch, const T *value, std::uint64_t revision)
    : batch_ (batch), value_ (value), revision_ (revision) {}
  detail::MachineBatch *batch_;
  const T *value_;
  std::uint64_t revision_;
};
template <class T>
class MachineNode {
 public:
  MachineNode (const T &value) : value_ (&value) {}
  MachineNode (MachineHandle<T> value)
    : value_ (value.value_), batch_ (value.batch_), revision_ (value.revision_) {}

 private:
  friend class MachineEditor;
  const T *value_;
  detail::MachineBatch *batch_ = nullptr;
  std::uint64_t revision_ = 0;
};
class MachineInput {
 public:
  template <class T>
    requires std::is_constructible_v<MachineOperand, T>
  MachineInput (T &&value) : value_ (MachineOperand (std::forward<T> (value))) {}
  MachineInput (MachineHandle<MachineVirtualRegister> value) : value_ (value) {}
  MachineInput (MachineHandle<MachineLabel> value) : value_ (value) {}
  MachineInput (MachineHandle<MachineSymbol> value) : value_ (value) {}
  MachineInput (MachineHandle<MachineStackSlot> value) : value_ (value) {}

 private:
  friend class MachineEditor;
  using Storage = std::variant<MachineOperand, MachineHandle<MachineVirtualRegister>,
                               MachineHandle<MachineLabel>, MachineHandle<MachineSymbol>,
                               MachineHandle<MachineStackSlot>>;
  Storage value_;
};
class MachineEditor {
 public:
  explicit MachineEditor (MachineFunction &);
  ~MachineEditor ();
  MachineEditor (MachineEditor &&) noexcept;
  MachineEditor &operator= (MachineEditor &&) noexcept;
  MachineEditor (const MachineEditor &) = delete;
  MachineEditor &operator= (const MachineEditor &) = delete;
  Result<void> finish ();
  OptionalRef<const Error> error () const;
  Result<const MachineFunction &> get () const;
  MachineHandle<MachineVirtualRegister> reg (ir::Type, std::string name = {}, bool argument = false,
                                             bool memory_backed = false,
                                             std::optional<target::PhysicalRegister> binding = {});
  MachineHandle<MachineStackSlot> stack_slot (std::size_t size, std::size_t alignment);
  MachineHandle<MachineMemoryLocation> memory_location ();
  MachineHandle<MachineBasicBlock> block (std::string name = {});
  MachineHandle<MachineLabel> label (MachineNode<MachineBasicBlock>, std::string name = {},
                                     bool address_taken = false,
                                     std::optional<std::size_t> source_ordinal = {},
                                     bool function_entry = false);
  MachineHandle<MachineSymbol> symbol (MachineSymbol);
  MachineHandle<MachineInstruction> append (MachineNode<MachineBasicBlock>, MachineOpcode,
                                            std::span<const MachineInput>);
  MachineHandle<MachineInstruction> append (MachineNode<MachineBasicBlock> block, MachineOpcode op,
                                            std::initializer_list<MachineInput> inputs) {
    return append (block, op, std::span<const MachineInput> (inputs.begin (), inputs.size ()));
  }
  MachineHandle<MachineInstruction> insert_before (MachineNode<MachineInstruction> instruction,
                                                   MachineOpcode opcode,
                                                   std::span<const MachineInput> inputs);
  MachineHandle<MachineInstruction> insert_after (MachineNode<MachineInstruction> instruction,
                                                  MachineOpcode opcode,
                                                  std::span<const MachineInput> inputs);
  MachineHandle<MachineInstruction> insert_before (MachineNode<MachineInstruction> instruction,
                                                   MachineOpcode opcode,
                                                   std::initializer_list<MachineInput> inputs) {
    return insert_before (instruction, opcode,
                          std::span<const MachineInput> (inputs.begin (), inputs.size ()));
  }
  MachineHandle<MachineInstruction> insert_after (MachineNode<MachineInstruction> instruction,
                                                  MachineOpcode opcode,
                                                  std::initializer_list<MachineInput> inputs) {
    return insert_after (instruction, opcode,
                         std::span<const MachineInput> (inputs.begin (), inputs.size ()));
  }
  void replace (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                std::initializer_list<MachineInput> inputs) {
    replace (instruction, opcode, std::span<const MachineInput> (inputs.begin (), inputs.size ()));
  }
  void lower_call (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                   std::initializer_list<MachineInput> inputs) {
    lower_call (instruction, opcode,
                std::span<const MachineInput> (inputs.begin (), inputs.size ()));
  }
  OptionalRef<const MachineInstruction> previous (MachineNode<MachineInstruction> instruction);
  OptionalRef<const MachineInstruction> next (MachineNode<MachineInstruction> instruction);
  void replace (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                std::span<const MachineInput> inputs);
  void erase (MachineNode<MachineInstruction> instruction);
  void move_before (MachineNode<MachineInstruction> instruction,
                    MachineNode<MachineInstruction> before);
  void move_to (MachineNode<MachineInstruction> instruction, MachineNode<MachineBasicBlock> block,
                bool at_end = true);
  void move_label (MachineNode<MachineLabel> label, MachineNode<MachineBasicBlock> block);
  void erase_label (MachineNode<MachineLabel> label);
  MachineHandle<MachineBasicBlock> split_edge (const MachineControlFlow &, std::size_t edge);
  MachineHandle<MachineBasicBlock> block_before (MachineNode<MachineBasicBlock> block);
  MachineHandle<MachineBasicBlock> block_after (MachineNode<MachineBasicBlock> block);
  void set_call_arguments (MachineNode<MachineInstruction> instruction,
                           std::span<const target::PhysicalRegister> registers);
  void set_call_dependencies (MachineNode<MachineInstruction> instruction,
                              std::span<const CallArgumentDependency> dependencies);
  void lower_call (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                   std::span<const MachineInput> inputs);
  void set_state (MachineFunctionState state);
  void set_global_registers (std::span<const target::PhysicalRegister> registers);
  void set_entry_setup (MachineNode<MachineInstruction> instruction, bool enabled = true);

 private:
  bool ready (std::string_view);
  void fail (std::string_view, Error);
  template <class T>
  T *resolve (MachineNode<T>, std::string_view);
  template <class T>
  MachineHandle<T> handle (const T * = nullptr) const;
  std::optional<std::vector<MachineOperand>> operands (std::span<const MachineInput>,
                                                       std::string_view);
  std::unique_ptr<detail::MachineBatch> batch_;
};
}  // namespace mirnext::codegen
