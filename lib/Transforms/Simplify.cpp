/* Licensed under the MIT License. Preserve MIR's simplify traversal and rewrite precedence. */
#include "mirnext/Transforms/Simplify.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <unordered_map>

namespace mirnext::transforms {
namespace {
using namespace ir;
Type scalar(Type type) { return type.is_float() ? type : Type::i64(); }
Opcode move(Type type) {
  return type.kind() == Type::Kind::F32 ? Opcode::FMov : type.kind() == Type::Kind::F64 ? Opcode::DMov
    : type.kind() == Type::Kind::LongDouble ? Opcode::LDMov : Opcode::Mov;
}
bool is_move(Opcode op) { return op == Opcode::Mov || op == Opcode::FMov || op == Opcode::DMov || op == Opcode::LDMov; }
bool move_like(Opcode op) {
  return is_move(op) || (sizeof(long double) == sizeof(double) && (op == Opcode::D2LD || op == Opcode::LD2D));
}
bool branch(Opcode op) {
  return op == Opcode::Jmp || (instruction_descriptor(op)->flow == ControlFlow::Branch && op != Opcode::PrBeq && op != Opcode::PrBne);
}
bool integer_branch(Opcode op) {
  auto d = instruction_descriptor(op);
  return branch(op) && op != Opcode::Jmp && d->roles.find_first_of("fde") == std::string_view::npos;
}
std::optional<Opcode> extension(Type type) {
  switch (type.kind()) {
  case Type::Kind::I8: return Opcode::Ext8; case Type::Kind::U8: return Opcode::UExt8;
  case Type::Kind::I16: return Opcode::Ext16; case Type::Kind::U16: return Opcode::UExt16;
  case Type::Kind::I32: return Opcode::Ext32; case Type::Kind::U32: return Opcode::UExt32;
  default: return {};
  }
}
std::optional<Opcode> reverse(Opcode op) {
  switch (op) {
#define PAIR(a,b) case Opcode::a: return Opcode::b; case Opcode::b: return Opcode::a;
  PAIR(Bt,Bf) PAIR(Bts,Bfs) PAIR(Beq,Bne) PAIR(Beqs,Bnes)
  PAIR(Blt,Bge) PAIR(Blts,Bges) PAIR(UBlt,UBge) PAIR(UBlts,UBges)
  PAIR(Ble,Bgt) PAIR(Bles,Bgts) PAIR(UBle,UBgt) PAIR(UBles,UBgts)
  PAIR(Bo,Bno) PAIR(UBo,UBno) PAIR(PrBeq,PrBne)
#undef PAIR
  default: return {};
  }
}
bool identity(Opcode op, std::span<const Operand> operands) {
  if (operands.size() != 3) return false;
  auto rhs = operands[2].get<std::int64_t>(); if (!rhs) return false;
  switch (op) {
  case Opcode::Mul: case Opcode::Muls: case Opcode::Mulo: case Opcode::Mulos: case Opcode::Div: case Opcode::Divs: return *rhs == 1;
  case Opcode::Add: case Opcode::Adds: case Opcode::Sub: case Opcode::Subs: case Opcode::Or: case Opcode::Ors:
  case Opcode::Xor: case Opcode::Xors: case Opcode::Lsh: case Opcode::Lshs: case Opcode::Rsh: case Opcode::Rshs:
  case Opcode::URsh: case Opcode::URshs: return *rhs == 0;
  default: return false;
  }
}
Operand key_operand(const Operand &op) {
  if (auto address = op.get<Memory>()) {
    // Alias annotations do not distinguish MIR simplify's scratch registers.
    Memory memory{address->type, address->base, address->index, address->displacement, address->index ? address->scale : 1};
    return Operand(std::move(memory));
  }
  return op;
}
void hash_combine(std::size_t &hash, std::size_t value) { hash ^= value + 0x9e3779b9U + (hash << 6) + (hash >> 2); }
std::size_t operand_hash(const Operand &op) {
  std::size_t hash = op.storage().index();
  std::visit([&](const auto &value) {
    using T = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<T, Memory>) {
      hash_combine(hash, static_cast<unsigned>(value.type.kind())); hash_combine(hash, value.type.block_size());
      hash_combine(hash, static_cast<unsigned>(value.type.block_class()));
      hash_combine(hash, std::hash<const Register *>{}(value.base)); hash_combine(hash, std::hash<const Register *>{}(value.index));
      hash_combine(hash, std::hash<std::int64_t>{}(value.displacement)); hash_combine(hash, value.scale);
    } else if constexpr (std::is_same_v<T, SymbolReference>) {
      hash_combine(hash, std::hash<const Symbol *>{}(value.symbol)); hash_combine(hash, std::hash<std::int64_t>{}(value.displacement));
    } else hash_combine(hash, std::hash<T>{}(value));
  }, op.storage()); return hash;
}
struct ValueKey {
  Type type; std::optional<Opcode> opcode; Operand lhs; std::optional<Operand> rhs;
  friend bool operator==(const ValueKey &, const ValueKey &) = default;
};
struct ValueHash {
  std::size_t operator()(const ValueKey &key) const {
    std::size_t hash = static_cast<unsigned>(key.type.kind());
    hash_combine(hash, key.opcode ? static_cast<unsigned>(*key.opcode) + 1 : 0); hash_combine(hash, operand_hash(key.lhs));
    if (key.rhs) hash_combine(hash, operand_hash(*key.rhs)); return hash;
  }
};
struct Cursor { BasicBlock *block = nullptr; Instruction *instruction = nullptr; };
class Simplifier {
public:
  explicit Simplifier(Function &function) : function_(function), rewriter_(function), expression_(!function.expression_data_uses().empty()) {}
  Result<std::size_t> run() {
    if (function_.module().error()) return *function_.module().error();
    auto valid = function_.verify(); if (!valid) return valid.error();
    auto materialized = materialize_stack_scopes(function_); if (!materialized) return materialized.error();
    changes_ += materialized->saves + materialized->restores;
    auto &entry = *function_.blocks().begin(); BasicBlock *prologue = &entry;
    for (auto argument : function_.arguments()) if (auto op = extension(argument->type())) {
      if (prologue == &entry && !entry.labels().empty()) {
        auto block = function_.create_block_before(entry); if (!block) return block.error(); prologue = *block;
      }
      auto first = prologue->size() ? const_cast<Instruction *>(&*prologue->instructions().begin()) : nullptr;
      auto emitted = rewriter_.insert(*prologue, first, *op, {Operand(*argument), Operand(*argument)}); if (!emitted) return emitted.error(); ++changes_;
    }
    Cursor cursor{&*function_.blocks().begin(), nullptr}; unsigned chain = 0;
    while (cursor.block) {
      if (!cursor.instruction) { cursor = first_after_labels(cursor.block); continue; }
      auto &instruction = *cursor.instruction; auto opcode = instruction.opcode();
      if (move_like(opcode) && instruction.operands()[0].get<Memory>() && instruction.operands()[1].get<Memory>()) {
        auto memory = instruction.operands()[0].get<Memory>(); auto temp = function_.create_register(scalar(memory->type)); if (!temp) return temp.error();
        auto store = rewriter_.insert_after(instruction, move(memory->type), {instruction.operands()[0], Operand(**temp)}); if (!store) return store.error();
        auto replaced = rewriter_.replace_operand(instruction, 0, Operand(**temp)); if (!replaced) return replaced.error(); ++changes_;
      }
      if (opcode == Opcode::Ret) returns_.push_back(&instruction);
      auto next = next_cursor(instruction);
      if (opcode == Opcode::Alloca) { auto consolidated = allocas(instruction, next); if (!consolidated) return consolidated.error(); }
      auto operands = instruction.operands();
      if ((integer_branch(opcode) || opcode == Opcode::Jmp) && adjacent(next, **operands[0].get<const Label *>())) {
        auto erased = rewriter_.erase(instruction); if (!erased) return erased.error(); ++changes_;
      } else if (identity(opcode, operands) && !live_overflow(instruction)) {
        if (key_operand(operands[0]) == key_operand(operands[1])) {
          auto erased = rewriter_.erase(instruction); if (!erased) return erased.error();
        } else {
          const Operand replacement[]{operands[0], operands[1]}; auto edited = rewriter_.replace(instruction, Opcode::Mov, replacement);
          if (!edited) return edited.error(); next = cursor;
        }
        ++changes_;
      } else if (integer_branch(opcode) && next.instruction && next.instruction->opcode() == Opcode::Jmp
                 && aliases(**operands[0].get<const Label *>(), **next.instruction->operands()[0].get<const Label *>())) {
        auto erased = rewriter_.erase(instruction); if (!erased) return erased.error(); ++changes_;
      } else if ((opcode == Opcode::Bt || opcode == Opcode::Bts || opcode == Opcode::Bf || opcode == Opcode::Bfs)
                 && operands[1].get<std::int64_t>() && (*operands[1].get<std::int64_t>() == 0 || *operands[1].get<std::int64_t>() == 1)) {
        if ((opcode == Opcode::Bt || opcode == Opcode::Bts) == (*operands[1].get<std::int64_t>() == 1)) {
          const Operand replacement[]{operands[0]}; auto edited = rewriter_.replace(instruction, Opcode::Jmp, replacement);
          if (!edited) return edited.error(); next = cursor;
        } else { auto erased = rewriter_.erase(instruction); if (!erased) return erased.error(); }
        ++changes_;
      } else if (auto reversed = reverse(opcode); reversed && next.instruction && next.instruction->opcode() == Opcode::Jmp
                 && adjacent(next_cursor(*next.instruction), **operands[0].get<const Label *>())) {
        std::vector<Operand> replacement(operands.begin(), operands.end()); replacement[0] = next.instruction->operands()[0];
        auto erased = rewriter_.erase(*next.instruction); if (!erased) return erased.error();
        auto edited = rewriter_.replace(instruction, *reversed, replacement); if (!edited) return edited.error(); next = cursor; ++changes_;
      } else if (branch(opcode) && target_instruction(**operands[0].get<const Label *>())
                 && target_instruction(**operands[0].get<const Label *>())->opcode() == Opcode::Jmp && ++chain < 32) {
        auto jump = target_instruction(**operands[0].get<const Label *>());
        auto edited = rewriter_.replace_operand(instruction, 0, jump->operands()[0]); if (!edited) return edited.error();
        ++changes_; continue;
      } else {
        auto simplified = simplify_operands(instruction); if (!simplified) return simplified.error();
      }
      chain = 0; cursor = next;
    }
    auto unified = unify_returns(); if (!unified) return unified.error();
    auto labels = rewriter_.erase_unused_labels(); if (!labels) return labels.error(); changes_ += *labels;
    for (auto block = &*function_.blocks().begin(); block;) {
      auto next = rewriter_.next_block(*block);
      if (next && !block->terminator() && next->labels().empty() && block->stack_scope() == next->stack_scope()) {
        auto merged = rewriter_.merge_fallthrough(*block); if (!merged) return merged.error(); ++changes_;
      } else block = next;
    }
    valid = function_.verify(); if (!valid) return valid.error(); return changes_;
  }
private:
  Function &function_; IRRewriter rewriter_; std::size_t changes_ = 0; bool expression_ = false;
  std::unordered_map<ValueKey, const Register *, ValueHash> values_; std::vector<Instruction *> returns_;
  Cursor next_cursor(const Instruction &instruction) {
    auto block = const_cast<BasicBlock *>(&instruction.block());
    if (auto next = rewriter_.next_instruction(instruction)) return {block, next};
    block = rewriter_.next_block(*block);
    while (block && block->labels().empty() && !block->size()) block = rewriter_.next_block(*block);
    if (!block || !block->labels().empty()) return {block, nullptr};
    return {block, &*block->instructions().begin()};
  }
  Cursor first_after_labels(BasicBlock *block) {
    while (block && !block->size()) block = rewriter_.next_block(*block);
    return {block, block ? &*block->instructions().begin() : nullptr};
  }
  bool adjacent(Cursor cursor, const Label &label) {
    if (cursor.instruction) return false;
    for (auto block = cursor.block; block; block = rewriter_.next_block(*block)) {
      if (block == &label.block()) return true;
      if (block->size()) break;
    }
    return false;
  }
  bool aliases(const Label &a, const Label &b) {
    return &a.block() == &b.block() || adjacent({const_cast<BasicBlock *>(&a.block()), nullptr}, b)
      || adjacent({const_cast<BasicBlock *>(&b.block()), nullptr}, a);
  }
  const Label &last_label(const Label &label) {
    const Label *last = &label; auto block = const_cast<BasicBlock *>(&label.block());
    while (block) {
      if (!block->labels().empty()) last = block->labels().back();
      if (block->size()) break; block = rewriter_.next_block(*block);
    }
    return *last;
  }
  Instruction *target_instruction(const Label &label) { return first_after_labels(const_cast<BasicBlock *>(&label.block())).instruction; }
  bool live_overflow(const Instruction &instruction) {
    switch (instruction.opcode()) {
    case Opcode::Addo: case Opcode::Addos: case Opcode::Subo: case Opcode::Subos:
    case Opcode::Mulo: case Opcode::Mulos: case Opcode::UMulo: case Opcode::UMulos: break;
    default: return false;
    }
    for (auto cursor = next_cursor(instruction); cursor.instruction; cursor = next_cursor(*cursor.instruction)) {
      auto opcode = cursor.instruction->opcode();
      if (opcode == Opcode::Bo || opcode == Opcode::Bno || opcode == Opcode::UBo || opcode == Opcode::UBno) return true;
      if (opcode != Opcode::Mov || !cursor.instruction->operands()[1].get<const Register *>()) break;
    }
    return false;
  }
  Result<const Register *> value_register(Type type, const Operand &lhs, std::optional<Opcode> opcode = {}, const Operand *rhs = nullptr) {
    ValueKey key{scalar(type), opcode, key_operand(lhs), rhs ? std::optional(key_operand(*rhs)) : std::nullopt};
    if (auto found = values_.find(key); found != values_.end()) return found->second;
    auto created = function_.create_register(scalar(type)); if (!created) return created.error(); values_.emplace(std::move(key), *created); return *created;
  }
  Result<void> emit(Instruction &anchor, Opcode opcode, std::initializer_list<Operand> operands, bool after, Instruction *&after_anchor) {
    auto emitted = after ? rewriter_.insert_after(*after_anchor, opcode, operands) : rewriter_.insert_before(anchor, opcode, operands);
    if (!emitted) return emitted.error(); if (after) after_anchor = *emitted; ++changes_; return {};
  }
  Result<void> simplify_operands(Instruction &instruction) {
    auto opcode = instruction.opcode(); const bool moving = move_like(opcode), call = instruction_descriptor(opcode)->count == OperandCount::Call;
    for (std::size_t n = 0; n < instruction.operands().size(); ++n) {
      auto operand = instruction.operands()[n]; const bool output = output_operand(function_, instruction, n);
      auto after_anchor = &instruction;
      if ((call && n == 0) || (call && n == 1 && operand.get<SymbolReference>()) || (opcode == Opcode::VaArg && n == 2)
          || ((opcode == Opcode::PrBeq || opcode == Opcode::PrBne) && n == 2) || (opcode == Opcode::PrSet && n == 1)) continue;
      if (auto label = operand.get<const Label *>()) operand = Operand(last_label(**label));
      else if (operand.get<float>() || operand.get<double>() || operand.get<long double>() || operand.get<std::string>()) {
        const bool string = operand.get<std::string>();
        auto data = function_.module().create_literal(operand); if (!data) return data.error();
        operand = Operand((*data)->symbol());
        if (!string) {
          auto address = value_register(Type::i64(), operand); if (!address) return address.error();
          auto emitted = emit(instruction, Opcode::Mov, {Operand(**address), operand}, false, after_anchor); if (!emitted) return emitted;
          auto type = std::get<InitializedData>((*data)->initializer()).type;
          Memory memory{type, *address};
          if (expression_) { auto tagged = rewriter_.literal_memory(**data, **address); if (!tagged) return tagged.error(); memory = std::move(*tagged); }
          operand = Operand(std::move(memory));
        }
        if (!moving) {
          auto type = string ? Type::i64() : operand.get<Memory>()->type;
          auto reg = value_register(type, operand); if (!reg) return reg.error();
          auto emitted = emit(instruction, move(type), {Operand(**reg), operand}, false, after_anchor); if (!emitted) return emitted;
          operand = Operand(**reg);
        }
      } else if (auto reg = operand.get<const Register *>()) {
        if ((*reg)->hardware_binding().empty()) continue;
        if (moving) {
          auto other = instruction.operands()[1 - n].get<const Register *>();
          if (other && (*other)->hardware_binding().empty()) continue;
        }
        auto temp = value_register((*reg)->type(), operand); if (!temp) return temp.error();
        auto emitted = emit(instruction, move((*reg)->type()), output ? std::initializer_list<Operand>{operand, Operand(**temp)}
          : std::initializer_list<Operand>{Operand(**temp), operand}, output, after_anchor); if (!emitted) return emitted;
        operand = Operand(**temp);
      } else if (auto original = operand.get<Memory>()) {
        auto memory = *original; auto base = memory.base, index = memory.index;
        for (auto address : {&base, &index}) if (*address && !(*address)->hardware_binding().empty()) {
          Operand reg(**address); auto temp = value_register(Type::i64(), reg); if (!temp) return temp.error();
          auto emitted = emit(instruction, Opcode::Mov, {Operand(**temp), reg}, false, after_anchor); if (!emitted) return emitted; *address = *temp;
        }
        memory.base = base; memory.index = index;
        const auto block_argument = memory.type.is_block() && call;
        const bool after = !moving && output && !live_overflow(instruction);
        auto literal = [&](std::int64_t number) -> Result<const Register *> {
          Operand op(number); auto reg = value_register(Type::i64(), op); if (!reg) return reg.error();
          auto emitted = emit(instruction, Opcode::Mov, {Operand(**reg), op}, after, after_anchor); if (!emitted) return emitted.error(); return *reg;
        };
        auto binary = [&](Opcode op, const Register *lhs, const Register *rhs) -> Result<const Register *> {
          Operand l(*lhs), r(*rhs); auto reg = value_register(Type::i64(), l, op, &r); if (!reg) return reg.error();
          auto emitted = emit(instruction, op, {Operand(**reg), l, r}, after, after_anchor); if (!emitted) return emitted.error(); return *reg;
        };
        auto compute_address = [&](const Register *base,const Register *index,unsigned scale,std::int64_t displacement_value) -> Result<const Register *> {
          if (base && !displacement_value && (!index || !scale)) return base;
          if (!base && index && scale == 1 && !displacement_value) return index;
          const Register *displacement = nullptr, *scaled = index;
          if (displacement_value) { auto reg = literal(displacement_value); if (!reg) return reg.error(); displacement = *reg; }
          if (index && scale > 1) {
            auto factor = literal(scale); if (!factor) return factor.error(); auto reg = binary(Opcode::Mul, index, *factor); if (!reg) return reg.error(); scaled = *reg;
          }
          auto sum = base ? base : scaled;
          if (base && scaled) { auto reg = binary(Opcode::Add, base, scaled); if (!reg) return reg.error(); sum = *reg; }
          const Register *address = sum ? sum : displacement;
          if (sum && displacement) { auto reg = binary(Opcode::Add, sum, displacement); if (!reg) return reg.error(); address = *reg; }
          if (!address) { auto reg = literal(0); if (!reg) return reg.error(); address = *reg; }
          return address;
        };
        // A typed BlockArgument may carry a real payload offset (not the
        // serialized block-size field). Canonicalize that offset to a plain
        // base before the ABI lowering checks the call operand.
        if (block_argument && (memory.displacement || memory.index)) {
          auto actual = compute_address(base,index,memory.scale,memory.displacement); if (!actual) return actual.error();
          memory.base = base = *actual; memory.index = index = nullptr; memory.displacement = 0; memory.scale = 0;
        }
        const auto displacement_value = block_argument ? static_cast<std::int64_t>(memory.type.block_size()) : memory.displacement;
        auto computed = compute_address(base,index,memory.scale,displacement_value);
        if (!computed) return computed.error(); const Register *address = *computed;
        if (block_argument) operand = Operand(memory);
        else { memory.base = address; memory.index = nullptr; memory.displacement = 0; memory.scale = 0; operand = Operand(memory); }
        if (moving && (n == 1 || instruction.operands()[1].get<const Register *>())) {}
        else if (memory.type.kind() == Type::Kind::Undef) operand = Operand(*address);
        else if (!memory.type.is_block() || !call) {
          auto type = scalar(memory.type); auto temp = value_register(type, operand); if (!temp) return temp.error();
          auto emitted = emit(instruction, move(type), output ? std::initializer_list<Operand>{operand, Operand(**temp)}
            : std::initializer_list<Operand>{Operand(**temp), operand}, output, after_anchor); if (!emitted) return emitted;
          operand = Operand(**temp);
        }
      } else if (!moving && (operand.get<std::int64_t>() || operand.get<std::uint64_t>() || operand.get<SymbolReference>())) {
        auto reg = value_register(Type::i64(), operand); if (!reg) return reg.error();
        auto emitted = emit(instruction, Opcode::Mov, {Operand(**reg), operand}, false, after_anchor); if (!emitted) return emitted;
        operand = Operand(**reg);
      }
      if (operand != instruction.operands()[n]) {
        auto edited = rewriter_.replace_operand(instruction, n, std::move(operand)); if (!edited) return edited;
      }
    }
    return {};
  }
  Result<void> allocas(Instruction &instruction, Cursor &next) {
    auto size_value = [](const Operand &op) -> std::optional<std::int64_t> {
      if (auto v = op.get<std::int64_t>()) return *v;
      if (auto v = op.get<std::uint64_t>()) return std::bit_cast<std::int64_t>(*v);
      return {};
    };
    auto size = size_value(instruction.operands()[1]); if (!size) return {};
    auto rounded = [](std::int64_t value, std::int64_t &align) -> Result<std::int64_t> {
      value = std::max<std::int64_t>(value, 1); align = value <= 2 ? value : value <= 4 ? 4 : value <= 8 ? 8 : 16;
      if (value > INT64_MAX - align + 1) return Error{ErrorCode::InvalidOperand, "alloca alignment overflows signed size"};
      return (value + align - 1) / align * align;
    };
    std::int64_t align; auto overall = rounded(*size, align); if (!overall) return overall.error();
    while (next.instruction && next.instruction->opcode() == Opcode::Alloca
           && size_value(next.instruction->operands()[1]) && key_operand(instruction.operands()[0]) != key_operand(next.instruction->operands()[0])) {
      std::int64_t next_align; auto amount = rounded(*size_value(next.instruction->operands()[1]), next_align); if (!amount) return amount.error();
      if (next_align > align) {
        if (*overall > INT64_MAX - next_align + 1) return Error{ErrorCode::InvalidOperand, "alloca alignment overflows signed size"};
        *overall = (*overall + next_align - 1) / next_align * next_align; align = next_align;
      }
      if (*overall > INT64_MAX - *amount) return Error{ErrorCode::InvalidOperand, "combined alloca size overflows signed size"};
      auto following = next_cursor(*next.instruction);
      const Operand operands[]{next.instruction->operands()[0], instruction.operands()[0], Operand(*overall)};
      auto edited = rewriter_.replace(*next.instruction, sizeof(void *) == 4 ? Opcode::Adds : Opcode::Add, operands); if (!edited) return edited;
      *overall += *amount; next = following; ++changes_;
    }
    auto normalized = instruction.operands()[1].get<std::uint64_t>() ? Operand(static_cast<std::uint64_t>(*overall)) : Operand(*overall);
    if (normalized != instruction.operands()[1]) {
      auto edited = rewriter_.replace_operand(instruction, 1, std::move(normalized)); if (!edited) return edited; ++changes_;
    }
    next = next_cursor(instruction); return {};
  }
  Result<void> unify_returns() {
    if (returns_.empty()) return {};
    auto &last = *returns_.back(); std::vector<Operand> original(last.operands().begin(), last.operands().end()); const Label *label = nullptr;
    if (returns_.size() > 1) {
      auto block = rewriter_.split_before(last); if (!block) return block.error();
      auto created = function_.create_label(**block); if (!created) return created.error(); label = *created;
    }
    for (std::size_t n = 0; n < original.size(); ++n) if (auto op = extension(function_.signature().results[n])) {
      auto reg = function_.create_register(Type::i64()); if (!reg) return reg.error();
      auto emitted = rewriter_.insert_before(last, *op, {Operand(**reg), original[n]}); if (!emitted) return emitted.error();
      auto edited = rewriter_.replace_operand(last, n, Operand(**reg)); if (!edited) return edited; ++changes_;
    }
    for (auto instruction : returns_) if (instruction != &last) {
      for (std::size_t n = 0; n < original.size(); ++n) {
        auto emitted = rewriter_.insert_before(*instruction, move(function_.signature().results[n]), {original[n], instruction->operands()[n]});
        if (!emitted) return emitted.error();
      }
      const Operand replacement[]{Operand(*label)};
      auto edited = rewriter_.replace(*instruction, Opcode::Jmp, replacement); if (!edited) return edited; ++changes_;
    }
    return {};
  }
};
}
Result<PassResult> SimplifyPass::run(ir::Function &function, analysis::FunctionAnalysisManager &) {
  auto phase = function.require_register_ir(); if (!phase) return phase.error();
  auto result = Simplifier(function).run(); if (!result) return result.error();
  return PassResult{*result, *result ? analysis::PreservedAnalyses::none() : analysis::PreservedAnalyses::all()};
}
} // namespace mirnext::transforms
