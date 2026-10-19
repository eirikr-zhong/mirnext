/* Licensed under the MIT License. Scope state is lowered on control-flow edges. */
#include "mirnext/IR/IR.hpp"
#include <algorithm>
#include <unordered_set>

namespace mirnext::ir {
namespace {
struct Transition {
  std::vector<const StackScope *> leave, enter;
  bool empty() const { return leave.empty() && enter.empty(); }
  friend bool operator==(const Transition &, const Transition &) = default;
};
Transition transition(const StackScope *from, const StackScope *to) {
  Transition result;
  while (from != to) {
    if (from && (!to || from->depth() >= to->depth())) { result.leave.push_back(from); from = from->parent(); }
    else { result.enter.push_back(to); to = to->parent(); }
  }
  std::reverse(result.enter.begin(), result.enter.end()); return result;
}
Opcode move(Type type) {
  return type.kind() == Type::Kind::F32 ? Opcode::FMov : type.kind() == Type::Kind::F64 ? Opcode::DMov
    : type.kind() == Type::Kind::LongDouble ? Opcode::LDMov : Opcode::Mov;
}
class Materializer {
public:
  explicit Materializer(Function &function) : function_(function), rewriter_(function) {}
  Result<MaterializationStats> run() {
    std::unordered_set<const StackScope *> used;
    for (auto &block : function_.blocks()) {
      blocks_.push_back(&block);
      for (auto scope = block.stack_scope(); scope && used.insert(scope).second; scope = scope->parent()) {}
      if (!block.labels().empty()) indirect_targets_.push_back(&block);
    }
    for (auto &scope : function_.stack_scopes()) if (used.contains(&scope)) {
      auto token = function_.create_register(Type::pointer()); if (!token) return token.error(); tokens_.emplace(&scope, *token);
    }
    // Keep the original first block address distinct from the one-time entry save.
    if (blocks_.front()->stack_scope()) {
      auto entry = function_.create_block_before(*blocks_.front()); if (!entry) return entry.error();
      auto saved = actions(**entry, transition(nullptr, blocks_.front()->stack_scope())); if (!saved) return saved.error();
      auto label = label_of(*blocks_.front()); if (!label) return label.error();
      auto jumped = IRBuilder(**entry).jump(**label); if (!jumped) return jumped.error();
    }
    for (std::size_t n = 0; n < blocks_.size(); ++n) {
      auto &block = *blocks_[n]; auto source = block.stack_scope();
      auto terminator = const_cast<Instruction *>(block.terminator());
      if (terminator) {
        auto opcode = terminator->opcode();
        if (opcode == Opcode::Ret || opcode == Opcode::JRet || opcode == Opcode::JCall) {
          if (source) {
            auto frozen = freeze_exit(*terminator); if (!frozen) return frozen.error();
            auto restored = actions(block, transition(source, nullptr), terminator); if (!restored) return restored.error();
          }
          continue;
        }
        if (opcode == Opcode::JmpIndirect) {
          auto lowered = indirect(block, *terminator); if (!lowered) return lowered.error(); continue;
        }
        if (opcode == Opcode::Switch || opcode == Opcode::Jmp || instruction_descriptor(opcode)->flow == ControlFlow::Branch) {
          for (std::size_t index = 0; index < terminator->operands().size(); ++index) {
            auto label = terminator->operands()[index].get<const Label *>(); if (!label) continue;
            if (source == (*label)->block().stack_scope()) continue;
            auto target = target_of(source, const_cast<BasicBlock &>((*label)->block())); if (!target) return target.error();
            if (*target != *label) { auto replaced = rewriter_.replace_operand(*terminator, index, Operand(**target)); if (!replaced) return replaced.error(); }
          }
        }
        if (instruction_descriptor(opcode)->flow != ControlFlow::Branch) continue;
      }
      if (n + 1 == blocks_.size()) continue;
      auto &next = *blocks_[n + 1]; auto change = transition(source, next.stack_scope()); if (change.empty()) continue;
      if (!terminator) { auto lowered = actions(block, change); if (!lowered) return lowered.error(); }
      else {
        auto cleanup = function_.create_block_after(block); if (!cleanup) return cleanup.error(); ++stats_.cleanup_blocks;
        auto lowered = actions(**cleanup, change); if (!lowered) return lowered.error();
        auto label = label_of(next); if (!label) return label.error(); auto jumped = IRBuilder(**cleanup).jump(**label); if (!jumped) return jumped.error();
      }
    }
    for (auto &block : function_.blocks()) { auto cleared = rewriter_.set_stack_scope(block, nullptr); if (!cleared) return cleared.error(); }
    return stats_;
  }
private:
  Function &function_; IRRewriter rewriter_; MaterializationStats stats_;
  std::vector<BasicBlock *> blocks_, indirect_targets_;
  std::unordered_map<const StackScope *, const Register *> tokens_;
  std::unordered_map<const StackScope *, std::unordered_map<const BasicBlock *, const Label *>> cleanups_;
  Result<const Label *> label_of(BasicBlock &block) {
    if (!block.labels().empty()) return block.labels().front();
    auto label = function_.create_label(block); if (!label) return label.error(); return static_cast<const Label *>(*label);
  }
  Result<void> actions(BasicBlock &block, const Transition &change, Instruction *before = nullptr) {
    for (bool entering : {false, true}) for (auto scope : entering ? change.enter : change.leave) {
      auto opcode = entering ? Opcode::BStart : Opcode::BEnd;
      auto instruction = before ? rewriter_.insert_before(*before, opcode, {Operand(*tokens_.at(scope))})
        : IRBuilder(block).emit(opcode, {Operand(*tokens_.at(scope))});
      if (!instruction) return instruction.error();
      if (entering) ++stats_.saves; else ++stats_.restores;
    }
    return {};
  }
  Result<const Label *> target_of(const StackScope *source, BasicBlock &destination) {
    auto change = transition(source, destination.stack_scope()); if (change.empty()) return label_of(destination);
    auto &cache = cleanups_[source]; auto found = cache.find(&destination); if (found != cache.end()) return found->second;
    auto block = function_.create_block(); if (!block) return block.error(); ++stats_.cleanup_blocks;
    auto cleanup = label_of(**block); if (!cleanup) return cleanup.error(); cache.emplace(&destination, *cleanup);
    auto lowered = actions(**block, change); if (!lowered) return lowered.error();
    auto label = label_of(destination); if (!label) return label.error(); auto jumped = IRBuilder(**block).jump(**label); if (!jumped) return jumped.error();
    return *cleanup;
  }
  Result<void> freeze_exit(Instruction &instruction) {
    std::vector<Operand> operands(instruction.operands().begin(), instruction.operands().end()); bool changed = false;
    for (std::size_t n = 0; n < operands.size(); ++n) if (operands[n].get<Memory>()) {
      if (instruction.opcode() == Opcode::JCall && n != 1) continue;
      Type type = instruction.opcode() == Opcode::Ret ? function_.signature().results[n] : Type::pointer();
      auto value = function_.create_register(type); if (!value) return value.error();
      auto loaded = rewriter_.insert_before(instruction, move(type), {Operand(**value), operands[n]}); if (!loaded) return loaded.error();
      operands[n] = Operand(**value); changed = true;
    }
    if (changed) return rewriter_.replace(instruction, instruction.opcode(), operands); return {};
  }
  Result<void> indirect(BasicBlock &source, Instruction &instruction) {
    auto scope = source.stack_scope(); auto common = indirect_targets_.empty() ? transition(scope, nullptr)
      : transition(scope, indirect_targets_.front()->stack_scope());
    bool same = std::all_of(indirect_targets_.begin(), indirect_targets_.end(), [&](auto target) { return transition(scope, target->stack_scope()) == common; });
    if (same && common == transition(scope, nullptr)) {
      if (common.empty()) return {};
      auto frozen = freeze_exit(instruction); if (!frozen) return frozen;
      return actions(source, common, &instruction);
    }
    ++stats_.indirect_dispatches;
    auto destination = function_.create_register(Type::pointer()); if (!destination) return destination.error();
    auto address = function_.create_register(Type::pointer()); if (!address) return address.error();
    auto evaluated = rewriter_.insert_before(instruction, Opcode::Mov, {Operand(**destination), instruction.operands()[0]}); if (!evaluated) return evaluated.error();
    BasicBlock *check = &source;
    for (std::size_t n = 0; n < indirect_targets_.size(); ++n) {
      auto original = label_of(*indirect_targets_[n]); if (!original) return original.error();
      auto target = target_of(scope, *indirect_targets_[n]); if (!target) return target.error();
      if (!n) {
        auto loaded = rewriter_.insert_before(instruction, Opcode::LAddr, {Operand(**address), Operand(**original)}); if (!loaded) return loaded.error();
        const Operand operands[]{Operand(**target), Operand(**destination), Operand(**address)};
        auto replaced = rewriter_.replace(instruction, Opcode::Beq, operands); if (!replaced) return replaced;
      } else {
        auto block = function_.create_block_after(*check); if (!block) return block.error(); check = *block;
        IRBuilder builder(*check); auto loaded = builder.emit(Opcode::LAddr, {Operand(**address), Operand(**original)}); if (!loaded) return loaded.error();
        auto branched = builder.emit(Opcode::Beq, {Operand(**target), Operand(**destination), Operand(**address)}); if (!branched) return branched.error();
      }
    }
    // The fallback retains the original indirect transfer after leaving active scopes.
    auto fallback = function_.create_block_after(*check); if (!fallback) return fallback.error();
    auto restored = actions(**fallback, transition(scope, nullptr)); if (!restored) return restored;
    auto jumped = IRBuilder(**fallback).emit(Opcode::JmpIndirect, {Operand(**destination)}); if (!jumped) return jumped.error(); return {};
  }
};
}
Result<MaterializationStats> materialize_stack_scopes(Function &function) {
  if (function.module().error()) return *function.module().error();
  auto valid = function.verify(); if (!valid) return valid.error();
  if (!function.needs_materialization()) return MaterializationStats{};
  auto result = Materializer(function).run(); if (!result) return result.error();
  for (auto &scope : function.scopes_) scope->materialized_ = true;
  function.changed(true);
  valid = function.verify(); if (!valid) return Error{valid.error().code, std::string(function.symbol().name()) + ": stack-materialization: " + valid.error().message};
  return result;
}
bool needs_materialization(const Context &context) {
  for (auto &module : context.modules()) {
    for (auto &function : module.functions()) if (function.needs_materialization()) return true;
    for (auto &data : module.data()) if (auto expression = std::get_if<ExpressionData>(&data.initializer()))
      for (auto &block : expression->function->blocks()) for (auto &instruction : block.instructions())
        for (auto &operand : instruction.operands()) if (auto memory = operand.get<Memory>(); memory && memory->literal_source()) return true;
  }
  return false;
}
Result<MaterializationStats> materialize_stack_scopes(Context &context) {
  auto valid = context.verify(); if (!valid) return valid.error(); MaterializationStats total;
  for (auto &module : context.modules()) for (auto &function : module.functions()) {
    auto result = materialize_stack_scopes(function); if (!result) return result.error();
    total.saves += result->saves; total.restores += result->restores; total.cleanup_blocks += result->cleanup_blocks; total.indirect_dispatches += result->indirect_dispatches;
  }
  return total;
}
Result<std::unique_ptr<Context>> materialize(const Context &context) {
  auto copy = clone(context); if (!copy) return copy.error();
  auto result = materialize_stack_scopes(**copy); if (!result) return result.error();
  for (auto &module : (*copy)->modules()) for (auto &data : module.data())
    if (auto expression = std::get_if<ExpressionData>(&data.initializer())) {
      auto &function = *const_cast<Function *>(expression->function); IRRewriter rewriter(function);
      for (auto &block : function.blocks()) for (auto &instruction : block.instructions())
        for (std::size_t n = 0; n < instruction.operands().size(); ++n)
          if (auto memory = instruction.operands()[n].get<Memory>(); memory && memory->literal_source()) {
            auto &literal = std::get<InitializedData>(memory->literal_source()->initializer()).values.front();
            auto replaced = rewriter.replace_operand(instruction, n, literal); if (!replaced) return replaced.error();
          }
    }
  auto valid = (*copy)->verify(); if (!valid) return valid.error(); return copy;
}
} // namespace mirnext::ir
