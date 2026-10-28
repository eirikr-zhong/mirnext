/* Licensed under the MIT License. Function-owned SSA metadata borrows canonical instructions. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include <deque>

namespace mirnext::ir {
namespace detail {
class NodeAccess;
}
class SSAValue;
class SSAUse;
class SSAPhi;
class SSAUseRange {
 public:
  explicit SSAUseRange (const std::list<SSAUse *> &uses) : uses_ (uses) {}
  auto begin () const {
    return NodeIterator<std::list<SSAUse *>::const_iterator, true> (uses_.begin ());
  }
  auto end () const {
    return NodeIterator<std::list<SSAUse *>::const_iterator, true> (uses_.end ());
  }
  auto rbegin () const { return std::make_reverse_iterator (end ()); }
  auto rend () const { return std::make_reverse_iterator (begin ()); }
  bool empty () const { return uses_.empty (); }
  std::size_t size () const { return uses_.size (); }
  const SSAUse &front () const { return *uses_.front (); }

 private:
  friend struct detail::ReferenceAccess;
  const std::list<SSAUse *> &uses_;
};
enum class SSAOperandPart { Register, MemoryBase, MemoryIndex };
struct SSAInstructionUse {
  SSAInstructionUse (const Instruction &instruction, std::size_t operand, SSAOperandPart part)
    : SSAInstructionUse (Key{}, &instruction, std::move (operand), std::move (part)) {}
  const Instruction &instruction () const { return *instruction_; }
  std::size_t operand;
  SSAOperandPart part;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  SSAInstructionUse (Key, const Instruction *instruction, std::size_t operand, SSAOperandPart part)
    : operand (operand), part (part), instruction_ (instruction) {}
  const Instruction *instruction_;
};
struct SSAPhiUse {
  SSAPhiUse (const SSAPhi &phi, std::size_t input) : SSAPhiUse (Key{}, &phi, std::move (input)) {}
  const SSAPhi &phi () const { return *phi_; }
  std::size_t input;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  SSAPhiUse (Key, const SSAPhi *phi, std::size_t input) : input (input), phi_ (phi) {}
  const SSAPhi *phi_;
};
struct SSAHardwareUse {
  SSAHardwareUse (const Instruction &ret, const Register &hardware)
    : SSAHardwareUse (Key{}, &ret, &hardware) {}
  const Instruction &ret () const { return *ret_; }
  const Register &hardware () const { return *hardware_; }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  SSAHardwareUse (Key, const Instruction *ret, const Register *hardware)
    : ret_ (ret), hardware_ (hardware) {}
  const Instruction *ret_;
  const Register *hardware_;
};
using SSAUseSite = std::variant<SSAInstructionUse, SSAPhiUse, SSAHardwareUse>;

class SSAValue {
 public:
  enum class Kind { Instruction, Phi, Entry, Undefined };
  class Key {
    friend class SSAUpdater;
    friend class SSAForm;
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
  OptionalRef<const Instruction> instruction () const {
    return ::mirnext::detail::borrow_optional (instruction_);
  }
  std::size_t operand () const { return operand_; }
  OptionalRef<const SSAPhi> phi () const { return ::mirnext::detail::borrow_optional (phi_); }
  SSAUseRange uses () const { return SSAUseRange (uses_); }
  bool alive () const { return alive_; }
  OptionalRef<const SSAValue> replacement () const {
    return ::mirnext::detail::borrow_optional (replacement_);
  }

 private:
  friend class SSAUpdater;
  friend class SSAForm;
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
    friend class SSAUpdater;
    friend class SSAForm;
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
  friend class SSAUpdater;
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
    friend class SSAUpdater;
    friend class SSAForm;
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
  ReferenceRange<SSAUse> inputs () const { return ReferenceRange<SSAUse> (inputs_); }
  OptionalReferenceRange<const BasicBlock> predecessors () const {
    return OptionalReferenceRange<const BasicBlock> (predecessors_);
  }
  bool alive () const { return output_->alive (); }
  OptionalRef<const Instruction> before () const {
    return ::mirnext::detail::borrow_optional (before_);
  }

 private:
  friend class SSAUpdater;
  friend class SSAForm;
  const SSAForm *form_;
  const BasicBlock *block_;
  SSAValue *output_;
  std::vector<const BasicBlock *> predecessors_;
  std::vector<SSAUse *> inputs_;
  const Instruction *before_ = nullptr;
};
class SSAForm {
 public:
  SSAForm (SSAForm &&) noexcept;
  SSAForm &operator= (SSAForm &&) noexcept;
  SSAForm (const SSAForm &) = delete;
  SSAForm &operator= (const SSAForm &) = delete;
  const Function &function () const { return function_; }
  auto values () const { return NodeRange (values_.cbegin (), values_.cend (), values_.size ()); }
  auto uses () const { return NodeRange (uses_.cbegin (), uses_.cend (), uses_.size ()); }
  auto phis () const { return NodeRange (phis_.cbegin (), phis_.cend (), phis_.size ()); }
  ReferenceRange<SSAPhi> phis (const BasicBlock &) const;
  ReferenceRange<SSAValue> definitions (const Instruction &) const;
  ReferenceRange<SSAUse> inputs (const Instruction &) const;
  OptionalRef<const BasicBlock> use_block (const SSAUse &) const;
  // Distinguishes successive SSA phases even when node storage is reused.
  std::uint64_t phase_identity () const { return identity_; }
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
  friend class SSAUpdater;
  SSAForm (Function &function, std::uint64_t identity)
    : function_ (function), identity_ (identity), revisions_ (function.revisions ()) {}
  Function &function_;
  std::uint64_t identity_;
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

bool ssa_output_operand (const Function &, const Instruction &, std::size_t);
std::string dump_ssa (const Function &);
}  // namespace mirnext::ir
