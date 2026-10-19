/* Licensed under the MIT License. Function-owned SSA metadata borrows canonical instructions. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <deque>

namespace mirnext::ir {
class SSAValue;
class SSAUse;
class SSAPhi;
class SSAUseRange {
 public:
  explicit SSAUseRange (const std::list<SSAUse *> &uses) : uses_ (uses) {}
  auto begin () const { return uses_.begin (); }
  auto end () const { return uses_.end (); }
  auto rbegin () const { return uses_.rbegin (); }
  auto rend () const { return uses_.rend (); }
  bool empty () const { return uses_.empty (); }
  std::size_t size () const { return uses_.size (); }
  SSAUse *front () const { return uses_.front (); }

 private:
  const std::list<SSAUse *> &uses_;
};
enum class SSAOperandPart { Register, MemoryBase, MemoryIndex };
struct SSAInstructionUse {
  const Instruction *instruction;
  std::size_t operand;
  SSAOperandPart part;
};
struct SSAPhiUse {
  const SSAPhi *phi;
  std::size_t input;
};
struct SSAHardwareUse {
  const Instruction *ret;
  const Register *hardware;
};
using SSAUseSite = std::variant<SSAInstructionUse, SSAPhiUse, SSAHardwareUse>;

class SSAValue {
 public:
  enum class Kind { Instruction, Phi, Entry, Undefined };
  class Key {
    friend class SSARewriter;
    Key () = default;
  };
  SSAValue (Key, const SSAForm &form, Kind kind, const Register &reg,
            const Instruction *instruction = nullptr, std::size_t operand = 0)
    : form_ (&form),
      kind_ (kind),
      origin_ (&reg),
      reg_ (&reg),
      instruction_ (instruction),
      operand_ (operand) {}
  SSAValue (const SSAValue &) = delete;
  SSAValue &operator= (const SSAValue &) = delete;
  const SSAForm &form () const { return *form_; }
  Kind kind () const { return kind_; }
  const Register &reg () const { return *reg_; }
  const Register &origin () const { return *origin_; }
  const Instruction *instruction () const { return instruction_; }
  std::size_t operand () const { return operand_; }
  const SSAPhi *phi () const { return phi_; }
  SSAUseRange uses () const { return SSAUseRange (uses_); }
  bool alive () const { return alive_; }
  const SSAValue *replacement () const { return replacement_; }

 private:
  friend class SSARewriter;
  const SSAForm *form_;
  Kind kind_;
  const Register *origin_, *reg_;
  const Instruction *instruction_;
  std::size_t operand_;
  const SSAPhi *phi_ = nullptr;
  std::list<SSAUse *> uses_;
  bool alive_ = true;
  SSAValue *replacement_ = nullptr;
};
class SSAUse {
 public:
  class Key {
    friend class SSARewriter;
    Key () = default;
  };
  SSAUse (Key, const SSAForm &form, SSAUseSite site, SSAValue &value)
    : form_ (&form), site_ (std::move (site)), value_ (&value) {}
  SSAUse (const SSAUse &) = delete;
  SSAUse &operator= (const SSAUse &) = delete;
  const SSAForm &form () const { return *form_; }
  const SSAUseSite &site () const { return site_; }
  const SSAValue &value () const { return *value_; }
  bool alive () const { return alive_; }

 private:
  friend class SSARewriter;
  friend class SSAForm;
  const SSAForm *form_;
  SSAUseSite site_;
  SSAValue *value_;
  std::list<SSAUse *>::iterator position_;
  bool alive_ = true;
};
class SSAPhi {
 public:
  class Key {
    friend class SSARewriter;
    Key () = default;
  };
  SSAPhi (Key, const SSAForm &form, const BasicBlock &block, SSAValue &output,
          std::vector<const BasicBlock *> predecessors)
    : form_ (&form), block_ (&block), output_ (&output), predecessors_ (std::move (predecessors)) {}
  SSAPhi (const SSAPhi &) = delete;
  SSAPhi &operator= (const SSAPhi &) = delete;
  const SSAForm &form () const { return *form_; }
  const BasicBlock &block () const { return *block_; }
  const SSAValue &output () const { return *output_; }
  std::span<SSAUse *const> inputs () const { return inputs_; }
  std::span<const BasicBlock *const> predecessors () const { return predecessors_; }
  bool alive () const { return output_->alive (); }
  const Instruction *before () const { return before_; }

 private:
  friend class SSARewriter;
  const SSAForm *form_;
  const BasicBlock *block_;
  SSAValue *output_;
  std::vector<const BasicBlock *> predecessors_;
  std::vector<SSAUse *> inputs_;
  const Instruction *before_ = nullptr;
};
class SSAForm {
 public:
  SSAForm (const SSAForm &) = delete;
  SSAForm &operator= (const SSAForm &) = delete;
  const Function &function () const { return function_; }
  auto values () const { return NodeRange (values_.cbegin (), values_.cend ()); }
  auto uses () const { return NodeRange (uses_.cbegin (), uses_.cend ()); }
  auto phis () const { return NodeRange (phis_.cbegin (), phis_.cend ()); }
  std::span<SSAPhi *const> phis (const BasicBlock &) const;
  std::span<SSAValue *const> definitions (const Instruction &) const;
  std::span<SSAUse *const> inputs (const Instruction &) const;
  const BasicBlock *use_block (const SSAUse &) const;
  bool renamed () const { return renamed_; }
  bool conventional () const { return conventional_; }
  bool current () const {
    // Def-use belongs to the function's body and entry topology. Unrelated
    // module data and symbol changes do not alter these owned relationships.
    auto current = function_.revisions ();
    return revisions_.instructions == current.instructions
           && revisions_.control_flow == current.control_flow
           && revisions_.registers == current.registers;
  }
  Result<void> verify () const;

 private:
  friend class SSARewriter;
  explicit SSAForm (Function &function)
    : function_ (function), revisions_ (function.revisions ()) {}
  Function &function_;
  Revisions revisions_;
  std::deque<SSAValue> values_;
  std::deque<SSAUse> uses_;
  std::deque<SSAPhi> phis_;
  std::unordered_map<const BasicBlock *, std::vector<SSAPhi *>> block_phis_;
  std::unordered_map<const Instruction *, std::vector<SSAValue *>> definitions_;
  std::unordered_map<const Instruction *, std::vector<SSAUse *>> inputs_;
  bool renamed_ = false, conventional_ = false;
};

// All SSA edits update canonical operands and both directions of def-use edges.
// Private PHIs and initial definitions never masquerade as public opcodes.
class SSARewriter {
 public:
  explicit SSARewriter (SSAForm &form) : form_ (form), rewriter_ (form.function_, true) {}
  static Result<std::unique_ptr<SSAForm>> create (Function &);
  static Result<void> install (std::unique_ptr<SSAForm>);
  static Result<SSARewriter> get (Function &);
  Result<SSAValue *> definition (Instruction &, std::size_t operand);
  Result<SSAValue *> initial (const Register &, SSAValue::Kind);
  Result<SSAPhi *> phi (const BasicBlock &, const Register &,
                        std::span<const BasicBlock *const> predecessors);
  Result<SSAUse *> input (Instruction &, std::size_t operand, SSAOperandPart, SSAValue &);
  Result<SSAUse *> hardware_input (Instruction &, const Register &, SSAValue &);
  Result<SSAUse *> phi_input (SSAPhi &, SSAValue &);
  Result<void> retarget (SSAUse &, SSAValue &);
  Result<void> replace_all_uses (SSAValue &, SSAValue &);
  Result<void> replace_uses (std::span<SSAUse *const>, SSAValue &);
  Result<void> replace_uses (SSAUseRange, SSAValue &);
  Result<void> rename (SSAValue &, const Register &, bool preserve_addresses = false);
  Result<const Register *> fresh_register (const Register &);
  Result<const Register *> fresh_register (Type);
  Result<void> replace (Instruction &, Opcode, std::span<const Operand>,
                        std::span<SSAValue *const> inputs);
  Result<void> replace_memory (Instruction &, std::size_t operand, Memory, SSAValue *base,
                               SSAValue *index);
  Result<void> set_memory_location (Instruction &, std::size_t, const MemoryLocation *);
  Result<SSAValue *> coalesce_copy (Instruction &);
  Result<void> erase_phi_input (SSAPhi &, std::size_t);
  Result<void> erase_phi (SSAPhi &, SSAValue &replacement);
  Result<void> erase_phi (SSAPhi &);
  Result<void> erase_instruction (Instruction &);
  Result<void> move_before (Instruction &, BasicBlock &, const Instruction *before = nullptr);
  Result<BasicBlock *> split_fallthrough (BasicBlock &source, BasicBlock &destination);
  Result<Instruction *> insert (BasicBlock &, const Instruction *before, Opcode,
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
  IRRewriter rewriter_;
};
bool ssa_output_operand (const Function &, const Instruction &, std::size_t);
std::string dump_ssa (const Function &);
}  // namespace mirnext::ir
