/* Licensed under the MIT License. Derived from MIR, Copyright 2018-2024 Vladimir Makarov. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <algorithm>
#include <unordered_set>

namespace mirnext::transforms {
namespace {
using namespace ir;
bool address (Opcode code) {
  return code == Opcode::Addr || code == Opcode::Addr8 || code == Opcode::Addr16
         || code == Opcode::Addr32;
}
bool move (Opcode code) {
  return code == Opcode::Mov || code == Opcode::FMov || code == Opcode::DMov
         || code == Opcode::LDMov || code == Opcode::VMov;
}
bool copy (const Instruction &i) {
  return move (i.opcode ())
         && ir::detail::operand_access (i.operands ()[0]).get<const Register *> ()
         && ir::detail::operand_access (i.operands ()[1]).get<const Register *> ();
}
Type scalar (Type type) { return type.is_float () || type.is_vector () ? type : Type::i64 (); }
Opcode move_code (Type type) {
  return type.kind () == Type::Kind::F32          ? Opcode::FMov
         : type.kind () == Type::Kind::F64        ? Opcode::DMov
         : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
         : type.is_vector ()                      ? Opcode::VMov
                                                  : Opcode::Mov;
}
std::optional<Opcode> extension (Type type) {
  switch (type.kind ()) {
  case Type::Kind::I8: return Opcode::Ext8;
  case Type::Kind::U8: return Opcode::UExt8;
  case Type::Kind::I16: return Opcode::Ext16;
  case Type::Kind::U16: return Opcode::UExt16;
  case Type::Kind::I32: return Opcode::Ext32;
  case Type::Kind::U32: return Opcode::UExt32;
  default: return {};
  }
}
Error invalid (const Function &function, std::string reason) {
  return {ErrorCode::InvalidArgument, std::string (function.symbol ().name ())
                                        + ": transform-addresses: " + std::move (reason)};
}
struct Family {
  Instruction *address;
  const Register *source;
  std::vector<Instruction *> memory_users;
  bool escapes = false;
  bool stored = false;
  unsigned store_width = 64;
};
unsigned integer_width (Type type) {
  switch (type.kind ()) {
  case Type::Kind::I8:
  case Type::Kind::U8: return 8;
  case Type::Kind::I16:
  case Type::Kind::U16: return 16;
  case Type::Kind::I32:
  case Type::Kind::U32: return 32;
  default: return 64;
  }
}
Family collect (const SSAForm &form, Instruction &addr) {
  Family family{&addr, *ir::detail::operand_access (addr.operands ()[1]).get<const Register *> ()};
  family.escapes
    = family.source->is_memory_backed ()
      || family.source
           == *ir::detail::operand_access (addr.operands ()[0]).get<const Register *> ();
  std::vector<const SSAValue *> pending{
    ir::detail::ReferenceAccess::pointers (form.definitions (addr))[0]};
  std::unordered_set<const Instruction *> seen;
  while (!pending.empty ()) {
    auto value = pending.back ();
    pending.pop_back ();
    // MIR prepends uses and pops copy work in LIFO order. Borrow inverse edges
    // directly instead of scanning the entire SSA body for every definition.
    for (auto it = ir::detail::ReferenceAccess::pointers (value->uses ()).begin ();
         it != ir::detail::ReferenceAccess::pointers (value->uses ()).end (); ++it) {
      auto site = std::get_if<SSAInstructionUse> (&(*it)->site ());
      if (!site) {
        family.escapes = true;
        continue;
      }
      auto &user = *ir::detail::NodeAccess::instruction (*(site));
      if (auto memory
          = ir::detail::operand_access (user.operands ()[site->operand]).get<Memory> ()) {
        // Simplify supplies scalar base-only moves. Other address consumers
        // require real storage; a scalar substitution cannot represent them.
        if (!move (user.opcode ()) || site->operand > 1 || site->part != SSAOperandPart::MemoryBase
            || ir::detail::NodeAccess::index (*(memory)) || memory->displacement
            || scalar (memory->type) != scalar (family.source->type ())) {
          family.escapes = true;
          continue;
        }
        family.memory_users.push_back (const_cast<Instruction *> (&user));
        if (site->operand == 0)
          family.store_width = std::min (family.store_width, integer_width (memory->type));
      } else if (!copy (user))
        family.escapes = true;
      else if (seen.insert (&user).second)
        pending.push_back (ir::detail::ReferenceAccess::pointers (form.definitions (user))[0]);
    }
  }
  return family;
}
}  // namespace
Result<PassResult> AddressTransformPass::run (ir::MIRBuilder &edit,
                                              analysis::FunctionAnalysisManager &analyses) {
  if (auto status = edit.status (); !status.has_value ()) return status.error ();
  if (!edit.function ())
    return Error{ErrorCode::InvalidArgument, "pass requires a selected function"};
  auto &function = *edit.function ();

  if (&analyses.function () != &function) return invalid (function, "analysis owner mismatch");
  auto checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto form = function.ssa_form ().get ();
  if (!form || form->renamed ()) return invalid (function, "requires unrenamed SSA");
  std::vector<Family> families;
  std::unordered_map<const Instruction *, std::size_t> indices;
  std::unordered_set<const Register *> escaping;
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ())
      if (address (instruction.opcode ())) {
        if (!ir::detail::operand_access (instruction.operands ()[0]).get<const Register *> ()
            || !ir::detail::operand_access (instruction.operands ()[1]).get<const Register *> ()
            || ir::detail::ReferenceAccess::pointers (form->definitions (instruction)).size () != 1)
          return invalid (function, "simplify must materialize address operands before SSA");
        indices.emplace (&instruction, families.size ());
        families.push_back (collect (*form, instruction));
        if (families.back ().escapes) escaping.insert (families.back ().source);
      }
  // Replacing a partial store with EXT overwrites the untouched high bytes.
  // Account for every address of the same register: a narrow view may alias a
  // natural-width view whose later load observes those bytes.
  if (std::any_of (families.begin (), families.end (),
                   [] (auto &family) { return family.store_width < 64; })) {
    std::unordered_map<const Register *, unsigned> widths;
    for (auto &family : families) {
      auto code = family.address->opcode ();
      auto width = code == Opcode::Addr8    ? 8u
                   : code == Opcode::Addr16 ? 16u
                   : code == Opcode::Addr32 ? 32u
                                            : 64u;
      auto &maximum = widths[family.source];
      maximum = std::max (maximum, width);
    }
    // Ordinary scalar reads observe the full register representation too,
    // including a return after a write through ADDR8/16/32.
    for (auto &block : function.blocks ())
      for (auto &i : block.instructions ())
        if (!address (i.opcode ()))
          for (std::size_t p = 0; p < i.operands ().size (); ++p)
            if (auto reg = ir::detail::operand_access (i.operands ()[p]).get<const Register *> ();
                reg && !output_operand (function, i, p))
              if (auto found = widths.find (*reg); found != widths.end ()) found->second = 64;
    for (auto &family : families)
      if (family.store_width < widths.at (family.source)) {
        family.escapes = true;
        escaping.insert (family.source);
      }
  }
  // Scalar memory users can observe a pointer assignment too. Retain its
  // pointee address when those loads later become ordinary register reads.
  if (families.size () > 1) {
    std::unordered_set<const Register *> observed;
    for (auto &family : families)
      if (family.escapes || !family.memory_users.empty ()) observed.insert (family.source);
    for (auto &family : families)
      if (observed.contains (
            *ir::detail::operand_access (family.address->operands ()[0]).get<const Register *> ()))
        escaping.insert (family.source);
  }
  // An address written into an escaping pointer slot escapes too, even when
  // the SSA definition has no ordinary uses. Propagate once per address edge.
  if (!escaping.empty ()) {
    std::unordered_multimap<const Register *, const Register *> stored_addresses;
    stored_addresses.reserve (families.size ());
    for (auto &family : families)
      stored_addresses.emplace (*ir::detail::operand_access (family.address->operands ()[0])
                                   .get<const Register *> (),
                                family.source);
    std::vector<const Register *> pending (escaping.begin (), escaping.end ());
    while (!pending.empty ()) {
      auto reg = pending.back ();
      pending.pop_back ();
      auto [begin, end] = stored_addresses.equal_range (reg);
      for (auto it = begin; it != end; ++it)
        if (escaping.insert (it->second).second) pending.push_back (it->second);
    }
    std::unordered_set<const Register *> addressed;
    for (auto &family : families) {
      auto destination
        = *ir::detail::operand_access (family.address->operands ()[0]).get<const Register *> ();
      family.stored = destination == family.source
                      || (escaping.contains (destination) && addressed.contains (destination));
      if (escaping.contains (family.source)) addressed.insert (family.source);
    }
  }
  for (auto &block : function.blocks ())
    for (auto &instruction : block.instructions ())
      for (auto &operand : instruction.operands ())
        if (auto memory = ir::detail::operand_access (operand).get<Memory> ();
            memory && ir::detail::NodeAccess::index (*(memory))
            && escaping.contains (ir::detail::NodeAccess::index (*(memory))))
          return invalid (function,
                          "simplify must materialize an escaping memory index before SSA");
  // Decisions borrow the intact def-use graph. Exiting unrenamed SSA keeps all
  // canonical registers and instructions; no shadow instruction body is made.
  std::size_t changes = 0;
  for (auto &phi : form->phis ()) changes += phi.alive ();
  auto ssa = SSAUpdater::get (function);
  if (!(ssa).has_value ()) return ssa.error ();
  checked = ssa.value ().complete ();
  if (!(checked).has_value ()) return checked.error ();
  InstructionRewriter rewriter (function);
  for (auto &block : function.blocks ())
    for (auto instruction = block.size () ? &*block.instructions ().begin () : nullptr;
         instruction;) {
      auto next = rewriter.next_instruction (*instruction);
      if (!address (instruction->opcode ())) {
        const Register *previous = nullptr, *addr_reg = nullptr;
        std::vector<Operand> operands;
        bool edited = false;
        for (std::size_t p = 0; p < instruction->operands ().size (); ++p) {
          auto &operand = instruction->operands ()[p];
          auto reg = ir::detail::operand_access (operand).get<const Register *> ();
          auto memory = ir::detail::operand_access (operand).get<Memory> ();
          auto variable = reg ? *reg : memory ? ir::detail::NodeAccess::base (*(memory)) : nullptr;
          if (!variable || !escaping.contains (variable)) continue;
          if (!edited)
            operands.assign (instruction->operands ().begin (), instruction->operands ().end ());
          auto type = scalar (variable->type ());
          bool output = reg && output_operand (function, *instruction, p);
          if (previous != variable) {
            auto created = ir::detail::access (function).create_register (Type::i64 ());
            if (!(created).has_value ()) return created.error ();
            addr_reg = &(created).value ();
            auto added = rewriter.insert_before (*instruction, Opcode::Addr,
                                                 {Operand (*addr_reg), Operand (*variable)});
            if (!(added).has_value ()) return added.error ();
            previous = variable;
            ++changes;
          }
          auto temp = ir::detail::access (function).create_register (type);
          if (!(temp).has_value ()) return temp.error ();
          Operand storage (ir::detail::NodeAccess::make_Memory (type, addr_reg));
          auto added = output ? rewriter.insert_after (*instruction, move_code (type),
                                                       {storage, Operand ((temp).value ())})
                              : rewriter.insert_before (*instruction, move_code (type),
                                                        {Operand ((temp).value ()), storage});
          if (!(added).has_value ()) return added.error ();
          ++changes;
          edited = true;
          if (reg)
            operands[p] = Operand ((temp).value ());
          else {
            auto updated = *memory;
            ir::detail::NodeAccess::base (updated) = &(temp).value ();
            operands[p] = Operand (std::move (updated));
          }
        }
        if (edited) {
          checked = rewriter.replace (*instruction, instruction->opcode (), operands);
          if (!(checked).has_value ()) return checked.error ();
          ++changes;
        }
      } else {
        auto &family = families[indices.at (instruction)];
        if (!escaping.contains (family.source)) {
          for (auto it = family.memory_users.rbegin (); it != family.memory_users.rend (); ++it) {
            auto user = *it;
            std::vector<Operand> operands (user->operands ().begin (), user->operands ().end ());
            auto p = ir::detail::operand_access (operands[0]).get<Memory> () ? 0u : 1u;
            auto type = ir::detail::operand_access (operands[p]).get<Memory> ()->type;
            auto code = extension (type).value_or (user->opcode ());
            operands[p] = Operand (*family.source);
            checked = rewriter.replace (*user, code, operands);
            if (!(checked).has_value ()) return checked.error ();
            ++changes;
          }
          checked = rewriter.erase (*instruction);
          if (!(checked).has_value ()) return checked.error ();
          ++changes;
        } else if (family.stored) {
          // Earlier and self addresses observe this pointer slot assignment.
          // Make the store explicit before SSA renaming changes its identity.
          auto destination
            = *ir::detail::operand_access (instruction->operands ()[0]).get<const Register *> ();
          auto pointer = ir::detail::access (function).create_register (Type::i64 ()),
               value = ir::detail::access (function).create_register (Type::i64 ());
          if (!(pointer).has_value ()) return pointer.error ();
          if (!(value).has_value ()) return value.error ();
          auto added
            = rewriter.insert_before (*instruction, Opcode::Addr,
                                      {Operand ((pointer).value ()), Operand (*destination)});
          if (!(added).has_value ()) return added.error ();
          added
            = rewriter.insert_after (*instruction, Opcode::Mov,
                                     {Operand (
                                        ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                             &(pointer).value ())),
                                      Operand ((value).value ())});
          if (!(added).has_value ()) return added.error ();
          const Operand operands[]{Operand ((value).value ()), Operand (*family.source)};
          checked = rewriter.replace (*instruction, instruction->opcode (), operands);
          if (!(checked).has_value ()) return checked.error ();
          changes += 3;
        }
      }
      instruction = next;
    }
  std::vector<const Register *> storage (escaping.begin (), escaping.end ());
  for (auto reg : storage) changes += !reg->is_memory_backed ();
  checked = rewriter.materialize_register_storage (storage);
  if (!(checked).has_value ()) return checked.error ();
  checked = function.verify ();
  if (!(checked).has_value ()) return checked.error ();
  auto preserved = analysis::PreservedAnalyses::none ();
  preserved.preserve<analysis::ControlFlowAnalysis> ();
  preserved.preserve<analysis::FunctionGraphAnalysis> ();
  preserved.preserve<analysis::DominatorAnalysis> ();
  preserved.preserve<analysis::LoopAnalysis> ();
  return PassResult{changes, preserved};
}
}  // namespace mirnext::transforms
