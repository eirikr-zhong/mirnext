/* Licensed under the MIT License. Checked machine edits with one error boundary. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Editor.hpp"
#include "mirnext/CodeGen/EditState.hpp"
#include "mirnext/CodeGen/Checked.hpp"
namespace mirnext::codegen {
namespace detail {
Result<void> machine_handle_status (const MachineBatch *batch, const void *value,
                                    std::uint64_t revision) {
  if (!batch) return Error{ErrorCode::InvalidArgument, "moved-from machine editor"};
  if (batch->state->error) return *batch->state->error;
  if (!value) return Error{ErrorCode::InvalidArgument, "invalid machine handle"};
  auto dead = batch->state->erased.find (value);
  if (dead != batch->state->erased.end () && revision <= dead->second)
    return Error{ErrorCode::InvalidArgument, "machine node was erased"};
  return {};
}
}  // namespace detail
MachineEditor::MachineEditor (MachineFunction &function) {
  if (!function.edit_state_) return;
  batch_
    = std::make_unique<detail::MachineBatch> (detail::MachineBatch{function.edit_state_.get ()});
  ++batch_->state->pending;
}
MachineEditor::~MachineEditor () {
  if (batch_ && !batch_->closed) {
    fail ("edit", {ErrorCode::InvalidArgument, "machine edit batch was not finished"});
    --batch_->state->pending;
  }
}
MachineEditor::MachineEditor (MachineEditor &&) noexcept = default;
MachineEditor &MachineEditor::operator= (MachineEditor &&other) noexcept {
  if (this != &other) {
    this->~MachineEditor ();
    new (this) MachineEditor (std::move (other));
  }
  return *this;
}
OptionalRef<const Error> MachineEditor::error () const {
  if (batch_ && batch_->state->error) return *batch_->state->error;
  return {};
}
bool MachineEditor::ready (std::string_view operation) {
  if (!batch_) return false;
  if (batch_->closed)
    fail (operation, {ErrorCode::InvalidArgument, "machine edit batch already finished"});
  return !batch_->closed && !batch_->state->error;
}
void MachineEditor::fail (std::string_view operation, Error error) {
  if (!batch_ || batch_->state->error) return;
  auto &f = *batch_->state->owner;
  error.message = std::string (f.module_name ()) + "/" + std::string (f.name ()) + " ["
                  + std::string (f.target ().name ()) + ", edit/" + std::string (operation)
                  + "]: " + error.message;
  batch_->state->error = std::move (error);
}
Result<const MachineFunction &> MachineEditor::get () const {
  if (!batch_) return Error{ErrorCode::InvalidArgument, "moved-from machine editor"};
  if (batch_->state->error) return *batch_->state->error;
  return *batch_->state->owner;
}
Result<void> MachineEditor::finish () {
  if (!batch_) return Error{ErrorCode::InvalidArgument, "moved-from machine editor"};
  if (!ready ("finish")) {
    if (!batch_->closed) {
      --batch_->state->pending;
      batch_->closed = true;
    }
    return *batch_->state->error;
  }
  --batch_->state->pending;
  batch_->closed = true;
  auto checked = batch_->state->owner->verify ();
  if (!(checked).has_value ()) {
    fail ("finish", checked.error ());
    return *batch_->state->error;
  }
  return {};
}
template <class T>
MachineHandle<T> MachineEditor::handle (const T *value) const {
  if (batch_ && value) batch_->state->tracks_handles = true;
  return {batch_.get (), value, batch_ ? batch_->state->owner->revision () : 0};
}
template <class T>
T *MachineEditor::resolve (MachineNode<T> node, std::string_view operation) {
  if (!ready (operation)) return nullptr;
  if (node.batch_) {
    auto valid = detail::machine_handle_status (node.batch_, node.value_, node.revision_);
    if (!valid.has_value ()) {
      fail (operation, valid.error ());
      return nullptr;
    }
    if (node.batch_->state != batch_->state) {
      fail (operation, {ErrorCode::InvalidArgument, "machine handle owner mismatch"});
      return nullptr;
    }
  }
  if (!node.value_) {
    fail (operation, {ErrorCode::InvalidArgument, "invalid machine node"});
    return nullptr;
  }
  const MachineFunction *owner;
  if constexpr (std::is_same_v<T, MachineInstruction> || std::is_same_v<T, MachineLabel>)
    owner = &node.value_->block ().function ();
  else
    owner = &node.value_->function ();
  if (owner != batch_->state->owner) {
    fail (operation, {ErrorCode::InvalidArgument, "machine node owner mismatch"});
    return nullptr;
  }
  return const_cast<T *> (node.value_);
}
std::optional<std::vector<MachineOperand>> MachineEditor::operands (
  std::span<const MachineInput> inputs, std::string_view operation) {
  if (!ready (operation)) return {};
  std::vector<MachineOperand> result;
  result.reserve (inputs.size ());
  for (const auto &input : inputs) {
    bool valid = true;
    std::visit (
      [&] (const auto &value) {
        using T = std::decay_t<decltype (value)>;
        if constexpr (std::is_same_v<T, MachineOperand>)
          result.push_back (value);
        else {
          auto item = value.get ();
          if (!item.has_value ()) {
            fail (operation, item.error ());
            valid = false;
            return;
          }
          if (value.batch_->state != batch_->state) {
            fail (operation, {ErrorCode::InvalidArgument, "machine operand owner mismatch"});
            valid = false;
            return;
          }
          if constexpr (std::is_same_v<T, MachineHandle<MachineSymbol>>)
            result.emplace_back (detail::NodeAccess::make_MachineSymbolReference (&item.value ()));
          else if constexpr (std::is_same_v<T, MachineHandle<MachineStackSlot>>)
            result.emplace_back (detail::NodeAccess::make_StackSlotReference (&item.value ()));
          else
            result.emplace_back (item.value ());
        }
      },
      input.value_);
    if (!valid) return {};
  }
  return result;
}
MachineHandle<MachineVirtualRegister> MachineEditor::reg (
  ir::Type type, std::string name, bool argument, bool memory_backed,
  std::optional<target::PhysicalRegister> binding) {
  if (!ready ("register")) return handle<MachineVirtualRegister> ();
  auto result = detail::access (*(batch_->state->owner))
                  .create_register (type, std::move (name), argument, memory_backed, binding);
  if (!(result).has_value ()) {
    fail ("register", result.error ());
    return handle<MachineVirtualRegister> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineStackSlot> MachineEditor::stack_slot (std::size_t size,
                                                           std::size_t alignment) {
  if (!ready ("stack slot")) return handle<MachineStackSlot> ();
  auto result = detail::access (*(batch_->state->owner)).create_stack_slot (size, alignment);
  if (!(result).has_value ()) {
    fail ("stack slot", result.error ());
    return handle<MachineStackSlot> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineMemoryLocation> MachineEditor::memory_location () {
  return handle (ready ("memory location")
                   ? &detail::access (*(batch_->state->owner)).create_memory_location ()
                   : nullptr);
}
MachineHandle<MachineBasicBlock> MachineEditor::block (std::string name) {
  return handle (ready ("block")
                   ? &detail::access (*(batch_->state->owner)).create_block (std::move (name))
                   : nullptr);
}
MachineHandle<MachineLabel> MachineEditor::label (MachineNode<MachineBasicBlock> block,
                                                  std::string name, bool address_taken,
                                                  std::optional<std::size_t> source_ordinal,
                                                  bool function_entry) {
  auto node = resolve (block, "label");
  if (!node) return handle<MachineLabel> ();
  auto result
    = detail::access (*(batch_->state->owner))
        .create_label (*node, std::move (name), address_taken, source_ordinal, function_entry);
  if (!(result).has_value ()) {
    fail ("label", result.error ());
    return handle<MachineLabel> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineSymbol> MachineEditor::symbol (MachineSymbol value) {
  if (!ready ("symbol")) return handle<MachineSymbol> ();
  auto result = detail::access (*(batch_->state->owner)).intern_symbol (std::move (value));
  if (!(result).has_value ()) {
    fail ("symbol", result.error ());
    return handle<MachineSymbol> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineInstruction> MachineEditor::append (MachineNode<MachineBasicBlock> block,
                                                         MachineOpcode op,
                                                         std::span<const MachineInput> inputs) {
  auto node = resolve (block, "append");
  if (!node) return handle<MachineInstruction> ();
  auto values = operands (inputs, "append");
  if (!values) return handle<MachineInstruction> ();
  auto result = MachineRewriter (*batch_->state->owner).append (*node, op, std::move (*values));
  if (!(result).has_value ()) {
    fail ("append", result.error ());
    return handle<MachineInstruction> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineInstruction> MachineEditor::insert_before (
  MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
  std::span<const MachineInput> inputs) {
  auto instruction_node = resolve (instruction, "insert_before");
  if (!instruction_node) return handle<MachineInstruction> ();
  auto values = operands (inputs, "insert_before");
  if (!values) return handle<MachineInstruction> ();
  if (!ready ("insert_before")) return handle<MachineInstruction> ();
  auto result = MachineRewriter (*batch_->state->owner)
                  .insert_before (*instruction_node, opcode, std::move (*values));
  if (!(result).has_value ()) {
    fail ("insert_before", result.error ());
    return handle<MachineInstruction> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineInstruction> MachineEditor::insert_after (
  MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
  std::span<const MachineInput> inputs) {
  auto instruction_node = resolve (instruction, "insert_after");
  if (!instruction_node) return handle<MachineInstruction> ();
  auto values = operands (inputs, "insert_after");
  if (!values) return handle<MachineInstruction> ();
  if (!ready ("insert_after")) return handle<MachineInstruction> ();
  auto result = MachineRewriter (*batch_->state->owner)
                  .insert_after (*instruction_node, opcode, std::move (*values));
  if (!(result).has_value ()) {
    fail ("insert_after", result.error ());
    return handle<MachineInstruction> ();
  }
  return handle (&(result).value ());
}
OptionalRef<const MachineInstruction> MachineEditor::previous (
  MachineNode<MachineInstruction> instruction) {
  auto instruction_node = resolve (instruction, "previous");
  if (!instruction_node) return {};
  if (!ready ("previous")) return {};
  auto result = MachineRewriter (*batch_->state->owner).previous (*instruction_node);
  if (!(result).has_value ()) {
    fail ("previous", result.error ());
    return {};
  }
  return (result).value ();
}
OptionalRef<const MachineInstruction> MachineEditor::next (
  MachineNode<MachineInstruction> instruction) {
  auto instruction_node = resolve (instruction, "next");
  if (!instruction_node) return {};
  if (!ready ("next")) return {};
  auto result = MachineRewriter (*batch_->state->owner).next (*instruction_node);
  if (!(result).has_value ()) {
    fail ("next", result.error ());
    return {};
  }
  return (result).value ();
}
void MachineEditor::replace (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                             std::span<const MachineInput> inputs) {
  auto instruction_node = resolve (instruction, "replace");
  if (!instruction_node) return;
  auto values = operands (inputs, "replace");
  if (!values) return;
  if (!ready ("replace")) return;
  auto result = MachineRewriter (*batch_->state->owner)
                  .replace (*instruction_node, opcode, std::move (*values));
  if (!(result).has_value ()) {
    fail ("replace", result.error ());
    return;
  }
}
void MachineEditor::erase (MachineNode<MachineInstruction> instruction) {
  auto instruction_node = resolve (instruction, "erase");
  if (!instruction_node) return;
  if (!ready ("erase")) return;
  auto result = MachineRewriter (*batch_->state->owner).erase (*instruction_node);
  if (!(result).has_value ()) {
    fail ("erase", result.error ());
    return;
  }
  batch_->state->erased[instruction_node] = batch_->state->owner->revision ();
}
void MachineEditor::move_before (MachineNode<MachineInstruction> instruction,
                                 MachineNode<MachineInstruction> before) {
  auto instruction_node = resolve (instruction, "move_before");
  if (!instruction_node) return;
  auto before_node = resolve (before, "move_before");
  if (!before_node) return;
  if (!ready ("move_before")) return;
  auto result
    = MachineRewriter (*batch_->state->owner).move_before (*instruction_node, *before_node);
  if (!(result).has_value ()) {
    fail ("move_before", result.error ());
    return;
  }
}
void MachineEditor::move_to (MachineNode<MachineInstruction> instruction,
                             MachineNode<MachineBasicBlock> block, bool at_end) {
  auto instruction_node = resolve (instruction, "move_to");
  if (!instruction_node) return;
  auto block_node = resolve (block, "move_to");
  if (!block_node) return;
  if (!ready ("move_to")) return;
  auto result
    = MachineRewriter (*batch_->state->owner).move_to (*instruction_node, *block_node, at_end);
  if (!(result).has_value ()) {
    fail ("move_to", result.error ());
    return;
  }
}
void MachineEditor::move_label (MachineNode<MachineLabel> label,
                                MachineNode<MachineBasicBlock> block) {
  auto label_node = resolve (label, "move_label");
  if (!label_node) return;
  auto block_node = resolve (block, "move_label");
  if (!block_node) return;
  if (!ready ("move_label")) return;
  auto result = MachineRewriter (*batch_->state->owner).move_label (*label_node, *block_node);
  if (!(result).has_value ()) {
    fail ("move_label", result.error ());
    return;
  }
}
void MachineEditor::erase_label (MachineNode<MachineLabel> label) {
  auto label_node = resolve (label, "erase_label");
  if (!label_node) return;
  if (!ready ("erase_label")) return;
  auto result = MachineRewriter (*batch_->state->owner).erase_label (*label_node);
  if (!(result).has_value ()) {
    fail ("erase_label", result.error ());
    return;
  }
  batch_->state->erased[label_node] = batch_->state->owner->revision ();
}
MachineHandle<MachineBasicBlock> MachineEditor::split_edge (const MachineControlFlow &graph,
                                                            std::size_t edge) {
  if (!ready ("split edge")) return handle<MachineBasicBlock> ();
  auto split = split_machine_edge (*batch_->state->owner, graph, edge);
  if (!(split).has_value ()) {
    fail ("split edge", split.error ());
    return handle<MachineBasicBlock> ();
  }
  return handle (detail::NodeAccess::block (((split)).value ()));
}
MachineHandle<MachineBasicBlock> MachineEditor::block_before (
  MachineNode<MachineBasicBlock> block) {
  auto block_node = resolve (block, "block_before");
  if (!block_node) return handle<MachineBasicBlock> ();
  if (!ready ("block_before")) return handle<MachineBasicBlock> ();
  auto result = MachineRewriter (*batch_->state->owner).create_block_before (*block_node);
  if (!(result).has_value ()) {
    fail ("block_before", result.error ());
    return handle<MachineBasicBlock> ();
  }
  return handle (&(result).value ());
}
MachineHandle<MachineBasicBlock> MachineEditor::block_after (MachineNode<MachineBasicBlock> block) {
  auto block_node = resolve (block, "block_after");
  if (!block_node) return handle<MachineBasicBlock> ();
  if (!ready ("block_after")) return handle<MachineBasicBlock> ();
  auto result = MachineRewriter (*batch_->state->owner).create_block_after (*block_node);
  if (!(result).has_value ()) {
    fail ("block_after", result.error ());
    return handle<MachineBasicBlock> ();
  }
  return handle (&(result).value ());
}
void MachineEditor::set_call_arguments (MachineNode<MachineInstruction> instruction,
                                        std::span<const target::PhysicalRegister> registers) {
  auto instruction_node = resolve (instruction, "set_call_arguments");
  if (!instruction_node) return;
  if (!ready ("set_call_arguments")) return;
  auto result
    = MachineRewriter (*batch_->state->owner).set_call_arguments (*instruction_node, registers);
  if (!(result).has_value ()) {
    fail ("set_call_arguments", result.error ());
    return;
  }
}
void MachineEditor::set_call_dependencies (MachineNode<MachineInstruction> instruction,
                                           std::span<const CallArgumentDependency> dependencies) {
  auto instruction_node = resolve (instruction, "set_call_dependencies");
  if (!instruction_node) return;
  if (!ready ("set_call_dependencies")) return;
  auto result = MachineRewriter (*batch_->state->owner)
                  .set_call_dependencies (*instruction_node, dependencies);
  if (!(result).has_value ()) {
    fail ("set_call_dependencies", result.error ());
    return;
  }
}
void MachineEditor::lower_call (MachineNode<MachineInstruction> instruction, MachineOpcode opcode,
                                std::span<const MachineInput> inputs) {
  auto instruction_node = resolve (instruction, "lower_call");
  if (!instruction_node) return;
  auto values = operands (inputs, "lower_call");
  if (!values) return;
  if (!ready ("lower_call")) return;
  auto result = MachineRewriter (*batch_->state->owner)
                  .lower_call (*instruction_node, opcode, std::move (*values));
  if (!(result).has_value ()) {
    fail ("lower_call", result.error ());
    return;
  }
}
void MachineEditor::set_state (MachineFunctionState state) {
  if (!ready ("set_state")) return;
  auto result = MachineRewriter (*batch_->state->owner).set_state (state);
  if (!(result).has_value ()) {
    fail ("set_state", result.error ());
    return;
  }
}
void MachineEditor::set_global_registers (std::span<const target::PhysicalRegister> registers) {
  if (!ready ("set_global_registers")) return;
  auto result = MachineRewriter (*batch_->state->owner).set_global_registers (registers);
  if (!(result).has_value ()) {
    fail ("set_global_registers", result.error ());
    return;
  }
}
void MachineEditor::set_entry_setup (MachineNode<MachineInstruction> instruction, bool enabled) {
  auto instruction_node = resolve (instruction, "set_entry_setup");
  if (!instruction_node) return;
  if (!ready ("set_entry_setup")) return;
  auto result
    = MachineRewriter (*batch_->state->owner).set_entry_setup (*instruction_node, enabled);
  if (!(result).has_value ()) {
    fail ("set_entry_setup", result.error ());
    return;
  }
}
}  // namespace mirnext::codegen
