/* Licensed under the MIT License. All edits validate before publishing a change. */
#include "mirnext/IR/IR.hpp"
#include <algorithm>
#include <unordered_set>

namespace mirnext::ir {
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidOperand, std::move(message)}; }
Opcode move_opcode(Type type) {
  switch (type.kind()) {
  case Type::Kind::F32: return Opcode::FMov;
  case Type::Kind::F64: return Opcode::DMov;
  case Type::Kind::LongDouble: return Opcode::LDMov;
  default: return Opcode::Mov;
  }
}
bool affects_cfg(Opcode opcode) {
  return instruction_descriptor(opcode)->flow != ControlFlow::Next || opcode == Opcode::LAddr;
}
}
Error IRBuilder::fail(Error error) {
  auto &module = *function_->module_;
  error.message = std::string(function_->symbol().name()) + ": builder: " + error.message;
  return module.fail(std::move(error));
}
Result<void> IRBuilder::set_insert_point(BasicBlock &block) {
  if (&block.function() != function_) return fail(invalid("insert block belongs to another function"));
  block_ = &block; scope_ = block.stack_scope(); return {};
}
Result<BasicBlock *> IRBuilder::create_block(std::string_view name) {
  if (function_->module().error()) return *function_->module().error();
  auto created = function_->create_block(name, scope_); if (!created) return fail(created.error()); return created;
}
Result<StackScope *> IRBuilder::begin_stack_scope(std::string_view name) {
  if (function_->module().error()) return *function_->module().error();
  if (!block_ || block_->terminator()) return fail(invalid("stack scope requires an open insertion block"));
  auto scope = function_->create_stack_scope(scope_, name); if (!scope) return fail(scope.error());
  auto entry = function_->create_block_after(*block_, {}, *scope); if (!entry) return fail(entry.error());
  block_ = *entry; scope_ = *scope; return scope;
}
Result<void> IRBuilder::end_stack_scope(const StackScope &scope) {
  if (function_->module().error()) return *function_->module().error();
  if (scope_ != &scope || &scope.function() != function_ || scope.materialized()) return fail(invalid("stack scopes must close in nesting order"));
  if (block_) {
    auto terminator = block_->terminator();
    if (terminator && (terminator->opcode() == Opcode::Ret || terminator->opcode() == Opcode::JRet || terminator->opcode() == Opcode::JCall)) block_ = nullptr;
    else {
      auto continuation = function_->create_block_after(*block_, {}, scope.parent());
      if (!continuation) return fail(continuation.error()); block_ = *continuation;
    }
  }
  scope_ = scope.parent(); return {};
}
Result<Instruction *> IRBuilder::emit(Opcode opcode, std::span<const Operand> operands) {
  auto phase = function_->require_register_ir(); if (!phase) return phase.error();
  if (function_->module().error()) return *function_->module().error();
  if (!block_) return fail({ErrorCode::NoInsertPoint, "no insertion block"});
  if (block_->terminator()) return fail(invalid("insertion block already has a terminator"));
  auto valid = validate_instruction(*function_, opcode, operands); if (!valid) return fail(valid.error());
  return append_validated(opcode, std::vector<Operand>(operands.begin(), operands.end()));
}
Instruction *IRBuilder::append_validated(Opcode opcode, std::vector<Operand> operands) {
  block_->instructions_.emplace_back(Instruction::ConstructionKey{}, *block_, opcode, std::move(operands));
  auto &instruction = block_->instructions_.back(); instruction.position_ = std::prev(block_->instructions_.end());
  function_->changed(affects_cfg(opcode)); return &instruction;
}
Result<const Register *> IRBuilder::binary(Opcode opcode, Operand lhs, Operand rhs, std::string_view name) {
  if (function_->module().error()) return *function_->module().error();
  if (!block_ || block_->terminator()) return fail(invalid("binary operation requires an open insertion block"));
  auto descriptor = instruction_descriptor(opcode);
  if (!descriptor || descriptor->count != OperandCount::Fixed || descriptor->roles.size() != 3
      || descriptor->roles[0] < 'A' || descriptor->roles[0] > 'Z' || descriptor->flow != ControlFlow::Next)
    return fail(invalid("opcode is not a binary value operation"));
  Type type = descriptor->roles[0] == 'F' ? Type(Type::Kind::F32) : descriptor->roles[0] == 'D' ? Type(Type::Kind::F64)
    : descriptor->roles[0] == 'E' ? Type(Type::Kind::LongDouble) : Type::i64();
  const Operand inputs[]{std::move(lhs), std::move(rhs)};
  return value_operation(opcode, inputs, type, name);
}
Result<Instruction *> IRBuilder::assign(const Register &reg, Operand operand) {
  return emit(move_opcode(reg.type()), {Operand(reg), std::move(operand)});
}
Result<const Register *> IRBuilder::value_operation(Opcode opcode, std::span<const Operand> inputs, Type type, std::string_view name) {
  auto phase = function_->require_register_ir(); if (!phase) return phase.error();
  if (function_->module().error()) return *function_->module().error();
  if (!block_ || block_->terminator()) return fail(invalid("value operation requires an open insertion block"));
  Register candidate(*function_, type, {}, {}, false);
  std::vector<Operand> operands; operands.reserve(inputs.size() + 1); operands.emplace_back(candidate);
  operands.insert(operands.end(), inputs.begin(), inputs.end());
  auto valid = validate_instruction(*function_, opcode, operands); if (!valid) return fail(valid.error());
  auto reg = function_->create_register(type, name); if (!reg) return fail(reg.error()); operands[0] = Operand(**reg);
  append_validated(opcode, std::move(operands)); return static_cast<const Register *>(*reg);
}
Result<const Register *> IRBuilder::unary(Opcode opcode, Operand input, std::string_view name) {
  auto descriptor = instruction_descriptor(opcode);
  if (!descriptor || descriptor->count != OperandCount::Fixed || descriptor->roles.size() != 2
      || descriptor->roles[0] < 'A' || descriptor->roles[0] > 'Z' || descriptor->flow != ControlFlow::Next)
    return fail(invalid("opcode is not a unary value operation"));
  Type type = descriptor->roles[0] == 'F' ? Type(Type::Kind::F32) : descriptor->roles[0] == 'D' ? Type(Type::Kind::F64)
    : descriptor->roles[0] == 'E' ? Type(Type::Kind::LongDouble) : Type::i64();
  return value_operation(opcode, std::span<const Operand>(&input, 1), type, name);
}
Result<const Register *> IRBuilder::allocate_stack(Operand size, std::string_view name) {
  return value_operation(Opcode::Alloca, std::span<const Operand>(&size, 1), Type::pointer(), name);
}
Result<const Register *> IRBuilder::stack_save(std::string_view name) { return value_operation(Opcode::BStart, {}, Type::pointer(), name); }
Result<Instruction *> IRBuilder::stack_restore(const Register &token) {
  if (token.type() != Type::pointer()) return fail(invalid("stack restore requires a pointer token"));
  return emit(Opcode::BEnd, {Operand(token)});
}
Result<const Register *> IRBuilder::address_of(const Register &reg, std::string_view name) {
  auto kind = reg.type().kind(); auto opcode = kind == Type::Kind::I8 || kind == Type::Kind::U8 ? Opcode::Addr8
    : kind == Type::Kind::I16 || kind == Type::Kind::U16 ? Opcode::Addr16
    : kind == Type::Kind::I32 || kind == Type::Kind::U32 ? Opcode::Addr32 : Opcode::Addr;
  Operand input(reg); return value_operation(opcode, std::span<const Operand>(&input, 1), Type::pointer(), name);
}
Result<const Register *> IRBuilder::address_of(const Symbol &symbol, std::int64_t displacement, std::string_view name) {
  Operand input(SymbolReference{&symbol, displacement}); return value_operation(Opcode::Mov, std::span<const Operand>(&input, 1), Type::pointer(), name);
}
Result<const Register *> IRBuilder::address_of(const Label &label, std::string_view name) {
  Operand input(label); return value_operation(Opcode::LAddr, std::span<const Operand>(&input, 1), Type::pointer(), name);
}
Result<const Register *> IRBuilder::load(Memory memory, std::string_view name) {
  auto type = memory.type; Operand input(std::move(memory));
  return value_operation(move_opcode(type), std::span<const Operand>(&input, 1), type, name);
}
Result<Instruction *> IRBuilder::store(Memory memory, Operand value) {
  auto opcode = move_opcode(memory.type); return emit(opcode, {Operand(std::move(memory)), std::move(value)});
}
Result<Instruction *> IRBuilder::branch_if(Operand condition, const Label &target, bool when_true) {
  return emit(when_true ? Opcode::Bt : Opcode::Bf, {Operand(target), std::move(condition)});
}
Result<Instruction *> IRBuilder::switch_to(Operand index, std::span<const Label *const> labels) {
  std::vector<Operand> operands; operands.reserve(labels.size() + 1); operands.push_back(std::move(index));
  for (auto label : labels) { if (!label) return fail(invalid("null switch label")); operands.emplace_back(*label); }
  return emit(Opcode::Switch, operands);
}
Result<Instruction *> IRBuilder::jump(const Label &label) { return emit(Opcode::Jmp, {Operand(label)}); }
Result<Instruction *> IRBuilder::ret(std::span<const Operand> operands) { return emit(Opcode::Ret, operands); }

bool output_operand(const Function &, const Instruction &instruction, std::size_t n) {
  auto descriptor = instruction_descriptor(instruction.opcode());
  if (!descriptor) return false;
  if (descriptor->count == OperandCount::Call) {
    if (instruction.operands().empty()) return false;
    auto symbol = instruction.operands()[0].get<SymbolReference>();
    return symbol && symbol->symbol && symbol->symbol->signature() && n >= 2 && n - 2 < symbol->symbol->signature()->results.size();
  }
  return n < descriptor->roles.size() && descriptor->roles[n] >= 'A' && descriptor->roles[n] <= 'Z';
}
const MemoryLocation &IRRewriter::create_memory_location() {
  auto location = std::unique_ptr<MemoryLocation>(new MemoryLocation(function_,function_.memory_locations_.size()));
  function_.memory_locations_.push_back(std::move(location)); return *function_.memory_locations_.back();
}
Result<void> IRRewriter::set_memory_location(Instruction &instruction,std::size_t operand,const MemoryLocation *location) {
  auto phase = writable(); if (!phase) return phase;
  if (&instruction.function() != &function_ || (location && &location->function() != &function_)) return invalid("memory location owner mismatch");
  if (operand >= instruction.operands_.size()) return invalid("memory location operand out of bounds");
  auto memory = std::get_if<Memory>(&instruction.operands_[operand].value_);
  if (!memory) return invalid("memory location requires a memory operand");
  if (memory->location_ != location) { memory->location_ = location; function_.changed(false); } return {};
}
Result<void> IRRewriter::replace(Instruction &instruction, Opcode opcode, std::span<const Operand> operands) {
  return replace_owned(instruction, opcode, std::vector<Operand>(operands.begin(), operands.end()));
}
Result<Instruction *> IRRewriter::insert_before(Instruction &before, Opcode opcode, std::span<const Operand> operands) {
  if (&before.function() != &function_) return invalid("insertion anchor belongs to another function");
  return insert(*before.block_, &before, opcode, operands);
}
Result<Instruction *> IRRewriter::insert(BasicBlock &block, const Instruction *before, Opcode opcode, std::span<const Operand> operands) {
  return insert(block, before, opcode, std::vector<Operand>(operands.begin(), operands.end()));
}
Result<Instruction *> IRRewriter::insert(BasicBlock &block, const Instruction *before, Opcode opcode, std::vector<Operand> &&operands) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&block.function() != &function_) return invalid("insertion block belongs to another function");
  if (before && &before->block() != &block) return invalid("insertion anchor belongs to another block");
  auto checked = validate_instruction(function_, opcode, operands); if (!checked) return checked.error();
  if (before && instruction_descriptor(opcode)->flow != ControlFlow::Next)
    return invalid("cannot insert a terminator before an instruction");
  if (!before && block.terminator()) return invalid("cannot insert after a terminator");
  auto &body = block.instructions_;
  auto position = body.emplace(before ? before->position_ : body.end(), Instruction::ConstructionKey{}, block, opcode,
                               std::move(operands));
  position->position_ = position; function_.changed(affects_cfg(opcode)); return &*position;
}
Result<Instruction *> IRRewriter::insert_after(Instruction &after, Opcode opcode, std::span<const Operand> operands) {
  if (&after.function() != &function_) return invalid("insertion anchor belongs to another function");
  if (instruction_descriptor(after.opcode())->flow != ControlFlow::Next) return invalid("cannot insert after a terminator");
  auto next = std::next(after.position_);
  return insert(*after.block_, next == after.block_->instructions_.end() ? nullptr : &*next, opcode, operands);
}
Result<BasicBlock *> IRRewriter::split_before(Instruction &before, std::string_view name) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&before.function() != &function_) return invalid("split anchor belongs to another function");
  auto &source = *before.block_;
  auto created = function_.create_block_after(source, name, source.scope_); if (!created) return created;
  auto &destination = **created;
  destination.instructions_.splice(destination.instructions_.end(), source.instructions_, before.position_, source.instructions_.end());
  for (auto &instruction : destination.instructions_) instruction.block_ = &destination;
  return created;
}
Result<void> IRRewriter::merge_fallthrough(BasicBlock &block) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&block.function() != &function_) return invalid("merge block belongs to another function");
  auto next = std::next(block.position_);
  if (next == function_.blocks_.end()) return invalid("merge requires a following block");
  auto &successor = **next;
  if (block.terminator() || !successor.labels_.empty() || block.scope_ != successor.scope_)
    return invalid("merge requires unlabeled fallthrough on the same stack scope");
  for (auto &instruction : successor.instructions_) instruction.block_ = &block;
  block.instructions_.splice(block.instructions_.end(), successor.instructions_);
  if (!successor.name().empty()) function_.block_names_.erase(successor.name());
  function_.blocks_.erase(next); function_.changed(true); return {};
}
Result<std::size_t> IRRewriter::separate_label_anchors(BasicBlock &block) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&block.function() != &function_) return invalid("label anchors belong to another function");
  if (function_.module().error()) return *function_.module().error();
  if (block.labels_.size() < 2) return std::size_t{0};
  auto labels = block.labels_; const auto count = labels.size() - 1;
  for (std::size_t n = 0; n < count; ++n) {
    auto prefix = function_.create_block_before(block, {}, block.scope_); if (!prefix) return prefix.error();
    auto label = const_cast<Label *>(labels[n]); label->block_ = *prefix; (*prefix)->labels_.push_back(label);
  }
  block.labels_.assign(1, labels.back()); return count;
}
Result<std::size_t> IRRewriter::erase_unused_labels() {
  auto phase = writable(); if (!phase) return phase.error();
  if (function_.labels_.empty()) return 0;
  std::unordered_set<const Label *> used;
  used.reserve(function_.labels_.size());
  for (auto &block : function_.blocks_) for (auto &instruction : block->instructions_)
    for (auto &operand : instruction.operands()) if (auto label = operand.get<const Label *>()) used.insert(*label);
  for (auto data : function_.label_data_uses()) if (auto reference = std::get_if<LabelReference>(&data->initializer())) {
    used.insert(reference->label);
    if (reference->base) used.insert(reference->base);
  }
  for (auto &block : function_.blocks_) std::erase_if(block->labels_, [&](auto label) { return !used.contains(label); });
  const auto erased = std::erase_if(function_.labels_, [&](auto &label) {
    if (used.contains(label.get())) return false;
    if (!label->name().empty()) function_.label_names_.erase(label->name());
    return true;
  });
  if (erased) function_.changed(true);
  return erased;
}
Result<void> IRRewriter::erase_blocks(std::span<BasicBlock *const> blocks) {
  auto phase = writable(); if (!phase) return phase.error();
  std::unordered_set<const BasicBlock *> removed;
  removed.reserve(blocks.size());
  for (auto block : blocks) {
    if (!block || &block->function() != &function_) return invalid("erased block belongs to another function");
    removed.insert(block);
  }
  if (removed.empty()) return {};
  if (removed.size() == function_.blocks_.size()) return invalid("cannot erase all function blocks");
  const BasicBlock *last = nullptr;
  for (auto &block : function_.blocks_) if (!removed.contains(block.get())) last = block.get();
  auto terminal = last->terminator();
  const bool jump_tail = !last->instructions_.empty() && last->instructions_.back().opcode() == Opcode::JCall;
  if (!jump_tail && (!terminal || (instruction_descriptor(terminal->opcode())->flow != ControlFlow::Exit
      && terminal->opcode() != Opcode::Jmp && terminal->opcode() != Opcode::JmpIndirect && terminal->opcode() != Opcode::Switch)))
    return invalid("erasing blocks would leave a final fallthrough");
  auto retained = [&](const Label *label) { return !removed.contains(&label->block()); };
  for (auto &block : function_.blocks_) if (!removed.contains(block.get()))
    for (auto &instruction : block->instructions_) for (auto &operand : instruction.operands())
      if (auto label = operand.get<const Label *>(); label && !retained(*label))
        return invalid("erased block is referenced by a surviving instruction");
  for (auto data : function_.label_data_uses()) if (auto reference = std::get_if<LabelReference>(&data->initializer()))
    if (!retained(reference->label) || (reference->base && !retained(reference->base)))
      return invalid("erased block is referenced by label data");
  std::erase_if(function_.labels_, [&](auto &label) {
    if (retained(label.get())) return false;
    if (!label->name().empty()) function_.label_names_.erase(label->name());
    return true;
  });
  for (auto block : blocks) if (removed.erase(block)) {
    if (!block->name().empty()) function_.block_names_.erase(block->name());
    function_.blocks_.erase(block->position_);
  }
  function_.changed(true); return {};
}
Result<Memory> IRRewriter::literal_memory(const GlobalData &source, const Register &address) const {
  auto data = std::get_if<InitializedData>(&source.initializer());
  if (&source.symbol().module() != &function_.module() || !source.is_literal_pool() || source.writable()
      || !data || !data->type.is_float() || data->values.size() != 1
      || &address.function() != &function_ || !address.type().is_integer())
    return invalid("invalid literal pool or address owner");
  Memory memory{data->type, &address}; memory.literal_source_ = &source; return memory;
}
BasicBlock *IRRewriter::next_block(const BasicBlock &block) const {
  if (&block.function() != &function_) return nullptr;
  auto next = std::next(block.position_); return next == function_.blocks_.end() ? nullptr : next->get();
}
Instruction *IRRewriter::next_instruction(const Instruction &instruction) const {
  if (&instruction.function() != &function_) return nullptr;
  auto next = std::next(instruction.position_);
  return next == instruction.block_->instructions_.end() ? nullptr : &*next;
}
Result<void> IRRewriter::set_stack_scope(BasicBlock &block, const StackScope *scope) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&block.function() != &function_ || (scope && (&scope->function() != &function_ || scope->materialized())))
    return invalid("invalid stack scope edit");
  if (block.scope_ != scope) { block.scope_ = scope; function_.changed(true); }
  return {};
}
Result<void> IRRewriter::replace_owned(Instruction &instruction, Opcode opcode, std::vector<Operand> operands) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&instruction.function() != &function_) return invalid("instruction belongs to another function");
  auto &body = instruction.block_->instructions_;
  auto valid = validate_instruction(function_, opcode, operands); if (!valid) return valid;
  auto new_flow = instruction_descriptor(opcode)->flow;
  if (new_flow != ControlFlow::Next && &body.back() != &instruction) return invalid("terminator must be the last instruction");
  bool cfg = affects_cfg(instruction.opcode()) || affects_cfg(opcode);
  if (cfg && instruction.opcode() == opcode && instruction.operands().size() == operands.size()) {
    // SSA register renaming preserves edges and label-address roots.
    cfg = false;
    for (std::size_t n = 0; n < operands.size(); ++n)
      if ((instruction.operands()[n].get<const Label *>() || operands[n].get<const Label *>())
          && instruction.operands()[n] != operands[n]) { cfg = true; break; }
  }
  instruction.opcode_ = opcode; instruction.operands_ = std::move(operands);
  function_.changed(cfg);
  return {};
}
Result<void> IRRewriter::materialize_register_storage(std::span<const Register *const> registers) {
  auto phase = writable(); if (!phase) return phase.error();
  std::unordered_set<const Register *> selected;
  for (auto reg : registers) {
    if (!reg || &reg->function() != &function_) return invalid("register storage crosses function ownership");
    if (!reg->hardware_binding().empty()) return invalid("hardware-bound register cannot have memory storage");
    selected.insert(reg);
  }
  for (auto &block : function_.blocks()) for (auto &instruction : block.instructions())
    for (std::size_t p = 0; p < instruction.operands().size(); ++p) {
      auto code = instruction.opcode();
      bool address = code == Opcode::Addr || code == Opcode::Addr8 || code == Opcode::Addr16 || code == Opcode::Addr32;
      bool transfer = code == Opcode::Mov || code == Opcode::FMov || code == Opcode::DMov || code == Opcode::LDMov;
      if (auto reg = instruction.operands()[p].get<const Register *>(); reg && selected.contains(*reg) && !address && !transfer)
        return invalid("register storage still has an ordinary value use or definition");
      if (auto memory = instruction.operands()[p].get<Memory>(); memory && (selected.contains(memory->base) || selected.contains(memory->index)))
        return invalid("register storage still has a direct memory address use");
    }
  bool changed = false;
  for (auto reg : selected) if (!reg->memory_backed_) { const_cast<Register *>(reg)->memory_backed_ = true; changed = true; }
  if (changed) {
    function_.registers_changed(); function_.changed(false);
  }
  return {};
}
Result<void> IRRewriter::replace_operand(Instruction &instruction, std::size_t index, Operand operand) {
  if (&instruction.function() != &function_ || index >= instruction.operands().size()) return invalid("invalid operand edit");
  auto operands = instruction.operands_;
  operands[index] = std::move(operand);
  return replace_owned(instruction, instruction.opcode(), std::move(operands));
}
Result<void> IRRewriter::erase(Instruction &instruction) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&instruction.function() != &function_) return invalid("instruction belongs to another function");
  const bool cfg = affects_cfg(instruction.opcode());
  instruction.block_->instructions_.erase(instruction.position_); function_.changed(cfg); return {};
}
Result<void> IRRewriter::move_before(Instruction &instruction, BasicBlock &block, const Instruction *before) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&instruction.function() != &function_ || &block.function() != &function_) return invalid("move crosses function ownership");
  auto &source = instruction.block_->instructions_;
  auto &destination = block.instructions_;
  if (before && &before->block() != &block) return invalid("insertion anchor belongs to another block");
  auto to = before ? before->position_ : destination.end();
  if (before == &instruction) return {};
  if (!before && block.terminator() && block.terminator() != &instruction) return invalid("cannot move after a terminator");
  if (instruction_descriptor(instruction.opcode())->flow != ControlFlow::Next && before) return invalid("cannot move terminator before another instruction");
  destination.splice(to, source, instruction.position_); instruction.block_ = &block;
  function_.changed(affects_cfg(instruction.opcode())); return {};
}
Result<BasicBlock *> IRRewriter::insert_empty_block_before(BasicBlock &before) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&before.function() != &function_) return invalid("block insertion crosses function ownership");
  auto block = std::unique_ptr<BasicBlock>(new BasicBlock(function_, {})); auto result = block.get();
  result->scope_ = before.scope_;
  result->position_ = function_.blocks_.insert(before.position_, std::move(block)); function_.changed(true); return result;
}
Result<void> IRRewriter::move_block_before(BasicBlock &block, BasicBlock *before) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&block.function() != &function_ || (before && &before->function() != &function_))
    return invalid("block move crosses function ownership");
  auto position = before ? before->position_ : function_.blocks_.end();
  if (before == &block || std::next(block.position_) == position) return {};
  function_.blocks_.splice(position, function_.blocks_, block.position_);
  function_.changed(true); return {};
}
Result<void> IRRewriter::replace_uses(const Register &old, const Register &replacement) {
  auto phase = writable(); if (!phase) return phase.error();
  if (&old.function() != &function_ || &replacement.function() != &function_ || old.type() != replacement.type())
    return invalid("register replacement has incompatible type or owner");
  if (&old == &replacement) return {};
  if (!old.hardware_binding().empty() || !replacement.hardware_binding().empty()) return invalid("hardware identities cannot be replaced");
  struct Edit { Instruction *instruction; std::vector<Operand> operands; };
  std::vector<Edit> edits;
  for (auto &block : function_.blocks_) for (auto &instruction : block->instructions_) {
    bool changed = false;
    for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
      auto &operand = instruction.operands()[n];
      if (auto reg = operand.get<const Register *>(); reg && *reg == &old && !output_operand(function_, instruction, n)) changed = true;
      if (auto memory = operand.get<Memory>(); memory && (memory->base == &old || memory->index == &old)) changed = true;
    }
    if (!changed) continue;
    auto operands = instruction.operands_;
    for (std::size_t n = 0; n < operands.size(); ++n) {
      auto &operand = operands[n];
      if (auto reg = operand.get<const Register *>(); reg && *reg == &old && !output_operand(function_, instruction, n)) {
        operand = Operand(replacement);
      } else if (auto address = std::get_if<Memory>(&operand.value_)) {
        if (address->base == &old) address->base = &replacement;
        if (address->index == &old) address->index = &replacement;
      }
    }
    auto checked = validate_instruction(function_, instruction.opcode(), operands); if (!checked) return checked;
    edits.push_back({&instruction, std::move(operands)});
  }
  for (auto &edit : edits) edit.instruction->operands_ = std::move(edit.operands);
  if (!edits.empty()) function_.changed(false);
  return {};
}
Result<void> IRRewriter::writable() const {
  return ssa_edit_ ? Result<void>{} : function_.require_register_ir();
}
} // namespace mirnext::ir
