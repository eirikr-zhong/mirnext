#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
/* Licensed under the MIT License. Stable SSA identities, checked edits and private phase
 * verification. */
#include "mirnext/IR/SSA.hpp"
#include <algorithm>
#include <sstream>
#include <unordered_set>

namespace mirnext::ir {
namespace {
Error invalid (const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument,
          std::string (function.symbol ().name ()) + ": SSA: " + std::move (reason)};
}
bool address (Opcode opcode) {
  return opcode == Opcode::Addr || opcode == Opcode::Addr8 || opcode == Opcode::Addr16
         || opcode == Opcode::Addr32;
}
const Register *input_register (const Function &function, const Instruction &instruction,
                                std::size_t p, SSAOperandPart part) {
  if (p >= instruction.operands ().size ()) return nullptr;
  if (part != SSAOperandPart::Register && part != SSAOperandPart::MemoryBase
      && part != SSAOperandPart::MemoryIndex)
    return nullptr;
  auto &operand = instruction.operands ()[p];
  if (part == SSAOperandPart::Register) {
    auto reg = detail::operand_access (operand).get<const Register *> ();
    return reg && !ssa_output_operand (function, instruction, p) ? *reg : nullptr;
  }
  if (auto memory = detail::operand_access (operand).get<Memory> ())
    return part == SSAOperandPart::MemoryBase ? detail::NodeAccess::base (*(memory))
                                              : detail::NodeAccess::index (*(memory));
  return nullptr;
}
Type scalar (Type type) {
  if (sizeof (long double) == sizeof (double) && type.kind () == Type::Kind::LongDouble)
    return Type (Type::Kind::F64);
  return type.is_float () ? type : Type::i64 ();
}
struct Input {
  std::size_t operand;
  SSAOperandPart part;
  const Register *reg;
};
std::vector<Input> input_positions (const Function &function, const Instruction &instruction) {
  std::vector<Input> result;
  for (std::size_t p = 0; p < instruction.operands ().size (); ++p) {
    if (auto reg = input_register (function, instruction, p, SSAOperandPart::Register))
      result.push_back ({p, SSAOperandPart::Register, reg});
    else if (auto memory = detail::operand_access (instruction.operands ()[p]).get<Memory> ()) {
      if (detail::NodeAccess::base (*(memory)))
        result.push_back ({p, SSAOperandPart::MemoryBase, detail::NodeAccess::base (*(memory))});
      if (detail::NodeAccess::index (*(memory)))
        result.push_back ({p, SSAOperandPart::MemoryIndex, detail::NodeAccess::index (*(memory))});
    }
  }
  return result;
}
}  // namespace
bool ssa_output_operand (const Function &function, const Instruction &instruction, std::size_t p) {
  return instruction.opcode () != Opcode::LAddr && output_operand (function, instruction, p);
}
SSAForm::SSAForm (SSAForm &&other) noexcept
  : function_ (other.function_),
    identity_ (other.identity_),
    revisions_ (std::move (other.revisions_)),
    values_ (std::move (other.values_)),
    uses_ (std::move (other.uses_)),
    phis_ (std::move (other.phis_)),
    block_phis_ (std::move (other.block_phis_)),
    definitions_ (std::move (other.definitions_)),
    inputs_ (std::move (other.inputs_)),
    renamed_ (std::move (other.renamed_)),
    conventional_ (std::move (other.conventional_)) {
  for (auto &value : values_) value.form_ = this;
  for (auto &use : uses_) use.form_ = this;
  for (auto &phi : phis_) phi.form_ = this;
}
SSAForm &SSAForm::operator= (SSAForm &&other) noexcept {
  if (this != &other) {
    this->~SSAForm ();
    std::construct_at (this, std::move (other));
  }
  return *this;
}
ReferenceRange<SSAPhi> SSAForm::phis (const BasicBlock &block) const {
  auto found = block_phis_.find (&block);
  return found == block_phis_.end () ? ReferenceRange<SSAPhi>{}
                                     : ReferenceRange<SSAPhi> (found->second);
}
ReferenceRange<SSAValue> SSAForm::definitions (const Instruction &instruction) const {
  auto found = definitions_.find (&instruction);
  return found == definitions_.end () ? ReferenceRange<SSAValue>{}
                                      : ReferenceRange<SSAValue> (found->second);
}
ReferenceRange<SSAUse> SSAForm::inputs (const Instruction &instruction) const {
  auto found = inputs_.find (&instruction);
  return found == inputs_.end () ? ReferenceRange<SSAUse>{}
                                 : ReferenceRange<SSAUse> (found->second);
}
OptionalRef<const BasicBlock> SSAForm::use_block (const SSAUse &use) const {
  return ::mirnext::detail::borrow_optional (std::visit (
    [] (const auto &site) -> const BasicBlock * {
      using T = std::decay_t<decltype (site)>;
      if constexpr (std::is_same_v<T, SSAInstructionUse>)
        return &detail::NodeAccess::instruction (site)->block ();
      else if constexpr (std::is_same_v<T, SSAHardwareUse>)
        return &detail::NodeAccess::ret (site)->block ();
      else
        return &detail::NodeAccess::phi (site)->block ();
    },
    use.site ()));
}
Result<void> SSAForm::verify () const {
  if (!current ()) return invalid (function_, "phase metadata is stale");
  std::unordered_set<const Instruction *> instructions;
  std::unordered_set<const BasicBlock *> blocks;
  std::unordered_set<const Register *> registers;
  std::unordered_set<const SSAValue *> values;
  std::unordered_set<const SSAUse *> uses;
  std::unordered_set<const SSAPhi *> phis;
  instructions.reserve (function_.instruction_count ());
  registers.reserve (std::ranges::distance (function_.registers ()));
  values.reserve (values_.size ());
  uses.reserve (uses_.size ());
  phis.reserve (phis_.size ());
  std::vector<const Register *> hardware_registers;
  for (auto &block : function_.blocks ()) {
    blocks.insert (&block);
    for (auto &instruction : block.instructions ()) instructions.insert (&instruction);
  }
  for (auto &reg : function_.registers ()) {
    registers.insert (&reg);
    if (!reg.hardware_binding ().empty () && !reg.is_memory_backed ())
      hardware_registers.push_back (&reg);
  }
  for (auto &value : values_)
    if (value.alive ()) values.insert (&value);
  for (auto &use : uses_)
    if (use.alive ()) uses.insert (&use);
  for (auto &phi : phis_)
    if (phi.alive ()) phis.insert (&phi);
  for (auto value : values) {
    if (&value->form () != this || !registers.contains (&value->reg ())
        || !registers.contains (&value->origin ()))
      return invalid (function_, "definition crosses ownership");
    if (value->kind () == SSAValue::Kind::Instruction) {
      auto instruction = value->instruction ().get ();
      if (!instructions.contains (instruction)
          || value->operand () >= instruction->operands ().size ()
          || !ssa_output_operand (function_, *instruction, value->operand ()))
        return invalid (function_, "invalid instruction definition");
      auto reg = detail::operand_access (instruction->operands ()[value->operand ()])
                   .get<const Register *> ();
      if (!reg || *reg != &value->reg ())
        return invalid (function_, "definition register differs from canonical operand");
    } else if (value->kind () == SSAValue::Kind::Phi) {
      if (!phis.contains (value->phi ().get ()) || &value->phi ().get ()->output () != value)
        return invalid (function_, "invalid PHI definition");
    } else if (value->instruction ().get () || value->phi ().get ())
      return invalid (function_, "initial value has a body node");
    for (auto it = detail::ReferenceAccess::pointers (value->uses ()).begin ();
         it != detail::ReferenceAccess::pointers (value->uses ()).end (); ++it) {
      auto use = *it;
      if (!uses.contains (use) || &use->value () != value || use->position_ != it)
        return invalid (function_, "invalid inverse use edge");
    }
  }
  for (auto phi : phis) {
    if (&phi->form () != this || !blocks.contains (&phi->block ())
        || detail::ReferenceAccess::pointers (phi->inputs ()).size ()
             != detail::ReferenceAccess::pointers (phi->predecessors ()).size ())
      return invalid (function_, "invalid PHI predecessor list");
    if (phi->before ().get ()
        && (!instructions.contains (phi->before ().get ())
            || &phi->before ().get ()->block () != &phi->block ()))
      return invalid (function_, "invalid PHI position");
    for (std::size_t p = 0; p < detail::ReferenceAccess::pointers (phi->inputs ()).size (); ++p) {
      auto use = detail::ReferenceAccess::pointers (phi->inputs ())[p];
      if (!uses.contains (use)) return invalid (function_, "missing PHI edge");
      auto site = std::get_if<SSAPhiUse> (&use->site ());
      if (!site || detail::NodeAccess::phi (*(site)) != phi || site->input != p)
        return invalid (function_, "PHI edge position differs");
      auto predecessor = detail::ReferenceAccess::pointers (phi->predecessors ())[p];
      if (predecessor && !blocks.contains (predecessor))
        return invalid (function_, "PHI predecessor crosses ownership");
      if (scalar (use->value ().reg ().type ()) != scalar (phi->output ().reg ().type ()))
        return invalid (function_, "PHI input type differs");
      if (conventional_ && &use->value ().reg () != &phi->output ().reg ())
        return invalid (function_, "conventional PHI register differs");
    }
  }
  std::unordered_set<const SSAPhi *> indexed_phis;
  for (auto &[block, ordered] : block_phis_)
    for (auto phi : ordered)
      if (!phis.contains (phi) || &phi->block () != block || !indexed_phis.insert (phi).second)
        return invalid (function_, "invalid ordered PHI list");
  if (indexed_phis.size () != phis.size ())
    return invalid (function_, "PHI is missing from layout");
  for (auto use : uses) {
    if (&use->form () != this || !values.contains (&use->value ()) || *use->position_ != use)
      return invalid (function_, "invalid definition edge");
    auto valid = std::visit (
      [&] (const auto &site) {
        using T = std::decay_t<decltype (site)>;
        if constexpr (std::is_same_v<T, SSAInstructionUse>) {
          if (!instructions.contains (detail::NodeAccess::instruction (site))) return false;
          auto reg = input_register (function_, *detail::NodeAccess::instruction (site),
                                     site.operand, site.part);
          return reg == &use->value ().reg ()
                 || (address (detail::NodeAccess::instruction (site)->opcode ())
                     && site.operand == 1 && reg == &use->value ().origin ());
        } else if constexpr (std::is_same_v<T, SSAPhiUse>) {
          return phis.contains (detail::NodeAccess::phi (site))
                 && site.input < detail::NodeAccess::phi (site)->inputs ().size ()
                 && detail::ReferenceAccess::pointers (
                      detail::NodeAccess::phi (site)->inputs ())[site.input]
                      == use;
        } else {
          return instructions.contains (detail::NodeAccess::ret (site))
                 && detail::NodeAccess::ret (site)->opcode () == Opcode::Ret
                 && registers.contains (detail::NodeAccess::hardware (site))
                 && !detail::NodeAccess::hardware (site)->hardware_binding ().empty ()
                 && !detail::NodeAccess::hardware (site)->is_memory_backed ()
                 && use->value ().reg ().hardware_binding ()
                      == detail::NodeAccess::hardware (site)->hardware_binding ();
        }
      },
      use->site ());
    if (!valid) return invalid (function_, "use differs from canonical operand or boundary");
  }
  // A byte tracks the output and all three input sites of each operand. Reuse
  // scratch across instructions; wide calls need a linear scan, not one scan of
  // the definition list per result. Nothing is cached between verifications.
  std::vector<unsigned char> sites;
  std::unordered_set<const Register *> hardware;
  hardware.reserve (hardware_registers.size ());
  for (auto instruction : instructions) {
    sites.assign (instruction->operands ().size (), 0);
    hardware.clear ();
    for (std::size_t p = 0; p < instruction->operands ().size (); ++p) {
      auto &operand = instruction->operands ()[p];
      if (detail::operand_access (operand).get<const Register *> ())
        sites[p] = ssa_output_operand (function_, *instruction, p) ? 1 : 2;
      else if (auto memory = detail::operand_access (operand).get<Memory> ())
        sites[p] = (detail::NodeAccess::base (*(memory)) ? 4 : 0)
                   | (detail::NodeAccess::index (*(memory)) ? 8 : 0);
    }
    for (auto definition : detail::ReferenceAccess::pointers (definitions (*instruction))) {
      if (!values.contains (definition) || definition->instruction ().get () != instruction
          || definition->operand () >= sites.size ())
        return invalid (function_, "extra instruction definition");
      auto &site = sites[definition->operand ()];
      if (!(site & 1)) return invalid (function_, "instruction output is missing or duplicated");
      site &= ~1u;
    }
    for (auto site : sites)
      if (site & 1) return invalid (function_, "instruction output is missing or duplicated");
    auto edges = detail::ReferenceAccess::pointers (inputs (*instruction));
    for (auto edge : edges) {
      if (!uses.contains (edge)) return invalid (function_, "invalid instruction input index");
      if (auto site = std::get_if<SSAInstructionUse> (&edge->site ())) {
        if (detail::NodeAccess::instruction (*(site)) != instruction
            || site->operand >= sites.size ())
          return invalid (function_, "invalid instruction input position");
        auto flag = 2u << static_cast<unsigned> (site->part);
        if (!(sites[site->operand] & flag))
          return invalid (function_, "duplicate instruction input");
        sites[site->operand] &= ~flag;
      } else if (auto site = std::get_if<SSAHardwareUse> (&edge->site ())) {
        if (detail::NodeAccess::ret (*(site)) != instruction
            || !hardware.insert (detail::NodeAccess::hardware (*(site))).second)
          return invalid (function_, "duplicate return boundary input");
      } else
        return invalid (function_, "PHI edge indexed as an ordinary input");
    }
    for (auto site : sites)
      if (site) return invalid (function_, "instruction input is missing");
    if (instruction->opcode () == Opcode::Ret)
      for (auto reg : hardware_registers)
        if (!hardware.contains (reg))
          return invalid (function_, "missing hardware return boundary");
  }
  return {};
}
Result<void> SSAUpdater::writable () const {
  if (!form_.current ()) return invalid (form_.function_, "phase metadata is stale");
  if (form_.function_.ssa_form ().get () && form_.function_.ssa_form ().get () != &form_)
    return invalid (form_.function_, "another SSA phase owns the function");
  return {};
}
Result<void> SSAUpdater::set_memory_location (Instruction &i, std::size_t operand,
                                              const MemoryLocation *location) {
  auto valid = writable ();
  if (!(valid).has_value ()) return valid;
  valid = rewriter_.set_memory_location (i, operand, location);
  if (!(valid).has_value ()) return valid;
  touched ();
  return {};
}
void SSAUpdater::touched () {
  if (form_.function_.ssa_form ().get () == &form_) form_.function_.changed (false);
  form_.revisions_ = form_.function_.revisions ();
}
Result<SSAForm> SSAUpdater::create (Function &function) {
  auto checked = function.require_register_ir ();
  if (!(checked).has_value ()) return checked.error ();
  checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  return SSAForm (function, ++function.next_ssa_identity_);
}
Result<void> SSAUpdater::install (SSAForm &&form) {
  return install (std::make_unique<SSAForm> (std::move (form)));
}
Result<void> SSAUpdater::install (std::unique_ptr<SSAForm> form) {
  if (!form) return Error{ErrorCode::InvalidArgument, "missing SSA form"};
  auto &function = form->function_;
  auto checked = function.require_register_ir ();
  if (!(checked).has_value ()) return checked;
  checked = form->verify ();
  if (!(checked).has_value ()) return checked;
  function.ssa_ = std::move (form);
  function.changed (false);
  function.ssa_->revisions_ = function.revisions ();
  return {};
}
Result<SSAUpdater> SSAUpdater::get (Function &function) {
  if (!function.ssa_) return invalid (function, "function is not in SSA");
  auto checked = function.ssa_->verify ();
  if (!(checked).has_value ()) return checked.error ();
  return SSAUpdater (*function.ssa_);
}
Result<SSAValue &> SSAUpdater::definition (Instruction &instruction, std::size_t p) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&instruction.function () != &form_.function_ || p >= instruction.operands ().size ())
    return invalid (form_.function_, "invalid output site");
  auto reg = detail::operand_access (instruction.operands ()[p]).get<const Register *> ();
  if (!reg || !ssa_output_operand (form_.function_, instruction, p))
    return invalid (form_.function_, "site is not a register output");
  auto &defs = form_.definitions_[&instruction];
  for (auto value : defs)
    if (value->operand () == p) return invalid (form_.function_, "duplicate output site");
  form_.values_.emplace_back (SSAValue::Key{}, form_, SSAValue::Kind::Instruction, **reg,
                              &instruction, p);
  auto value = &form_.values_.back ();
  defs.push_back (value);
  touched ();
  return *(value);
}
Result<SSAValue &> SSAUpdater::initial (const Register &reg, SSAValue::Kind kind) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&reg.function () != &form_.function_
      || (kind != SSAValue::Kind::Entry && kind != SSAValue::Kind::Undefined))
    return invalid (form_.function_, "invalid initial definition");
  form_.values_.emplace_back (SSAValue::Key{}, form_, kind, reg);
  auto value = &form_.values_.back ();
  touched ();
  return *(value);
}
Result<SSAPhi &> SSAUpdater::phi (const BasicBlock &block, const Register &reg,
                                  std::span<const BasicBlock *const> predecessors) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&block.function () != &form_.function_ || &reg.function () != &form_.function_)
    return invalid (form_.function_, "PHI crosses ownership");
  for (auto p : predecessors)
    if (p && &p->function () != &form_.function_)
      return invalid (form_.function_, "PHI predecessor crosses ownership");
  form_.values_.emplace_back (SSAValue::Key{}, form_, SSAValue::Kind::Phi, reg);
  auto value = &form_.values_.back ();
  form_.phis_.emplace_back (SSAPhi::Key{}, form_, block, *value,
                            std::vector<const BasicBlock *> (predecessors.begin (),
                                                             predecessors.end ()));
  auto phi = &form_.phis_.back ();
  value->phi_ = phi;
  if (block.size ()) phi->before_ = &*block.instructions ().begin ();
  auto &ordered = form_.block_phis_[&block];
  ordered.insert (ordered.begin (), phi);
  touched ();
  return *(phi);
}
SSAUse &SSAUpdater::add_use (SSAUseSite site, SSAValue &value) {
  form_.uses_.emplace_back (SSAUse::Key{}, form_, std::move (site), value);
  auto &use = form_.uses_.back ();
  value.uses_.push_front (&use);
  use.position_ = value.uses_.begin ();
  return use;
}
Result<SSAUse &> SSAUpdater::input (Instruction &instruction, std::size_t p, SSAOperandPart part,
                                    SSAValue &value) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&instruction.function () != &form_.function_ || &value.form () != &form_ || !value.alive ())
    return invalid (form_.function_, "input crosses ownership");
  auto reg = input_register (form_.function_, instruction, p, part);
  if (reg != &value.reg ()
      && !(reg == &value.origin () && address (instruction.opcode ()) && p == 1))
    return invalid (form_.function_, "input register differs");
  auto &inputs = form_.inputs_[&instruction];
  for (auto use : inputs)
    if (auto site = std::get_if<SSAInstructionUse> (&use->site ());
        site && site->operand == p && site->part == part)
      return invalid (form_.function_, "duplicate input site");
  auto &use = add_use (detail::NodeAccess::make_SSAInstructionUse (&instruction, p, part), value);
  inputs.push_back (&use);
  touched ();
  return use;
}
Result<SSAUse &> SSAUpdater::hardware_input (Instruction &ret, const Register &reg,
                                             SSAValue &value) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&ret.function () != &form_.function_ || ret.opcode () != Opcode::Ret
      || &reg.function () != &form_.function_ || &value.form () != &form_ || !value.alive ()
      || reg.hardware_binding ().empty ()
      || value.reg ().hardware_binding () != reg.hardware_binding ())
    return invalid (form_.function_, "invalid hardware return boundary");
  auto &inputs = form_.inputs_[&ret];
  for (auto use : inputs)
    if (auto site = std::get_if<SSAHardwareUse> (&use->site ());
        site && detail::NodeAccess::hardware (*(site)) == &reg)
      return invalid (form_.function_, "duplicate hardware return boundary");
  auto &use = add_use (detail::NodeAccess::make_SSAHardwareUse (&ret, &reg), value);
  inputs.push_back (&use);
  touched ();
  return use;
}
Result<SSAUse &> SSAUpdater::phi_input (SSAPhi &phi, SSAValue &value) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&phi.form () != &form_ || !phi.alive () || &value.form () != &form_ || !value.alive ()
      || phi.inputs_.size () >= phi.predecessors_.size ()
      || scalar (value.reg ().type ()) != scalar (phi.output ().reg ().type ()))
    return invalid (form_.function_, "invalid PHI input");
  auto &use = add_use (detail::NodeAccess::make_SSAPhiUse (&phi, phi.inputs_.size ()), value);
  phi.inputs_.push_back (&use);
  touched ();
  return use;
}
void SSAUpdater::unlink (SSAUse &use) { use.value_->uses_.erase (use.position_); }
Result<void> SSAUpdater::rewrite (std::span<SSAUse *const> uses, const Register &reg,
                                  bool preserve_addresses, const SSAValue *definition) {
  if (&reg.function () != &form_.function_)
    return invalid (form_.function_, "replacement register crosses ownership");
  std::unordered_map<const Instruction *, std::size_t> indices;
  struct Edit {
    Instruction *instruction;
    std::vector<Operand> operands;
  };
  std::vector<Edit> edits;
  auto operands = [&] (const Instruction &instruction) -> std::vector<Operand> & {
    auto [index, added] = indices.emplace (&instruction, edits.size ());
    if (added)
      edits.push_back ({const_cast<Instruction *> (&instruction),
                        {instruction.operands ().begin (), instruction.operands ().end ()}});
    return edits[index->second].operands;
  };
  if (definition && definition->instruction ().get () && &definition->reg () != &reg)
    operands (*definition->instruction ().get ())[definition->operand ()] = Operand (reg);
  for (auto use : uses) {
    if (!use || &use->form () != &form_ || !use->alive ())
      return invalid (form_.function_, "invalid replacement use");
    if (auto site = std::get_if<SSAInstructionUse> (&use->site ())) {
      if (preserve_addresses && address (detail::NodeAccess::instruction (*(site))->opcode ())
          && site->operand == 1)
        continue;
      if (input_register (form_.function_, *detail::NodeAccess::instruction (*(site)),
                          site->operand, site->part)
          == &reg)
        continue;
      auto &op = operands (*detail::NodeAccess::instruction (*(site)))[site->operand];
      if (site->part == SSAOperandPart::Register)
        op = Operand (reg);
      else {
        auto memory = *detail::operand_access (op).get<Memory> ();
        if (site->part == SSAOperandPart::MemoryBase)
          detail::NodeAccess::base (memory) = &reg;
        else
          detail::NodeAccess::index (memory) = &reg;
        op = Operand (std::move (memory));
      }
    } else if (auto site = std::get_if<SSAPhiUse> (&use->site ());
               site && form_.conventional_
               && &reg != &detail::NodeAccess::phi (*(site))->output ().reg ()) {
      return invalid (form_.function_, "replacement breaks conventional PHI");
    } else if (auto hardware = std::get_if<SSAHardwareUse> (&use->site ());
               hardware
               && reg.hardware_binding ()
                    != detail::NodeAccess::hardware (*(hardware))->hardware_binding ())
      return invalid (form_.function_, "replacement loses a hardware return effect");
  }
  for (auto &edit : edits) {
    auto checked
      = validate_instruction (form_.function_, edit.instruction->opcode (), edit.operands);
    if (!(checked).has_value ()) return checked;
  }
  for (auto &edit : edits) {
    auto checked
      = rewriter_.replace (*edit.instruction, edit.instruction->opcode (), edit.operands);
    if (!(checked).has_value ()) return checked;
  }
  form_.revisions_ = form_.function_.revisions ();
  return {};
}
void SSAUpdater::redirect (SSAUse &use, SSAValue &value) {
  value.uses_.splice (value.uses_.begin (), use.value_->uses_, use.position_);
  use.value_ = &value;
}
Result<void> SSAUpdater::retarget (SSAUse &use, SSAValue &value) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&use.form () != &form_ || !use.alive () || &value.form () != &form_ || !value.alive ()
      || scalar (use.value ().reg ().type ()) != scalar (value.reg ().type ()))
    return invalid (form_.function_, "invalid replacement definition");
  if (&use.value () == &value) return {};
  SSAUse *single[]{&use};
  checked = rewrite (single, value.reg (), false);
  if (!(checked).has_value ()) return checked;
  form_.renamed_ |= &use.value ().reg () != &value.reg ();
  redirect (use, value);
  touched ();
  return {};
}
Result<void> SSAUpdater::replace_all_uses (SSAValue &from, SSAValue &to) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&from.form () != &form_ || &to.form () != &form_ || !from.alive () || !to.alive ()
      || scalar (from.reg ().type ()) != scalar (to.reg ().type ()))
    return invalid (form_.function_, "invalid replacement definitions");
  if (&from == &to) return {};
  std::vector<SSAUse *> pending (from.uses_.begin (), from.uses_.end ());
  checked = rewrite (pending, to.reg (), false);
  if (!(checked).has_value ()) return checked;
  form_.renamed_ |= &from.reg () != &to.reg ();
  for (auto use : pending) use->value_ = &to;
  to.uses_.splice (to.uses_.end (), from.uses_);
  touched ();
  return {};
}
Result<void> SSAUpdater::replace_uses (const std::list<SSAUse *> &uses, SSAValue &to) {
  std::vector<SSAUse *> pending (uses.begin (), uses.end ());
  return replace_uses (pending, to);
}
Result<void> SSAUpdater::replace_uses (std::span<SSAUse *const> uses, SSAValue &to) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&to.form () != &form_ || !to.alive ())
    return invalid (form_.function_, "invalid replacement definition");
  for (auto use : uses)
    if (!use || &use->form () != &form_ || !use->alive ()
        || scalar (use->value ().reg ().type ()) != scalar (to.reg ().type ()))
      return invalid (form_.function_, "invalid replacement use type");
  checked = rewrite (uses, to.reg (), false);
  if (!(checked).has_value ()) return checked;
  // Snapshot callers' borrowed ranges before modifying inverse-use chains.
  std::vector<SSAUse *> pending (uses.begin (), uses.end ());
  for (auto use : pending)
    if (&use->value () != &to) {
      form_.renamed_ |= &use->value ().reg () != &to.reg ();
      redirect (*use, to);
    }
  touched ();
  return {};
}
Result<void> SSAUpdater::rename (SSAValue &value, const Register &reg, bool preserve_addresses) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&value.form () != &form_ || !value.alive () || &reg.function () != &form_.function_
      || scalar (value.reg ().type ()) != scalar (reg.type ())
      || value.reg ().hardware_binding () != reg.hardware_binding ())
    return invalid (form_.function_, "invalid definition register");
  if (&value.reg () == &reg) return {};
  if (value.phi ().get () && form_.conventional_)
    for (auto use : detail::ReferenceAccess::pointers (value.phi ().get ()->inputs ()))
      if (&use->value ().reg () != &reg)
        return invalid (form_.function_, "rename breaks conventional PHI");
  std::vector<SSAUse *> uses (value.uses_.begin (), value.uses_.end ());
  checked = rewrite (uses, reg, preserve_addresses, &value);
  if (!(checked).has_value ()) return checked;
  value.reg_ = &reg;
  form_.renamed_ = true;
  touched ();
  return {};
}
Result<const Register &> SSAUpdater::fresh_register (const Register &reg) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&reg.function () != &form_.function_)
    return invalid (form_.function_, "temporary register crosses ownership");
  if (!reg.hardware_binding ().empty ()) return reg;
  auto created = form_.function_.add_register (reg.type ().is_block () ? Type::i64 () : reg.type (),
                                               {}, {}, false);
  if (!(created).has_value ()) return created.error ();
  touched ();
  return *(static_cast<const Register *> (&(created).value ()));
}
Result<const Register &> SSAUpdater::fresh_register (Type type) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  auto created = form_.function_.add_register (type, {}, {}, false);
  if (!(created).has_value ()) return created.error ();
  touched ();
  return *(static_cast<const Register *> (&(created).value ()));
}
Result<void> SSAUpdater::replace (Instruction &instruction, Opcode opcode,
                                  std::span<const Operand> operands,
                                  std::span<SSAValue *const> inputs) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&instruction.function () != &form_.function_)
    return invalid (form_.function_, "replacement crosses ownership");
  checked = validate_instruction (form_.function_, opcode, operands);
  if (!(checked).has_value ()) return checked;
  auto output = [&] (std::size_t p) {
    const auto descriptor = instruction_descriptor (opcode).get ();
    if (descriptor->count == OperandCount::Call)
      return p >= 2
             && p - 2 < detail::NodeAccess::symbol (
                          *(detail::operand_access (operands[0]).get<SymbolReference> ()))
                          ->signature ()
                          .get ()
                          ->results.size ();
    return descriptor->count == OperandCount::Fixed && p < descriptor->roles.size ()
           && descriptor->roles[p] >= 'A' && descriptor->roles[p] <= 'Z' && opcode != Opcode::LAddr;
  };
  auto definitions = detail::ReferenceAccess::pointers (form_.definitions (instruction));
  std::size_t outputs = 0, count = 0;
  for (std::size_t p = 0; p < operands.size (); ++p) {
    if (auto reg = detail::operand_access (operands[p]).get<const Register *> ();
        reg && output (p)) {
      bool found = false;
      for (auto value : definitions)
        if (value->operand () == p && &value->reg () == *reg) found = true;
      if (!found) return invalid (form_.function_, "replacement changes output definitions");
      ++outputs;
      continue;
    }
    auto input = [&] (const Register *reg) {
      if (!reg) return true;
      if (count >= inputs.size () || !inputs[count] || &inputs[count]->form () != &form_
          || !inputs[count]->alive () || &inputs[count]->reg () != reg)
        return false;
      ++count;
      return true;
    };
    if (auto reg = detail::operand_access (operands[p]).get<const Register *> ();
        reg && !input (*reg))
      return invalid (form_.function_, "replacement input differs");
    if (auto memory = detail::operand_access (operands[p]).get<Memory> ();
        memory
        && (!input (detail::NodeAccess::base (*(memory)))
            || !input (detail::NodeAccess::index (*(memory)))))
      return invalid (form_.function_, "replacement memory input differs");
  }
  if (outputs != definitions.size () || count != inputs.size ())
    return invalid (form_.function_, "replacement site count differs");
  for (auto use : detail::ReferenceAccess::pointers (form_.inputs (instruction)))
    if (std::holds_alternative<SSAHardwareUse> (use->site ()))
      return invalid (form_.function_, "replacement would remove a hardware return boundary");
  checked = rewriter_.replace (instruction, opcode, operands);
  if (!(checked).has_value ()) return checked;
  touched ();
  for (auto use : detail::ReferenceAccess::pointers (form_.inputs (instruction))) {
    unlink (*use);
    use->alive_ = false;
  }
  form_.inputs_.erase (&instruction);
  count = 0;
  for (auto &site : input_positions (form_.function_, instruction)) {
    auto use = input (instruction, site.operand, site.part, *inputs[count++]);
    if (!(use).has_value ()) return use.error ();
  }
  return {};
}
Result<void> SSAUpdater::replace_memory (Instruction &instruction, std::size_t p, Memory memory,
                                         SSAValue *base, SSAValue *index) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&instruction.function () != &form_.function_ || p >= instruction.operands ().size ()
      || !detail::operand_access (instruction.operands ()[p]).get<Memory> ())
    return invalid (form_.function_, "replacement requires a memory operand of the same function");
  auto matches = [&] (const Register *reg, SSAValue *value) {
    return !reg ? !value
                : value && &value->form () == &form_ && value->alive () && &value->reg () == reg;
  };
  if (!matches (detail::NodeAccess::base (memory), base)
      || !matches (detail::NodeAccess::index (memory), index))
    return invalid (form_.function_, "replacement memory definitions differ");
  std::vector<Operand> operands (instruction.operands ().begin (), instruction.operands ().end ());
  operands[p] = Operand (std::move (memory));
  checked = rewriter_.replace_owned (instruction, instruction.opcode (), std::move (operands));
  if (!(checked).has_value ()) return checked;
  touched ();
  auto &inputs = form_.inputs_[&instruction];
  std::erase_if (inputs, [&] (SSAUse *use) {
    auto site = std::get_if<SSAInstructionUse> (&use->site ());
    if (!site || site->operand != p) return false;
    unlink (*use);
    use->alive_ = false;
    return true;
  });
  if (base) {
    auto use = input (instruction, p, SSAOperandPart::MemoryBase, *base);
    if (!(use).has_value ()) return use.error ();
  }
  if (index) {
    auto use = input (instruction, p, SSAOperandPart::MemoryIndex, *index);
    if (!(use).has_value ()) return use.error ();
  }
  return {};
}
Result<void> SSAUpdater::erase_phi_input (SSAPhi &phi, std::size_t p) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&phi.form () != &form_ || !phi.alive () || p >= phi.inputs_.size ())
    return invalid (form_.function_, "invalid removed PHI input");
  auto use = phi.inputs_[p];
  unlink (*use);
  use->alive_ = false;
  phi.inputs_.erase (phi.inputs_.begin () + p);
  phi.predecessors_.erase (phi.predecessors_.begin () + p);
  for (; p < phi.inputs_.size (); ++p) std::get<SSAPhiUse> (phi.inputs_[p]->site_).input = p;
  form_.conventional_ = false;
  touched ();
  return {};
}
Result<SSAValue &> SSAUpdater::coalesce_copy (Instruction &instruction) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&instruction.function () != &form_.function_
      || (instruction.opcode () != Opcode::Mov && instruction.opcode () != Opcode::FMov
          && instruction.opcode () != Opcode::DMov && instruction.opcode () != Opcode::LDMov
          && !(sizeof (long double) == sizeof (double)
               && (instruction.opcode () == Opcode::D2LD || instruction.opcode () == Opcode::LD2D)))
      || !detail::operand_access (instruction.operands ()[0]).get<const Register *> ()
      || !detail::operand_access (instruction.operands ()[1]).get<const Register *> ())
    return invalid (form_.function_, "coalescing requires a register copy");
  auto definitions = detail::ReferenceAccess::pointers (form_.definitions (instruction));
  auto inputs = detail::ReferenceAccess::pointers (form_.inputs (instruction));
  if (definitions.size () != 1 || inputs.size () != 1)
    return invalid (form_.function_, "invalid copy sites");
  auto destination = definitions[0];
  auto source = const_cast<SSAValue *> (&inputs[0]->value ());
  if (source == destination || (!source->instruction ().get () && !source->phi ().get ()))
    return invalid (form_.function_, "copy source has no editable definition");
  auto reg = &destination->reg ();
  if (scalar (source->reg ().type ()) != scalar (reg->type ()))
    return invalid (form_.function_, "copy register representations differ");
  if (source->phi ().get () && form_.conventional_)
    for (auto use : detail::ReferenceAccess::pointers (source->phi ().get ()->inputs ()))
      if (&use->value ().reg () != reg)
        return invalid (form_.function_, "coalescing breaks conventional PHI");
  // Validate the combined rewrite before moving a definition to a binding.
  // Hardware return effects must still refer to that same physical register.
  std::vector<SSAUse *> uses (source->uses_.begin (), source->uses_.end ());
  uses.insert (uses.end (), destination->uses_.begin (), destination->uses_.end ());
  checked = rewrite (uses, *reg, false, source);
  if (!(checked).has_value ()) return checked.error ();
  source->reg_ = reg;
  form_.renamed_ = true;
  for (auto use : destination->uses_) use->value_ = source;
  source->uses_.splice (source->uses_.end (), destination->uses_);
  touched ();
  checked = erase_instruction (instruction);
  if (!(checked).has_value ()) return checked.error ();
  destination->replacement_ = source;
  return *(source);
}
Result<void> SSAUpdater::erase_phi (SSAPhi &phi, SSAValue &replacement) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&phi.form () != &form_ || !phi.alive () || &replacement.form () != &form_
      || !replacement.alive ())
    return invalid (form_.function_, "redundant PHI replacement is foreign or dead");
  if (phi.output_ == &replacement)
    return invalid (form_.function_, "redundant PHI replacement is its own output");
  if (scalar (phi.output ().reg ().type ()) != scalar (replacement.reg ().type ()))
    return invalid (form_.function_, "redundant PHI replacement changes scalar type");
  checked = replace_all_uses (*phi.output_, replacement);
  if (!(checked).has_value ()) return checked;
  for (auto use : phi.inputs_) {
    unlink (*use);
    use->alive_ = false;
  }
  phi.output_->alive_ = false;
  phi.output_->replacement_ = &replacement;
  auto &ordered = form_.block_phis_.at (phi.block_);
  std::erase (ordered, &phi);
  touched ();
  return {};
}
Result<void> SSAUpdater::erase_instruction (Instruction &instruction) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&instruction.function () != &form_.function_)
    return invalid (form_.function_, "erased instruction crosses ownership");
  for (auto value : detail::ReferenceAccess::pointers (form_.definitions (instruction)))
    if (!detail::ReferenceAccess::pointers (value->uses ()).empty ())
      return invalid (form_.function_, "erased definition still has uses");
  for (auto use : detail::ReferenceAccess::pointers (form_.inputs (instruction))) {
    unlink (*use);
    use->alive_ = false;
  }
  for (auto value : detail::ReferenceAccess::pointers (form_.definitions (instruction)))
    value->alive_ = false;
  auto next = rewriter_.next_instruction (instruction);
  for (auto phi : detail::ReferenceAccess::pointers (form_.phis (instruction.block ())))
    if (phi->before_ == &instruction) phi->before_ = next;
  form_.inputs_.erase (&instruction);
  form_.definitions_.erase (&instruction);
  checked = rewriter_.erase (instruction);
  if (!(checked).has_value ()) return checked;
  touched ();
  return {};
}
Result<void> SSAUpdater::erase_phi (SSAPhi &phi) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&phi.form () != &form_ || !phi.alive ()
      || !detail::ReferenceAccess::pointers (phi.output ().uses ()).empty ())
    return invalid (form_.function_, "erased PHI is foreign, dead or still used");
  for (auto use : phi.inputs_) {
    unlink (*use);
    use->alive_ = false;
  }
  phi.output_->alive_ = false;
  auto &ordered = form_.block_phis_.at (phi.block_);
  std::erase (ordered, &phi);
  touched ();
  return {};
}
Result<void> SSAUpdater::move_before (Instruction &instruction, BasicBlock &block,
                                      const Instruction *before) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&instruction.function () != &form_.function_ || &block.function () != &form_.function_
      || instruction_descriptor (instruction.opcode ()).get ()->flow != ControlFlow::Next)
    return invalid (form_.function_,
                    "SSA motion requires an ordinary instruction of the same function");
  if (before && &before->block () != &block)
    return invalid (form_.function_, "SSA motion anchor belongs to another block");
  if (before == &instruction) return {};
  auto &source = instruction.block ();
  auto next = rewriter_.next_instruction (instruction);
  checked = rewriter_.move_before (instruction, block, before);
  if (!(checked).has_value ()) return checked;
  for (auto phi : detail::ReferenceAccess::pointers (form_.phis (source)))
    if (phi->before_ == &instruction) phi->before_ = next;
  for (auto phi : detail::ReferenceAccess::pointers (form_.phis (block)))
    if (phi->before_ == before) phi->before_ = &instruction;
  // An insertion anchor denotes the position after private PHIs there.
  // Existing PHIs stay in the source/destination; only the instruction moves.
  touched ();
  return {};
}
Result<BasicBlock &> SSAUpdater::split_fallthrough (BasicBlock &source, BasicBlock &destination) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (&source.function () != &form_.function_ || &destination.function () != &form_.function_
      || rewriter_.next_block (source) != &destination
      || (source.terminator ().get ()
          && instruction_descriptor (source.terminator ().get ()->opcode ()).get ()->flow
               != ControlFlow::Branch))
    return invalid (form_.function_,
                    "SSA edge split requires a layout fallthrough: source "
                      + std::string (source.name ())
                      + (source.terminator ().get ()
                           ? " / "
                               + std::string (
                                 instruction_descriptor (source.terminator ().get ()->opcode ())
                                   .get ()
                                   ->name)
                           : "")
                      + ", destination " + std::string (destination.name ()));
  auto block = rewriter_.insert_empty_block_before (destination);
  if (!(block).has_value ()) return block.error ();
  for (auto phi : detail::ReferenceAccess::pointers (form_.phis (destination)))
    for (auto &predecessor : phi->predecessors_)
      if (predecessor == &source) {
        // A conditional may also target its fallthrough block. The fallthrough
        // edge is the first predecessor from that source in MIR's edge order.
        predecessor = &(block).value ();
        break;
      }
  touched ();
  return (block).value ();
}
Result<Instruction &> SSAUpdater::insert (BasicBlock &block, const Instruction *before,
                                          Opcode opcode, std::span<const Operand> operands,
                                          std::span<SSAValue *const> inputs,
                                          const SSAPhi *after_phi) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked.error ();
  if (instruction_descriptor (opcode).get () == nullptr
      || instruction_descriptor (opcode).get ()->flow != ControlFlow::Next)
    return invalid (form_.function_, "SSA insertion requires an ordinary instruction");
  if (after_phi
      && (&after_phi->form () != &form_ || !after_phi->alive () || &after_phi->block () != &block
          || after_phi->before ().get () != before))
    return invalid (form_.function_, "invalid insertion after PHI");
  checked = validate_instruction (form_.function_, opcode, operands);
  if (!(checked).has_value ()) return checked.error ();
  // Validate input definitions before publishing the canonical instruction.
  std::size_t count = 0;
  for (std::size_t p = 0; p < operands.size (); ++p) {
    const auto descriptor = instruction_descriptor (opcode).get ();
    bool output = descriptor->count == OperandCount::Fixed && p < descriptor->roles.size ()
                  && descriptor->roles[p] >= 'A' && descriptor->roles[p] <= 'Z'
                  && opcode != Opcode::LAddr;
    if (descriptor->count == OperandCount::Call)
      output = p >= 2
               && p - 2 < detail::NodeAccess::symbol (
                            *(detail::operand_access (operands[0]).get<SymbolReference> ()))
                            ->signature ()
                            .get ()
                            ->results.size ();
    std::vector<const Register *> regs;
    if (auto r = detail::operand_access (operands[p]).get<const Register *> (); r && !output)
      regs.push_back (*r);
    else if (auto memory = detail::operand_access (operands[p]).get<Memory> ()) {
      if (detail::NodeAccess::base (*(memory)))
        regs.push_back (detail::NodeAccess::base (*(memory)));
      if (detail::NodeAccess::index (*(memory)))
        regs.push_back (detail::NodeAccess::index (*(memory)));
    }
    for (auto reg : regs) {
      if (count >= inputs.size () || !inputs[count] || &inputs[count]->form () != &form_
          || !inputs[count]->alive () || &inputs[count]->reg () != reg)
        return invalid (form_.function_, "inserted input differs from definition");
      ++count;
    }
  }
  if (count != inputs.size ()) return invalid (form_.function_, "inserted input count differs");
  auto inserted = rewriter_.insert (block, before, opcode, operands);
  if (!(inserted).has_value ()) return inserted.error ();
  touched ();
  for (auto phi : detail::ReferenceAccess::pointers (form_.phis (block)))
    if (phi->before_ == before) {
      phi->before_ = &(inserted).value ();
      if (phi == after_phi) break;
    }
  auto &instruction = (inserted).value ();
  for (std::size_t p = 0; p < operands.size (); ++p)
    if (detail::operand_access (operands[p]).get<const Register *> ()
        && ssa_output_operand (form_.function_, instruction, p)) {
      auto value = definition (instruction, p);
      if (!(value).has_value ()) return value.error ();
    }
  count = 0;
  for (auto &site : input_positions (form_.function_, instruction)) {
    auto use = input (instruction, site.operand, site.part, *inputs[count++]);
    if (!(use).has_value ()) return use.error ();
  }
  return (inserted).value ();
}
Result<void> SSAUpdater::mark_renamed () {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  form_.renamed_ = true;
  touched ();
  return {};
}
Result<void> SSAUpdater::mark_conventional () {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  for (auto &phi : form_.phis_)
    if (phi.alive ())
      for (auto use : detail::ReferenceAccess::pointers (phi.inputs ()))
        if (&use->value ().reg () != &phi.output ().reg ())
          return invalid (form_.function_, "PHI has not been made conventional");
  form_.conventional_ = true;
  touched ();
  return form_.verify ();
}
Result<void> SSAUpdater::set_phi_position (SSAPhi &phi, const Instruction *before) {
  auto checked = writable ();
  if (!(checked).has_value ()) return checked;
  if (&phi.form () != &form_ || !phi.alive () || (before && &before->block () != &phi.block ()))
    return invalid (form_.function_, "invalid PHI insertion position");
  phi.before_ = before;
  touched ();
  return {};
}
Result<void> SSAUpdater::complete () {
  auto checked = form_.verify ();
  if (!(checked).has_value ()) return checked;
  if (form_.renamed_ && !form_.conventional_)
    return invalid (form_.function_, "renamed SSA requires conventional PHIs before exit");
  auto &function = form_.function_;
  if (function.ssa_form ().get () != &form_)
    return invalid (function, "SSA phase is not installed");
  function.ssa_.reset ();
  function.changed (false);
  return function.verify ();
}
std::string dump_ssa (const Function &function) {
  auto form = function.ssa_form ().get ();
  if (!form) return {};
  auto checked = form->verify ();
  if (!(checked).has_value ()) return "SSA error: " + checked.error ().message + '\n';
  std::unordered_map<const SSAValue *, std::size_t> ids;
  std::unordered_map<const Register *, std::size_t> regs;
  std::unordered_map<const Instruction *, std::size_t> instructions;
  std::unordered_map<const BasicBlock *, std::size_t> blocks;
  for (auto &reg : function.registers ()) regs.emplace (&reg, regs.size ());
  for (auto &block : function.blocks ()) {
    blocks.emplace (&block, blocks.size ());
    for (auto &instruction : block.instructions ())
      instructions.emplace (&instruction, instructions.size ());
  }
  for (auto &value : form->values ())
    if (value.alive ()) ids.emplace (&value, ids.size ());
  std::ostringstream out;
  out << "SSA renamed=" << form->renamed () << " conventional=" << form->conventional () << " {\n";
  for (auto &value : form->values ())
    if (value.alive ()) {
      out << "  v" << ids.at (&value) << " = ";
      if (value.instruction ().get ())
        out << "insn " << instructions.at (value.instruction ().get ()) << ':' << value.operand ();
      else if (value.phi ().get ()) {
        auto phi = value.phi ().get ();
        out << "phi bb" << blocks.at (&phi->block ()) << " before=";
        if (phi->before ().get ())
          out << instructions.at (phi->before ().get ());
        else
          out << "end";
        out << " predecessors=";
        for (auto predecessor : detail::ReferenceAccess::pointers (phi->predecessors ())) {
          if (predecessor)
            out << "bb" << blocks.at (predecessor);
          else
            out << "entry";
          out << ',';
        }
      } else
        out << (value.kind () == SSAValue::Kind::Entry ? "entry" : "undefined");
      out << " %" << regs.at (&value.reg ()) << " origin=%" << regs.at (&value.origin ()) << '\n';
    }
  for (auto &use : form->uses ())
    if (use.alive ()) {
      out << "  ";
      std::visit (
        [&] (const auto &site) {
          using T = std::decay_t<decltype (site)>;
          if constexpr (std::is_same_v<T, SSAInstructionUse>)
            out << "insn " << instructions.at (detail::NodeAccess::instruction (site)) << ':'
                << site.operand << '/' << static_cast<unsigned> (site.part);
          else if constexpr (std::is_same_v<T, SSAPhiUse>)
            out << "phi v" << ids.at (&detail::NodeAccess::phi (site)->output ()) << ':'
                << site.input;
          else
            out << "hardware-return " << instructions.at (detail::NodeAccess::ret (site)) << " %"
                << regs.at (detail::NodeAccess::hardware (site));
        },
        use.site ());
      out << " <- v" << ids.at (&use.value ()) << '\n';
    }
  out << "}\n";
  return out.str ();
}
}  // namespace mirnext::ir
