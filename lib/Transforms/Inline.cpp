/* Licensed under the MIT License.
   Inlining derived from MIR, Copyright 2018-2024 Vladimir Makarov. */
#include "mirnext/Transforms/Inline.hpp"
#include <algorithm>
#include <bit>
#include <unordered_set>

namespace mirnext::transforms {
using namespace ir;
namespace {
Error invalid(std::string message) { return {ErrorCode::InvalidOperand, std::move(message)}; }
Type scalar(Type type) { return type.is_float() ? type : Type::i64(); }
Opcode move(Type type) {
  return type.kind() == Type::Kind::F32 ? Opcode::FMov : type.kind() == Type::Kind::F64 ? Opcode::DMov
    : type.kind() == Type::Kind::LongDouble ? Opcode::LDMov : Opcode::Mov;
}
bool call(Opcode opcode) { return instruction_descriptor(opcode)->count == OperandCount::Call; }
Instruction *first(BasicBlock &block) { return block.size() ? &*block.instructions().begin() : nullptr; }
std::size_t mir_size(const Function &function) {
  std::size_t size = function.instruction_count();
  for (auto &block : function.blocks()) size += block.labels().size();
  return size;
}
struct Allocation {
  Instruction *top = nullptr;
  bool used = false, non_top = false;
  std::uint64_t size = 0;
};
Allocation allocations(Function &function) {
  Allocation result; bool allowed = true; const Instruction *previous = nullptr;
  for (auto &block : function.blocks()) {
    if (!block.labels().empty()) { allowed = false; previous = nullptr; }
    for (auto &instruction : block.instructions()) {
      auto &i = instruction;
      if (i.opcode() != Opcode::Alloca) {
        if (result.top && !result.used) {
          auto destination = *result.top->operands()[0].get<const Register *>();
          for (auto &op : i.operands()) {
            if (auto r = op.get<const Register *>()) result.used |= *r == destination;
            if (auto m = op.get<Memory>()) result.used |= m->base == destination || m->index == destination;
          }
        }
      } else {
        const Operand *size = &i.operands()[1];
        if (size->get<const Register *>() && previous && previous->opcode() == Opcode::Mov && previous->operands()[0] == *size)
          size = &previous->operands()[1];
        auto signed_size = size->get<std::int64_t>(); auto unsigned_size = size->get<std::uint64_t>();
        if (!allowed || (!signed_size && !unsigned_size)) {
          result.non_top = true; if (!result.top) return result;
        } else {
          result.top = &i; result.size = signed_size ? static_cast<std::uint64_t>(*signed_size) : *unsigned_size; allowed = false;
        }
      }
      previous = &i;
    }
  }
  return result;
}
Result<std::uint64_t> aligned(std::uint64_t size, std::uint64_t &alignment) {
  if (std::bit_cast<std::int64_t>(size) <= 0) size = 1;
  alignment = size <= 2 ? size : size <= 4 ? 4 : size <= 8 ? 8 : 16;
  if (size > std::uint64_t(INT64_MAX) - alignment + 1) return invalid("inline frame size overflow");
  return (size + alignment - 1) / alignment * alignment;
}
}

struct InlinePass::Symbols {
  Context &context;
  StringMap<const Symbol *> exports;
  std::unordered_set<std::string_view> names;
  struct Imports {
    std::unordered_map<const Symbol *, const Symbol *> symbols, prototypes;
  };
  std::unordered_map<Module *, Imports> imports;
  std::uint64_t next = 0;
  explicit Symbols(Context &context) : context(context) {
    for (auto &module : context.modules()) {
      for (auto &symbol : module.symbols()) {
        names.insert(symbol.name()); if (symbol.exported()) exports.emplace(symbol.name(), &symbol);
      }
    }
  }
  std::string unique() {
    std::string name;
    do { name = ".inline" + std::to_string(next++); } while (names.contains(name));
    names.insert(context.intern_string(name)); return name;
  }
  const Function *resolve(const Symbol &symbol) const {
    if (symbol.function()) return symbol.function();
    if (symbol.kind() != Symbol::Kind::Import) return nullptr;
    auto found = exports.find(symbol.name()); return found == exports.end() ? nullptr : found->second->function();
  }
  Result<const Symbol *> relocate(const Symbol &source, Module &destination, bool prototype) {
    if (&source.module() == &destination) return &source;
    auto &cache = prototype || source.kind() == Symbol::Kind::Prototype ? imports[&destination].prototypes : imports[&destination].symbols;
    if (auto found = cache.find(&source); found != cache.end()) return found->second;
    auto remember = [&](const Symbol *value) -> Result<const Symbol *> { cache.emplace(&source, value); return value; };
    if (prototype || source.kind() == Symbol::Kind::Prototype) {
      if (!source.signature()) return invalid("inline prototype has no signature");
      auto copied = destination.prototype(unique(), *source.signature()); if (!copied) return copied.error(); return remember(*copied);
    }
    auto &owner = const_cast<Module &>(source.module());
    if (source.kind() != Symbol::Kind::Import && !source.exported()) {
      auto renamed = owner.rename_symbol(source, unique()); if (!renamed) return renamed.error();
      auto exported = owner.export_symbol(source); if (!exported) return exported.error();
      exports.emplace(source.name(), &source);
    }
    if (auto existing = destination.find_symbol(source.name())) {
      if (existing->kind() == Symbol::Kind::Import) {
        if (existing->signature() && source.signature() && *existing->signature() != *source.signature())
          return invalid("inline import signature mismatch");
        if (auto variable = source.variable(); variable && !existing->variable()) {
          auto bound = destination.bind_variable(*existing, variable->type(), variable->writable()); if (!bound) return bound.error();
        }
        return remember(existing);
      }
      // Local identities survive the rename, including data and forward references.
      if (existing->exported()) return invalid("inline relocation conflicts with an exported symbol");
      auto renamed = destination.rename_symbol(*existing, unique()); if (!renamed) return renamed.error();
    }
    auto copied = destination.import(source.name(), source.signature() ? std::optional<Signature>(*source.signature()) : std::nullopt);
    if (!copied) return copied.error();
    if (auto variable = source.variable()) {
      auto bound = destination.bind_variable(**copied, variable->type(), variable->writable()); if (!bound) return bound.error();
    }
    return remember(*copied);
  }
};

namespace {
template<class Symbols> class Inliner {
public:
  Inliner(Function &function, Symbols &symbols) : function_(function), rewriter_(function), symbols_(symbols) {}
  Result<std::size_t> run() {
    auto checked = function_.verify(); if (!checked) return checked.error();
    if (function_.needs_materialization()) return invalid("inline requires stack scope materialization and simplify first");
    bool has_calls = false;
    for (auto &block : function_.blocks()) for (auto &i : block.instructions()) has_calls |= call(i.opcode());
    if (!has_calls) return std::size_t{0};
    caller_ = allocations(function_);
    if (caller_.top && caller_.used) {
      auto size = aligned(caller_.size, alignment_); if (!size) return size.error(); initial_ = current_ = maximum_ = *size;
    }
    original_ = count_ = mir_size(function_);
    auto blocks = function_.blocks(); Cursor cursor{blocks.begin() == blocks.end() ? nullptr : &*blocks.begin(), nullptr};
    while (cursor.block) {
      if (!cursor.instruction) {
        while (!frames_.empty() && frames_.back().anchor == cursor.block) { current_ = frames_.back().size; frames_.pop_back(); }
        if (cold_.contains(cursor.block) || !(cursor.instruction = first(*cursor.block))) {
          cursor = {rewriter_.next_block(*cursor.block), nullptr}; continue;
        }
      }
      auto &i = *cursor.instruction; auto next = advance(i);
      if (call(i.opcode())) {
        auto target = i.operands()[1].template get<SymbolReference>();
        auto callee = target && !target->displacement ? symbols_.resolve(*target->symbol) : nullptr;
        if (callee && callee != &function_ && eligible(*callee, i.opcode())) {
          auto expanded = expand(i, *const_cast<Function *>(callee)); if (!expanded) return expanded.error();
          cursor = {*expanded, nullptr}; ++changes_; continue;
        }
      }
      cursor = next;
    }
    if (!frames_.empty()) return invalid("unclosed inline allocation frame");
    if (caller_.top) {
      if (!caller_.used) { auto erased = rewriter_.erase(*caller_.top); if (!erased) return erased.error(); ++changes_; }
      else if (maximum_ != initial_) {
        auto size = temporary(); if (!size) return size.error();
        auto emitted = rewriter_.insert_before(*caller_.top, Opcode::Mov, {Operand(**size), Operand(static_cast<std::int64_t>(maximum_))});
        if (!emitted) return emitted.error();
        auto changed = rewriter_.replace_operand(*caller_.top, 1, Operand(**size)); if (!changed) return changed.error();
        ++changes_;
      }
    }
    // The C cold-code stack appends later expansions before earlier expansions.
    for (auto chunk = cold_chunks_.rbegin(); chunk != cold_chunks_.rend(); ++chunk)
      for (auto block : *chunk) { auto moved = rewriter_.move_block_before(*block); if (!moved) return moved.error(); }
    return changes_;
  }
private:
  struct Cursor { BasicBlock *block; Instruction *instruction; };
  struct Frame { BasicBlock *anchor; std::uint64_t size; };
  Cursor advance(Instruction &i) {
    if (auto next = rewriter_.next_instruction(i)) return {const_cast<BasicBlock *>(&i.block()), next};
    return {rewriter_.next_block(i.block()), nullptr};
  }
  bool eligible(const Function &callee, Opcode opcode) const {
    if ((count_ > original_ / 2 && count_ > 200) || !callee.label_data_uses().empty() || callee.signature().variadic
        || mir_size(callee) > (opcode == Opcode::Call ? 50 : 200)) return false;
    for (auto &block : callee.blocks()) for (auto &i : block.instructions()) if (i.opcode() == Opcode::JRet) return false;
    return true;
  }
  Result<Register *> temporary(Type type = Type::i64()) { return function_.create_register(type); }
  Result<BasicBlock *> expand(Instruction &call_insn, Function &callee) {
    if (callee.needs_materialization()) return invalid("inline callee requires stack scope materialization and simplify first");
    auto features = allocations(callee);
    Instruction *ret = nullptr; unsigned returns = 0;
    std::vector<BasicBlock *> source_blocks;
    for (auto &block : callee.blocks()) {
      source_blocks.push_back(&block);
      for (auto &i : block.instructions()) if (i.opcode() == Opcode::Ret) { ret = &i; ++returns; }
    }
    if (returns != 1 || !ret) return invalid("inline callee requires a unified return from simplify");
    bool after_return = false;
    for (auto block : source_blocks) {
      if (features.non_top && after_return && (block->size() || !block->labels().empty()))
        return invalid("inline dynamic-stack callee return is not the last instruction");
      after_return |= block == &ret->block();
    }
    for (auto &o : ret->operands()) if (!o.get<const Register *>()) return invalid("inline callee requires simplified return operands");
    std::vector<Operand> operands(call_insn.operands().begin(), call_insn.operands().end());
    auto continuation = rewriter_.split_before(call_insn); if (!continuation) return continuation.error();
    auto preamble = function_.create_block_before(**continuation); if (!preamble) return preamble.error();
    BasicBlock *setup = *preamble;
    std::unordered_map<const Register *, const Register *> registers;
    registers.reserve(std::distance(callee.registers().begin(), callee.registers().end()));
    for (auto &reg : callee.registers()) {
      auto copied = function_.create_register(scalar(reg.type()), {}, reg.hardware_binding()); if (!copied) return copied.error();
      registers.emplace(&reg, *copied);
    }
    std::unordered_map<const BasicBlock *, BasicBlock *> blocks;
    std::unordered_map<const Label *, const Label *> labels;
    blocks.reserve(source_blocks.size()); labels.reserve(std::distance(callee.labels().begin(), callee.labels().end()));
    std::vector<BasicBlock *> cold_chunk; after_return = false;
    for (auto source : source_blocks) {
      auto copied = function_.create_block_before(**continuation); if (!copied) return copied.error();
      blocks.emplace(source, *copied);
      for (auto label : source->labels()) {
        auto value = function_.create_label(**copied); if (!value) return value.error(); labels.emplace(label, *value);
      }
      if (!features.non_top && after_return) { cold_.insert(*copied); cold_chunk.push_back(*copied); }
      after_return |= source == &ret->block();
    }
    std::unordered_map<const Symbol *, const Symbol *> relocated, prototypes;
    auto remap = [&](const Operand &op, bool prototype = false) -> Result<Operand> {
      if (auto r = op.get<const Register *>()) return Operand(*registers.at(*r));
      if (auto l = op.get<const Label *>()) return Operand(*labels.at(*l));
      if (auto m = op.get<Memory>()) return Operand(Memory{m->type, m->base ? registers.at(m->base) : nullptr,
        m->index ? registers.at(m->index) : nullptr, m->displacement, m->scale, m->alias, m->nonalias});
      if (auto s = op.get<SymbolReference>()) {
        auto &cache = prototype ? prototypes : relocated;
        auto found = cache.find(s->symbol);
        if (found == cache.end()) {
          auto value = symbols_.relocate(*s->symbol, function_.module(), prototype); if (!value) return value.error();
          found = cache.emplace(s->symbol, *value).first;
        }
        return Operand(SymbolReference{found->second, s->displacement});
      }
      return op;
    };
    auto add = [&](BasicBlock &block, Opcode opcode, std::initializer_list<Operand> ops) -> Result<void> {
      auto emitted = rewriter_.insert(block, nullptr, opcode, ops); if (!emitted) return emitted.error(); return {};
    };
    frames_.push_back({*continuation, current_});
    std::uint64_t frame_size = 0;
    if (features.top && features.used) {
      std::uint64_t alignment; auto size = aligned(features.size, alignment); if (!size) return size.error(); frame_size = *size;
      if (alignment_ < alignment) {
        alignment_ = alignment;
        if (current_ > std::uint64_t(INT64_MAX) - alignment + 1) return invalid("inline frame alignment overflow");
        current_ = (current_ + alignment - 1) / alignment * alignment;
      }
      if (current_ > std::uint64_t(INT64_MAX) - frame_size) return invalid("inline frame growth overflow");
      current_ += frame_size; maximum_ = std::max(maximum_, current_);
    }
    const Register *top_register = features.top ? registers.at(*features.top->operands()[0].get<const Register *>()) : nullptr;
    if (features.top && features.used) {
      caller_.used = true;
      if (!caller_.top) {
        auto size = temporary(); if (!size) return size.error(); auto &head = *function_.blocks().begin();
        auto anchor = first(head);
        auto initial = rewriter_.insert(head, anchor, Opcode::Mov, {Operand(**size), Operand(0)}); if (!initial) return initial.error();
        auto alloc = rewriter_.insert(head, anchor, Opcode::Alloca, {Operand(*top_register), Operand(**size)}); if (!alloc) return alloc.error();
        caller_.top = *alloc; initial_ = 0;
      }
      auto base = caller_.top->operands()[0];
      if (current_ == frame_size) { auto value = add(*setup, Opcode::Mov, {Operand(*top_register), base}); if (!value) return value.error(); }
      else {
        auto offset = temporary(); if (!offset) return offset.error();
        auto value = add(*setup, Opcode::Mov, {Operand(**offset), Operand(static_cast<std::int64_t>(current_ - frame_size))}); if (!value) return value.error();
        value = add(*setup, Opcode::Add, {Operand(*top_register), base, Operand(**offset)}); if (!value) return value.error();
      }
    }
    const Register *stack = nullptr;
    if (features.non_top) {
      auto token = temporary(); if (!token) return token.error(); stack = *token;
      auto saved = add(*setup, Opcode::BStart, {Operand(*stack)}); if (!saved) return saved.error();
    }
    for (std::size_t a = 0; a < callee.arguments().size(); ++a) {
      auto argument = callee.arguments()[a]; auto destination = registers.at(argument);
      auto source = operands[2 + callee.signature().results.size() + a];
      if (argument->type().is_block()) {
        auto memory = source.get<Memory>(); if (!memory || !memory->base) return invalid("inline block argument requires an address");
        const Register *base = memory->base;
        if (memory->index || memory->displacement) {
          auto adjusted = block_address(*setup, *memory); if (!adjusted) return adjusted.error(); base = *adjusted;
        }
        if (argument->type().kind() == Type::Kind::ReturnBlock) {
          auto copied = add(*setup, Opcode::Mov, {Operand(*destination), Operand(*base)}); if (!copied) return copied.error();
        } else {
          auto copied = block_move(*setup, **continuation, *destination, *base, argument->type().block_size()); if (!copied) return copied.error();
          setup = *copied;
        }
      } else { auto copied = add(*setup, move(scalar(argument->type())), {Operand(*destination), source}); if (!copied) return copied.error(); }
    }
    for (auto source : source_blocks) {
      auto destination = blocks.at(source);
      for (auto &i : source->instructions()) {
        if (&i == features.top) continue;
        if (i.opcode() == Opcode::Ret) {
          for (std::size_t n = 0; n < i.operands().size(); ++n) {
            auto value = remap(i.operands()[n]); if (!value) return value.error();
            auto copied = add(*destination, move(callee.signature().results[n]), {operands[2 + n], *value}); if (!copied) return copied.error();
          }
          if (stack) { auto restored = add(*destination, Opcode::BEnd, {Operand(*stack)}); if (!restored) return restored.error(); }
        } else {
          std::vector<Operand> copied; copied.reserve(i.operands().size());
          for (std::size_t n = 0; n < i.operands().size(); ++n) {
            auto value = remap(i.operands()[n], !n && call(i.opcode())); if (!value) return value.error(); copied.push_back(std::move(*value));
          }
          auto emitted = rewriter_.insert(*destination, nullptr, i.opcode(), std::move(copied)); if (!emitted) return emitted.error();
        }
      }
    }
    // Block copies belong before the callee entry, even when they introduce loops.
    if (setup != *preamble) {
      auto first_body = blocks.at(source_blocks.front());
      for (auto block : argument_blocks_) { auto moved = rewriter_.move_block_before(*block, first_body); if (!moved) return moved.error(); }
      argument_blocks_.clear();
    }
    for (auto block : cold_chunk) { auto moved = rewriter_.move_block_before(*block); if (!moved) return moved.error(); }
    if (!cold_chunk.empty()) cold_chunks_.push_back(std::move(cold_chunk));
    auto erased = rewriter_.erase(call_insn); if (!erased) return erased.error();
    count_ += mir_size(callee);
    return *preamble;
  }
  Result<const Register *> block_address(BasicBlock &block, const Memory &memory) {
    const Register *base = memory.base;
    auto emit = [&](Opcode opcode, std::initializer_list<Operand> operands) -> Result<void> {
      auto result = rewriter_.insert(block, nullptr, opcode, operands); if (!result) return result.error(); return {};
    };
    if (memory.index) {
      const Register *index = memory.index;
      if (memory.scale != 1) {
        auto scale = temporary(), scaled = temporary(); if (!scale) return scale.error(); if (!scaled) return scaled.error();
        auto r = emit(Opcode::Mov, {Operand(**scale), Operand(memory.scale)}); if (!r) return r.error();
        r = emit(Opcode::Mul, {Operand(**scaled), Operand(*index), Operand(**scale)}); if (!r) return r.error(); index = *scaled;
      }
      auto address = temporary(); if (!address) return address.error();
      auto r = emit(Opcode::Add, {Operand(**address), Operand(*base), Operand(*index)}); if (!r) return r.error(); base = *address;
    }
    if (memory.displacement) {
      auto displacement = temporary(), address = temporary(); if (!displacement) return displacement.error(); if (!address) return address.error();
      auto r = emit(Opcode::Mov, {Operand(**displacement), Operand(memory.displacement)}); if (!r) return r.error();
      r = emit(Opcode::Add, {Operand(**address), Operand(*base), Operand(**displacement)}); if (!r) return r.error(); base = *address;
    }
    return base;
  }
  Result<BasicBlock *> block_move(BasicBlock &setup, BasicBlock &continuation, const Register &destination, const Register &source, std::uint64_t size) {
    if (size > std::uint64_t(INT64_MAX) - 7) return invalid("inline block size overflow");
    size = (size + 7) / 8 * 8;
    auto size_reg = temporary(); if (!size_reg) return size_reg.error();
    auto emit = [&](BasicBlock &block, Opcode opcode, std::initializer_list<Operand> operands) -> Result<void> {
      auto result = rewriter_.insert(block, nullptr, opcode, operands); if (!result) return result.error(); return {};
    };
    auto r = emit(setup, Opcode::Mov, {Operand(**size_reg), Operand(static_cast<std::int64_t>(size))}); if (!r) return r.error();
    r = emit(setup, Opcode::Alloca, {Operand(destination), Operand(**size_reg)}); if (!r) return r.error();
    if (!size) return &setup;
    auto address = temporary(), displacement = temporary(), step = temporary(), value = temporary();
    if (!address) return address.error(); if (!displacement) return displacement.error(); if (!step) return step.error(); if (!value) return value.error();
    auto loop = function_.create_block_before(continuation), skip = function_.create_block_before(continuation);
    if (!loop) return loop.error(); if (!skip) return skip.error();
    argument_blocks_.push_back(*loop); argument_blocks_.push_back(*skip);
    auto loop_label = function_.create_label(**loop), skip_label = function_.create_label(**skip);
    if (!loop_label) return loop_label.error(); if (!skip_label) return skip_label.error();
    r = emit(setup, Opcode::Mov, {Operand(**displacement), Operand(0)}); if (!r) return r.error();
    r = emit(setup, Opcode::Ble, {Operand(**skip_label), Operand(**size_reg), Operand(**displacement)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Add, {Operand(**address), Operand(source), Operand(**displacement)}); if (!r) return r.error();
    Memory memory{Type::i64(), *address};
    r = emit(**loop, Opcode::Mov, {Operand(**value), Operand(memory)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Add, {Operand(**address), Operand(destination), Operand(**displacement)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Mov, {Operand(memory), Operand(**value)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Mov, {Operand(**step), Operand(8)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Add, {Operand(**displacement), Operand(**displacement), Operand(**step)}); if (!r) return r.error();
    r = emit(**loop, Opcode::Blt, {Operand(**loop_label), Operand(**displacement), Operand(**size_reg)}); if (!r) return r.error();
    return *skip;
  }
  Function &function_;
  IRRewriter rewriter_;
  Symbols &symbols_;
  Allocation caller_;
  std::uint64_t initial_ = 0, current_ = 0, maximum_ = 0, alignment_ = 0;
  std::size_t changes_ = 0, original_ = 0, count_ = 0;
  std::vector<Frame> frames_;
  std::unordered_set<const BasicBlock *> cold_;
  std::vector<std::vector<BasicBlock *>> cold_chunks_;
  std::vector<BasicBlock *> argument_blocks_;
};
}
InlinePass::InlinePass() = default;
InlinePass::~InlinePass() = default;
Result<void> InlinePass::initialize(Context &context) {
  auto checked = context.verify(); if (!checked) return checked;
  symbols_ = std::make_unique<Symbols>(context); return {};
}
void InlinePass::finalize() noexcept { symbols_.reset(); }
Result<PassResult> InlinePass::run(Function &function, analysis::FunctionAnalysisManager &analyses) {
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  if (&analyses.function() != &function) return invalid("inline analysis owner mismatch");
  bool local = !symbols_;
  if (local) { auto initialized = initialize(function.module().context()); if (!initialized) return initialized.error(); }
  if (&symbols_->context != &function.module().context()) return invalid("inline context owner mismatch");
  auto result = Inliner(function, *symbols_).run();
  if (local) finalize();
  if (!result) return result.error();
  return PassResult{*result, *result ? analysis::PreservedAnalyses::none() : analysis::PreservedAnalyses::all()};
}
}
