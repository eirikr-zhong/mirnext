/* Licensed under the MIT License. Scope state is lowered on control-flow edges. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/IR.hpp"
#include <algorithm>
#include <unordered_set>

namespace mirnext::ir {
namespace {
struct Transition {
  std::vector<const StackScope *> leave, enter;
  bool empty () const { return leave.empty () && enter.empty (); }
  friend bool operator== (const Transition &, const Transition &) = default;
};
Transition transition (const StackScope *from, const StackScope *to) {
  Transition result;
  while (from != to) {
    if (from && (!to || from->depth () >= to->depth ())) {
      result.leave.push_back (from);
      from = from->parent ().get ();
    } else {
      result.enter.push_back (to);
      to = to->parent ().get ();
    }
  }
  std::reverse (result.enter.begin (), result.enter.end ());
  return result;
}
Opcode move (Type type) {
  return type.kind () == Type::Kind::F32          ? Opcode::FMov
         : type.kind () == Type::Kind::F64        ? Opcode::DMov
         : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
         : type.is_vector ()                      ? Opcode::VMov
                                                  : Opcode::Mov;
}
class Materializer {
 public:
  explicit Materializer (Function &function) : function_ (function), rewriter_ (function) {}
  Result<MaterializationStats> run () {
    std::unordered_set<const StackScope *> used;
    for (auto &block : function_.blocks ()) {
      blocks_.push_back (&block);
      for (auto scope = block.stack_scope ().get (); scope && used.insert (scope).second;
           scope = scope->parent ().get ()) {
      }
      if (!detail::ReferenceAccess::pointers (block.labels ()).empty ())
        indirect_targets_.push_back (&block);
    }
    for (auto &scope : function_.stack_scopes ())
      if (used.contains (&scope)) {
        auto token = detail::access (function_).create_register (Type::pointer ());
        if (!(token).has_value ()) return token.error ();
        tokens_.emplace (&scope, &(token).value ());
      }
    // Keep the original first block address distinct from the one-time entry save.
    if (blocks_.front ()->stack_scope ().get ()) {
      auto entry = detail::access (function_).create_block_before (*blocks_.front ());
      if (!(entry).has_value ()) return entry.error ();
      auto saved
        = actions ((entry).value (), transition (nullptr, blocks_.front ()->stack_scope ().get ()));
      if (!(saved).has_value ()) return saved.error ();
      auto label = label_of (*blocks_.front ());
      if (!(label).has_value ()) return label.error ();
      auto jumped = InstructionBuilder ((entry).value ()).jump ((label).value ());
      if (!(jumped).has_value ()) return jumped.error ();
    }
    for (std::size_t n = 0; n < blocks_.size (); ++n) {
      auto &block = *blocks_[n];
      auto source = block.stack_scope ().get ();
      auto terminator = const_cast<Instruction *> (block.terminator ().get ());
      if (terminator) {
        auto opcode = terminator->opcode ();
        if (opcode == Opcode::Ret || opcode == Opcode::JRet || opcode == Opcode::JCall) {
          if (source) {
            auto frozen = freeze_exit (*terminator);
            if (!(frozen).has_value ()) return frozen.error ();
            auto restored = actions (block, transition (source, nullptr), terminator);
            if (!(restored).has_value ()) return restored.error ();
          }
          continue;
        }
        if (opcode == Opcode::JmpIndirect) {
          auto lowered = indirect (block, *terminator);
          if (!(lowered).has_value ()) return lowered.error ();
          continue;
        }
        if (opcode == Opcode::Switch || opcode == Opcode::Jmp
            || instruction_descriptor (opcode).get ()->flow == ControlFlow::Branch) {
          for (std::size_t index = 0; index < terminator->operands ().size (); ++index) {
            auto label
              = detail::operand_access (terminator->operands ()[index]).get<const Label *> ();
            if (!label) continue;
            if (source == (*label)->block ().stack_scope ().get ()) continue;
            auto target = target_of (source, const_cast<BasicBlock &> ((*label)->block ()));
            if (!(target).has_value ()) return target.error ();
            if (&(target).value () != *label) {
              auto replaced
                = rewriter_.replace_operand (*terminator, index, Operand ((target).value ()));
              if (!(replaced).has_value ()) return replaced.error ();
            }
          }
        }
        if (instruction_descriptor (opcode).get ()->flow != ControlFlow::Branch) continue;
      }
      if (n + 1 == blocks_.size ()) continue;
      auto &next = *blocks_[n + 1];
      auto change = transition (source, next.stack_scope ().get ());
      if (change.empty ()) continue;
      if (!terminator) {
        auto lowered = actions (block, change);
        if (!(lowered).has_value ()) return lowered.error ();
      } else {
        auto cleanup = detail::access (function_).create_block_after (block);
        if (!(cleanup).has_value ()) return cleanup.error ();
        ++stats_.cleanup_blocks;
        auto lowered = actions ((cleanup).value (), change);
        if (!(lowered).has_value ()) return lowered.error ();
        auto label = label_of (next);
        if (!(label).has_value ()) return label.error ();
        auto jumped = InstructionBuilder ((cleanup).value ()).jump ((label).value ());
        if (!(jumped).has_value ()) return jumped.error ();
      }
    }
    for (auto &block : function_.blocks ()) {
      auto cleared = rewriter_.set_stack_scope (block, nullptr);
      if (!(cleared).has_value ()) return cleared.error ();
    }
    return stats_;
  }

 private:
  Function &function_;
  InstructionRewriter rewriter_;
  MaterializationStats stats_;
  std::vector<BasicBlock *> blocks_, indirect_targets_;
  std::unordered_map<const StackScope *, const Register *> tokens_;
  std::unordered_map<const StackScope *, std::unordered_map<const BasicBlock *, const Label *>>
    cleanups_;
  Result<const Label &> label_of (BasicBlock &block) {
    if (!detail::ReferenceAccess::pointers (block.labels ()).empty ())
      return *(detail::ReferenceAccess::pointers (block.labels ()).front ());
    auto label = detail::access (function_).create_label (block);
    if (!(label).has_value ()) return label.error ();
    return *(static_cast<const Label *> (&(label).value ()));
  }
  Result<void> actions (BasicBlock &block, const Transition &change,
                        Instruction *before = nullptr) {
    for (bool entering : {false, true})
      for (auto scope : entering ? change.enter : change.leave) {
        auto opcode = entering ? Opcode::BStart : Opcode::BEnd;
        auto instruction
          = before ? rewriter_.insert_before (*before, opcode, {Operand (*tokens_.at (scope))})
                   : InstructionBuilder (block).emit (opcode, {Operand (*tokens_.at (scope))});
        if (!(instruction).has_value ()) return instruction.error ();
        if (entering)
          ++stats_.saves;
        else
          ++stats_.restores;
      }
    return {};
  }
  Result<const Label &> target_of (const StackScope *source, BasicBlock &destination) {
    auto change = transition (source, destination.stack_scope ().get ());
    if (change.empty ()) return label_of (destination);
    auto &cache = cleanups_[source];
    auto found = cache.find (&destination);
    if (found != cache.end ()) return *(found->second);
    auto block = detail::access (function_).create_block ();
    if (!(block).has_value ()) return block.error ();
    ++stats_.cleanup_blocks;
    auto cleanup = label_of ((block).value ());
    if (!(cleanup).has_value ()) return cleanup.error ();
    cache.emplace (&destination, &(cleanup).value ());
    auto lowered = actions ((block).value (), change);
    if (!(lowered).has_value ()) return lowered.error ();
    auto label = label_of (destination);
    if (!(label).has_value ()) return label.error ();
    auto jumped = InstructionBuilder ((block).value ()).jump ((label).value ());
    if (!(jumped).has_value ()) return jumped.error ();
    return (cleanup).value ();
  }
  Result<void> freeze_exit (Instruction &instruction) {
    std::vector<Operand> operands (instruction.operands ().begin (),
                                   instruction.operands ().end ());
    bool changed = false;
    for (std::size_t n = 0; n < operands.size (); ++n)
      if (detail::operand_access (operands[n]).get<Memory> ()) {
        if (instruction.opcode () == Opcode::JCall && n != 1) continue;
        Type type = instruction.opcode () == Opcode::Ret ? function_.signature ().results[n]
                                                         : Type::pointer ();
        auto value = detail::access (function_).create_register (type);
        if (!(value).has_value ()) return value.error ();
        auto loaded = rewriter_.insert_before (instruction, move (type),
                                               {Operand ((value).value ()), operands[n]});
        if (!(loaded).has_value ()) return loaded.error ();
        operands[n] = Operand ((value).value ());
        changed = true;
      }
    if (changed) return rewriter_.replace (instruction, instruction.opcode (), operands);
    return {};
  }
  Result<void> indirect (BasicBlock &source, Instruction &instruction) {
    auto scope = source.stack_scope ().get ();
    auto common = indirect_targets_.empty ()
                    ? transition (scope, nullptr)
                    : transition (scope, indirect_targets_.front ()->stack_scope ().get ());
    bool same
      = std::all_of (indirect_targets_.begin (), indirect_targets_.end (), [&] (auto target) {
          return transition (scope, target->stack_scope ().get ()) == common;
        });
    if (same && common == transition (scope, nullptr)) {
      if (common.empty ()) return {};
      auto frozen = freeze_exit (instruction);
      if (!(frozen).has_value ()) return frozen;
      return actions (source, common, &instruction);
    }
    ++stats_.indirect_dispatches;
    auto destination = detail::access (function_).create_register (Type::pointer ());
    if (!(destination).has_value ()) return destination.error ();
    auto address = detail::access (function_).create_register (Type::pointer ());
    if (!(address).has_value ()) return address.error ();
    auto evaluated
      = rewriter_.insert_before (instruction, Opcode::Mov,
                                 {Operand ((destination).value ()), instruction.operands ()[0]});
    if (!(evaluated).has_value ()) return evaluated.error ();
    BasicBlock *check = &source;
    for (std::size_t n = 0; n < indirect_targets_.size (); ++n) {
      auto original = label_of (*indirect_targets_[n]);
      if (!(original).has_value ()) return original.error ();
      auto target = target_of (scope, *indirect_targets_[n]);
      if (!(target).has_value ()) return target.error ();
      if (!n) {
        auto loaded
          = rewriter_.insert_before (instruction, Opcode::LAddr,
                                     {Operand ((address).value ()), Operand ((original).value ())});
        if (!(loaded).has_value ()) return loaded.error ();
        const Operand operands[]{Operand ((target).value ()), Operand ((destination).value ()),
                                 Operand ((address).value ())};
        auto replaced = rewriter_.replace (instruction, Opcode::Beq, operands);
        if (!(replaced).has_value ()) return replaced;
      } else {
        auto block = detail::access (function_).create_block_after (*check);
        if (!(block).has_value ()) return block.error ();
        check = &(block).value ();
        InstructionBuilder builder (*check);
        auto loaded = builder.emit (Opcode::LAddr,
                                    {Operand ((address).value ()), Operand ((original).value ())});
        if (!(loaded).has_value ()) return loaded.error ();
        auto branched = builder.emit (Opcode::Beq, {Operand ((target).value ()),
                                                    Operand ((destination).value ()),
                                                    Operand ((address).value ())});
        if (!(branched).has_value ()) return branched.error ();
      }
    }
    // The fallback retains the original indirect transfer after leaving active scopes.
    auto fallback = detail::access (function_).create_block_after (*check);
    if (!(fallback).has_value ()) return fallback.error ();
    auto restored = actions ((fallback).value (), transition (scope, nullptr));
    if (!(restored).has_value ()) return restored;
    auto jumped = InstructionBuilder ((fallback).value ())
                    .emit (Opcode::JmpIndirect, {Operand ((destination).value ())});
    if (!(jumped).has_value ()) return jumped.error ();
    return {};
  }
};
}  // namespace
Result<MaterializationStats> materialize_stack_scopes (Function &function) {
  if ((function.module ().error ()).has_value ()) return (function.module ().error ()).value ();
  auto valid = function.verify ();
  if (!(valid).has_value ()) return valid.error ();
  if (!function.needs_materialization ()) return MaterializationStats{};
  auto result = Materializer (function).run ();
  if (!(result).has_value ()) return result.error ();
  detail::access (function).finish_stack_materialization ();
  valid = function.verify ();
  if (!(valid).has_value ())
    return Error{valid.error ().code, std::string (function.symbol ().name ())
                                        + ": stack-materialization: " + valid.error ().message};
  return result;
}
bool needs_materialization (const Program &context) {
  for (auto &module : context.modules ()) {
    for (auto &function : module.functions ())
      if (function.needs_materialization ()) return true;
    for (auto &data : module.data ())
      if (auto expression = std::get_if<ExpressionData> (&data.initializer ()))
        for (auto &block : detail::NodeAccess::function (*(expression))->blocks ())
          for (auto &instruction : block.instructions ())
            for (auto &operand : instruction.operands ())
              if (auto memory = detail::operand_access (operand).get<Memory> ();
                  memory && memory->literal_source ().get ())
                return true;
  }
  return false;
}
Result<MaterializationStats> materialize_stack_scopes (Program &context) {
  auto valid = context.verify ();
  if (!(valid).has_value ()) return valid.error ();
  MaterializationStats total;
  for (auto &module : context.modules ())
    for (auto &function : module.functions ()) {
      auto result = materialize_stack_scopes (function);
      if (!(result).has_value ()) return result.error ();
      total.saves += result.value ().saves;
      total.restores += result.value ().restores;
      total.cleanup_blocks += result.value ().cleanup_blocks;
      total.indirect_dispatches += result.value ().indirect_dispatches;
    }
  return total;
}
Result<Program> materialize (const Program &context) {
  auto copy = clone (context);
  if (!(copy).has_value ()) return copy.error ();
  auto result = materialize_stack_scopes ((copy).value ());
  if (!(result).has_value ()) return result.error ();
  for (auto &module : copy.value ().modules ())
    for (auto &data : module.data ())
      if (auto expression = std::get_if<ExpressionData> (&data.initializer ())) {
        auto &function = *const_cast<Function *> (detail::NodeAccess::function (*(expression)));
        InstructionRewriter rewriter (function);
        for (auto &block : function.blocks ())
          for (auto &instruction : block.instructions ())
            for (std::size_t n = 0; n < instruction.operands ().size (); ++n)
              if (auto memory = detail::operand_access (instruction.operands ()[n]).get<Memory> ();
                  memory && memory->literal_source ().get ()) {
                auto &literal
                  = std::get<InitializedData> (memory->literal_source ().get ()->initializer ())
                      .values.front ();
                auto replaced = rewriter.replace_operand (instruction, n, literal);
                if (!(replaced).has_value ()) return replaced.error ();
              }
      }
  auto valid = copy.value ().verify ();
  if (!(valid).has_value ()) return valid.error ();
  return copy;
}
}  // namespace mirnext::ir
