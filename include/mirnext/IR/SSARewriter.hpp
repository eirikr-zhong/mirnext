/* Licensed under the MIT License. SSA extensions share their function edit batch. */
#pragma once
#include "mirnext/IR/Builder.hpp"
#include "mirnext/IR/SSA.hpp"
namespace mirnext::ir {
namespace detail {
struct FunctionBuildState;
struct BuildAccess;
enum class SSAHandleKind { Value, Use, Phi, Register, Block, Instruction };
Result<void> ssa_handle_status (FunctionBuildState *, const SSAForm *, const void *, SSAHandleKind,
                                std::uint64_t, std::uint64_t);
template <class T>
constexpr SSAHandleKind ssa_kind () {
  if constexpr (std::is_same_v<T, SSAValue>)
    return SSAHandleKind::Value;
  else if constexpr (std::is_same_v<T, SSAUse>)
    return SSAHandleKind::Use;
  else if constexpr (std::is_same_v<T, SSAPhi>)
    return SSAHandleKind::Phi;
  else if constexpr (std::is_same_v<T, Register>)
    return SSAHandleKind::Register;
  else if constexpr (std::is_same_v<T, BasicBlock>)
    return SSAHandleKind::Block;
  else
    return SSAHandleKind::Instruction;
}
}  // namespace detail
class SSARewriter;
template <class T>
class SSAHandle {
 public:
  Result<const T &> get () const {
    auto valid = detail::ssa_handle_status (state_, form_, value_, detail::ssa_kind<T> (),
                                            revision_, phase_);
    if (!valid.has_value ()) return valid.error ();
    return *value_;
  }

 private:
  friend class SSARewriter;
  template <class>
  friend class SSANode;
  SSAHandle (detail::FunctionBuildState *state, const SSAForm *form, const T *value,
             std::uint64_t revision)
    : state_ (state),
      form_ (form),
      value_ (value),
      revision_ (revision),
      phase_ (form ? form->phase_identity () : 0) {}
  detail::FunctionBuildState *state_;
  const SSAForm *form_;
  const T *value_;
  std::uint64_t revision_ = 0, phase_ = 0;
};
template <class T>
class SSANode {
 public:
  SSANode (const T &value) : value_ (&value) {
    if constexpr (requires { value.form (); }) {
      form_ = &value.form ();
      phase_ = form_->phase_identity ();
    } else
      revision_ = value.function ().revisions ().instructions;
  }
  SSANode (SSAHandle<T> value)
    : state_ (value.state_),
      form_ (value.form_),
      value_ (value.value_),
      revision_ (value.revision_),
      phase_ (value.phase_) {}

 private:
  friend class SSARewriter;
  detail::FunctionBuildState *state_ = nullptr;
  const SSAForm *form_ = nullptr;
  const T *value_;
  std::uint64_t revision_ = 0, phase_ = 0;
};
class SSARewriter {
 public:
  Result<const SSAForm &> get () const;
  SSAHandle<SSAValue> definition (SSANode<Instruction> instruction, std::size_t operand) const;
  SSAHandle<SSAValue> initial (SSANode<Register> reg, SSAValue::Kind kind) const;
  SSAHandle<SSAPhi> phi (SSANode<BasicBlock> block, SSANode<Register> reg,
                         std::span<const std::optional<SSANode<BasicBlock>>> predecessors) const;
  SSAHandle<SSAPhi> phi (
    SSANode<BasicBlock> block, SSANode<Register> reg,
    std::initializer_list<std::optional<SSANode<BasicBlock>>> predecessors) const {
    return phi (block, reg, std::span (predecessors));
  }
  SSAHandle<SSAUse> input (SSANode<Instruction> instruction, std::size_t operand,
                           SSAOperandPart part, SSANode<SSAValue> value) const;
  SSAHandle<SSAUse> hardware_input (SSANode<Instruction> instruction, SSANode<Register> hardware,
                                    SSANode<SSAValue> value) const;
  SSAHandle<SSAUse> phi_input (SSANode<SSAPhi> phi, SSANode<SSAValue> value) const;
  void retarget (SSANode<SSAUse> use, SSANode<SSAValue> value) const;
  void replace_all_uses (SSANode<SSAValue> from, SSANode<SSAValue> to) const;
  void replace_uses (std::span<const SSANode<SSAUse>> uses, SSANode<SSAValue> value) const;
  void rename (SSANode<SSAValue> value, SSANode<Register> reg,
               bool preserve_addresses = false) const;
  SSAHandle<Register> fresh_register (SSANode<Register> reg) const;
  SSAHandle<Register> fresh_register (Type type) const;
  void replace (SSANode<Instruction> instruction, Opcode opcode, std::span<const Operand> operands,
                std::span<const SSANode<SSAValue>> inputs) const;
  void replace_memory (SSANode<Instruction> instruction, std::size_t operand, Memory memory,
                       std::optional<SSANode<SSAValue>> base,
                       std::optional<SSANode<SSAValue>> index) const;
  void set_memory_location (SSANode<Instruction> instruction, std::size_t operand,
                            OptionalRef<const MemoryLocation> location) const;
  SSAHandle<SSAValue> coalesce_copy (SSANode<Instruction> instruction) const;
  void erase_phi_input (SSANode<SSAPhi> phi, std::size_t input) const;
  void erase_phi (SSANode<SSAPhi> phi, SSANode<SSAValue> replacement) const;
  void erase_phi (SSANode<SSAPhi> phi) const;
  void erase_instruction (SSANode<Instruction> instruction) const;
  void move_before (SSANode<Instruction> instruction, SSANode<BasicBlock> block,
                    OptionalRef<const Instruction> before) const;
  SSAHandle<BasicBlock> split_fallthrough (SSANode<BasicBlock> source,
                                           SSANode<BasicBlock> destination) const;
  SSAHandle<Instruction> insert (SSANode<BasicBlock> block, OptionalRef<const Instruction> before,
                                 Opcode opcode, std::span<const Operand> operands,
                                 std::span<const SSANode<SSAValue>> inputs,
                                 OptionalRef<const SSAPhi> after_phi = {}) const;
  void mark_renamed () const;
  void mark_conventional () const;
  void set_phi_position (SSANode<SSAPhi> phi, OptionalRef<const Instruction> before) const;
  void complete () const;

 private:
  friend struct detail::BuildAccess;
  friend class MIRBuilder;
  explicit SSARewriter (detail::FunctionBuildState *state) : state_ (state) {}
  template <class T>
  T *resolve (SSANode<T>, std::string_view) const;
  template <class T>
  SSAHandle<T> handle (const T * = nullptr) const;
  detail::FunctionBuildState *state_;
};
}  // namespace mirnext::ir
