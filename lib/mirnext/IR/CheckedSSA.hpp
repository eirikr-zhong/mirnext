/* Licensed under the MIT License. Private SSA def-use maintenance. */
#pragma once
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/SSA.hpp"
namespace mirnext::ir {
class SSAUpdater {
 public:
  explicit SSAUpdater (SSAForm &form) : form_ (form), rewriter_ (form.function_, true) {}
  static Result<SSAForm> create (Function &);
  static Result<void> install (SSAForm &&);
  static Result<void> install (std::unique_ptr<SSAForm>);
  static Result<SSAUpdater> get (Function &);
  Result<SSAValue &> definition (Instruction &, std::size_t operand);
  Result<SSAValue &> initial (const Register &, SSAValue::Kind);
  Result<SSAPhi &> phi (const BasicBlock &, const Register &,
                        std::span<const BasicBlock *const> predecessors);
  Result<SSAUse &> input (Instruction &, std::size_t operand, SSAOperandPart, SSAValue &);
  Result<SSAUse &> hardware_input (Instruction &, const Register &, SSAValue &);
  Result<SSAUse &> phi_input (SSAPhi &, SSAValue &);
  Result<void> retarget (SSAUse &, SSAValue &);
  Result<void> replace_all_uses (SSAValue &, SSAValue &);
  Result<void> replace_uses (std::span<SSAUse *const>, SSAValue &);
  Result<void> replace_uses (const std::list<SSAUse *> &, SSAValue &);
  Result<void> rename (SSAValue &, const Register &, bool preserve_addresses = false);
  Result<const Register &> fresh_register (const Register &);
  Result<const Register &> fresh_register (Type);
  Result<void> replace (Instruction &, Opcode, std::span<const Operand>,
                        std::span<SSAValue *const> inputs);
  Result<void> replace_memory (Instruction &, std::size_t operand, Memory, SSAValue *base,
                               SSAValue *index);
  Result<void> set_memory_location (Instruction &, std::size_t, const MemoryLocation *);
  Result<SSAValue &> coalesce_copy (Instruction &);
  Result<void> erase_phi_input (SSAPhi &, std::size_t);
  Result<void> erase_phi (SSAPhi &, SSAValue &replacement);
  Result<void> erase_phi (SSAPhi &);
  Result<void> erase_instruction (Instruction &);
  Result<void> move_before (Instruction &, BasicBlock &, const Instruction *before = nullptr);
  Result<BasicBlock &> split_fallthrough (BasicBlock &source, BasicBlock &destination);
  Result<Instruction &> insert (BasicBlock &, const Instruction *before, Opcode,
                                std::span<const Operand>, std::span<SSAValue *const> inputs,
                                const SSAPhi *after_phi = nullptr);
  Result<void> mark_renamed ();
  Result<void> mark_conventional ();
  Result<void> set_phi_position (SSAPhi &, const Instruction *before);
  Result<void> complete ();

 private:
  Result<void> writable () const;
  void touched ();
  SSAUse &add_use (SSAUseSite, SSAValue &);
  void unlink (SSAUse &);
  void redirect (SSAUse &, SSAValue &);
  Result<void> rewrite (std::span<SSAUse *const>, const Register &, bool preserve_addresses,
                        const SSAValue *definition = nullptr);
  SSAForm &form_;
  InstructionRewriter rewriter_;
};
}  // namespace mirnext::ir
