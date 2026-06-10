/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mirnext {

namespace {

enum class BinaryOp { Add, Sub, Mul, Div, Mod };
enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

bool is_integer(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::U8:
  case Type::Kind::I16:
  case Type::Kind::U16:
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::I64:
  case Type::Kind::U64:
    return true;
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
  case Type::Kind::P:
    return false;
  }
  return false;
}

bool is_numeric_scalar(Type type) noexcept {
  return is_integer(type) || type == Type::f() || type == Type::d() || type == Type::ld();
}

bool is_integer_literal_type(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::U8:
  case Type::Kind::I16:
  case Type::Kind::U16:
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::P:
    return true;
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
    return false;
  }
  return false;
}

bool is_register_operand(const Value &value) noexcept {
  return value.operand().kind() == Operand::Kind::Register;
}

std::int64_t type_size(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::U8:
    return 1;
  case Type::Kind::I16:
  case Type::Kind::U16:
    return 2;
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::F:
    return 4;
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::D:
  case Type::Kind::P:
    return 8;
  case Type::Kind::LD:
    return 16;
  }
  return 0;
}

std::optional<Opcode> binary_opcode(BinaryOp op, Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
    switch (op) {
    case BinaryOp::Add: return Opcode::Add;
    case BinaryOp::Sub: return Opcode::Sub;
    case BinaryOp::Mul: return Opcode::Mul;
    case BinaryOp::Div: return Opcode::Div;
    case BinaryOp::Mod: return Opcode::Mod;
    }
    break;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
    switch (op) {
    case BinaryOp::Add: return Opcode::Add;
    case BinaryOp::Sub: return Opcode::Sub;
    case BinaryOp::Mul: return Opcode::Mul;
    case BinaryOp::Div: return Opcode::UDiv;
    case BinaryOp::Mod: return Opcode::UMod;
    }
    break;
  case Type::Kind::F:
    switch (op) {
    case BinaryOp::Add: return Opcode::FAdd;
    case BinaryOp::Sub: return Opcode::FSub;
    case BinaryOp::Mul: return Opcode::FMul;
    case BinaryOp::Div: return Opcode::FDiv;
    case BinaryOp::Mod: return std::nullopt;
    }
    break;
  case Type::Kind::D:
    switch (op) {
    case BinaryOp::Add: return Opcode::DAdd;
    case BinaryOp::Sub: return Opcode::DSub;
    case BinaryOp::Mul: return Opcode::DMul;
    case BinaryOp::Div: return Opcode::DDiv;
    case BinaryOp::Mod: return std::nullopt;
    }
    break;
  case Type::Kind::LD:
    switch (op) {
    case BinaryOp::Add: return Opcode::LDAdd;
    case BinaryOp::Sub: return Opcode::LDSub;
    case BinaryOp::Mul: return Opcode::LDMul;
    case BinaryOp::Div: return Opcode::LDDiv;
    case BinaryOp::Mod: return std::nullopt;
    }
    break;
  case Type::Kind::P:
    return std::nullopt;
  }
  return std::nullopt;
}

std::optional<Opcode> branch_opcode(CompareOp op, Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
    switch (op) {
    case CompareOp::Eq: return Opcode::Beq;
    case CompareOp::Ne: return Opcode::Bne;
    case CompareOp::Lt: return Opcode::Blt;
    case CompareOp::Le: return Opcode::Ble;
    case CompareOp::Gt: return Opcode::Bgt;
    case CompareOp::Ge: return Opcode::Bge;
    }
    break;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
    switch (op) {
    case CompareOp::Eq: return Opcode::Beq;
    case CompareOp::Ne: return Opcode::Bne;
    case CompareOp::Lt: return Opcode::UBlt;
    case CompareOp::Le: return Opcode::UBle;
    case CompareOp::Gt: return Opcode::UBgt;
    case CompareOp::Ge: return Opcode::UBge;
    }
    break;
  case Type::Kind::F:
    switch (op) {
    case CompareOp::Eq: return Opcode::FBeq;
    case CompareOp::Ne: return Opcode::FBne;
    case CompareOp::Lt: return Opcode::FBlt;
    case CompareOp::Le: return Opcode::FBle;
    case CompareOp::Gt: return Opcode::FBgt;
    case CompareOp::Ge: return Opcode::FBge;
    }
    break;
  case Type::Kind::D:
    switch (op) {
    case CompareOp::Eq: return Opcode::DBeq;
    case CompareOp::Ne: return Opcode::DBne;
    case CompareOp::Lt: return Opcode::DBlt;
    case CompareOp::Le: return Opcode::DBle;
    case CompareOp::Gt: return Opcode::DBgt;
    case CompareOp::Ge: return Opcode::DBge;
    }
    break;
  case Type::Kind::LD:
    switch (op) {
    case CompareOp::Eq: return Opcode::LDBeq;
    case CompareOp::Ne: return Opcode::LDBne;
    case CompareOp::Lt: return Opcode::LDBlt;
    case CompareOp::Le: return Opcode::LDBle;
    case CompareOp::Gt: return Opcode::LDBgt;
    case CompareOp::Ge: return Opcode::LDBge;
    }
    break;
  case Type::Kind::P:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

Value::Value() noexcept
    : builder_(nullptr), label_id_(0), type_(Type::i64()), operand_(Operand::int64(0)) {}

Value::Value(IRBuilder *builder, std::size_t label_id, Type type, Operand operand) noexcept
    : builder_(builder), label_id_(label_id), type_(type), operand_(std::move(operand)) {}

Type Value::type() const noexcept { return type_; }

Operand Value::operand() const noexcept { return operand_; }

IRBuilder *Value::builder() const noexcept { return builder_; }

bool Value::is_valid() const noexcept { return builder_ != nullptr; }

Var::Var() noexcept : label_(), value_() {}

Var::Var(Label label, Value value) noexcept : label_(label), value_(std::move(value)) {}

Var::operator Value() const noexcept { return value_; }

Type Var::type() const noexcept { return value_.type(); }

Value Var::value() const noexcept { return value_; }

IRBuilder *Var::builder() const noexcept { return value_.builder(); }

bool Var::is_valid() const noexcept { return value_.is_valid(); }

Var &Var::operator=(const Var &rhs) {
  return *this = rhs.value_;
}

Var &Var::operator=(Value rhs) {
  IRBuilder *owner = builder();
  if (owner == nullptr) return *this;
  Label label = owner->current_or(label_);
  owner->assign(label, value_, std::move(rhs));
  return *this;
}

Var &Var::operator=(std::int64_t rhs) {
  IRBuilder *owner = builder();
  if (owner == nullptr) return *this;
  Label label = owner->current_or(label_);
  *this = owner->integer_literal(label, type(), rhs);
  return *this;
}

Memory::Memory() noexcept
    : builder_(nullptr), label_id_(0), type_(Type::i64()), operand_(Operand::int64(0)) {}

Memory::Memory(IRBuilder *builder, std::size_t label_id, Type type, Operand operand) noexcept
    : builder_(builder), label_id_(label_id), type_(type), operand_(std::move(operand)) {}

Type Memory::type() const noexcept { return type_; }

Operand Memory::operand() const noexcept { return operand_; }

IRBuilder *Memory::builder() const noexcept { return builder_; }

bool Memory::is_valid() const noexcept { return builder_ != nullptr; }

Cond::Cond() noexcept
    : builder_(nullptr), label_id_(0), opcode_(Opcode::Beq), lhs_(), rhs_() {}

Cond::Cond(IRBuilder *builder, std::size_t label_id, Opcode opcode, Value lhs, Value rhs) noexcept
    : builder_(builder), label_id_(label_id), opcode_(opcode), lhs_(std::move(lhs)),
      rhs_(std::move(rhs)) {}

IRBuilder *Cond::builder() const noexcept { return builder_; }

bool Cond::is_valid() const noexcept { return builder_ != nullptr; }

Value Label::arg(std::string_view name) {
  if (builder_ == nullptr) return Value();
  return builder_->arg(*this, name);
}

void Label::begin() {
  if (builder_ == nullptr) return;
  builder_->begin(*this);
}

void Label::end() {
  if (builder_ == nullptr) return;
  builder_->end(*this);
}

Var Label::local(Type type, std::string_view name) {
  if (builder_ == nullptr) return Var();
  return builder_->local(*this, type, name);
}

Value Label::i8(std::int8_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->i8(*this, value);
}

Value Label::u8(std::uint8_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->u8(*this, value);
}

Value Label::i16(std::int16_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->i16(*this, value);
}

Value Label::u16(std::uint16_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->u16(*this, value);
}

Value Label::i32(std::int32_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->i32(*this, value);
}

Value Label::u32(std::uint32_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->u32(*this, value);
}

Value Label::i64(std::int64_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->i64(*this, value);
}

Value Label::u64(std::uint64_t value) {
  if (builder_ == nullptr) return Value();
  return builder_->u64(*this, value);
}

Value Label::f32(float value) {
  if (builder_ == nullptr) return Value();
  return builder_->f32(*this, value);
}

Value Label::f64(double value) {
  if (builder_ == nullptr) return Value();
  return builder_->f64(*this, value);
}

Value Label::ld(long double value) {
  if (builder_ == nullptr) return Value();
  return builder_->ld(*this, value);
}

Value Label::alloca(std::int64_t size, std::string_view name) {
  if (builder_ == nullptr) return Value();
  return builder_->alloca(*this, size, name);
}

Value Label::alloca(Type element_type, std::string_view name) {
  if (builder_ == nullptr) return Value();
  return builder_->alloca(*this, element_type, name);
}

Value Label::alloca(Type element_type, std::int64_t count, std::string_view name) {
  if (builder_ == nullptr) return Value();
  return builder_->alloca(*this, element_type, count, name);
}

Memory Label::mem(Type type, Value base) {
  if (builder_ == nullptr) return Memory();
  return builder_->mem(*this, type, std::move(base));
}

Memory Label::mem(Type type, Value base, Value index, int scale, std::int64_t displacement) {
  if (builder_ == nullptr) return Memory();
  return builder_->mem(*this, type, std::move(base), std::move(index), scale, displacement);
}

Value Label::load(Memory memory) {
  if (builder_ == nullptr) return Value();
  return builder_->load(*this, std::move(memory));
}

void Label::store(Memory memory, Value value) {
  if (builder_ == nullptr) return;
  builder_->store(*this, std::move(memory), std::move(value));
}

void Label::assign(Value dst, Value src) {
  if (builder_ == nullptr) return;
  builder_->assign(*this, std::move(dst), std::move(src));
}

void Label::jmp(Label target) {
  if (builder_ == nullptr) return;
  builder_->jmp(*this, target);
}

void Label::if_(Cond cond, Label target) {
  if (builder_ == nullptr) return;
  builder_->if_(*this, std::move(cond), target);
}

void Label::ret(Value value) {
  if (builder_ == nullptr) return;
  builder_->ret(*this, std::move(value));
}

void Label::ret(std::vector<Value> values) {
  if (builder_ == nullptr) return;
  builder_->ret(*this, std::move(values));
}

void Label::ret() {
  if (builder_ == nullptr) return;
  builder_->ret(*this);
}

Value Label::call(const Prototype &prototype, const Import &callee, std::vector<Value> args) {
  if (builder_ == nullptr) return Value();
  return builder_->call(*this, prototype, callee, std::move(args));
}

Value Label::call(const Prototype &prototype, const Function &callee, std::vector<Value> args) {
  if (builder_ == nullptr) return Value();
  return builder_->call(*this, prototype, callee, std::move(args));
}

CallResult Label::call_multi(const Prototype &prototype, const Import &callee,
                             std::vector<Value> args) {
  if (builder_ == nullptr) return {};
  return builder_->call_multi(*this, prototype, callee, std::move(args));
}

CallResult Label::call_multi(const Prototype &prototype, const Function &callee,
                             std::vector<Value> args) {
  if (builder_ == nullptr) return {};
  return builder_->call_multi(*this, prototype, callee, std::move(args));
}

void Label::call_void(const Prototype &prototype, const Import &callee, std::vector<Value> args) {
  if (builder_ == nullptr) return;
  builder_->call_void(*this, prototype, callee, std::move(args));
}

void Label::call_void(const Prototype &prototype, const Function &callee, std::vector<Value> args) {
  if (builder_ == nullptr) return;
  builder_->call_void(*this, prototype, callee, std::move(args));
}

IRBuilder::IRBuilder(Function &function) noexcept : function_(&function) {}

Label IRBuilder::entry() {
  if (!ok()) return Label();
  if (entry_label_id_) {
    fail(Error{ErrorCode::InvalidOperand, "entry block already exists"});
    return Label();
  }
  Label label = create_attached_label(true);
  entry_label_id_ = label.id();
  return label;
}

Label IRBuilder::label() {
  if (!ok()) return Label();
  return create_attached_label(false);
}

bool IRBuilder::ok() const noexcept { return !error_.has_value(); }

const Error &IRBuilder::error() const noexcept {
  if (!error_) std::abort();
  return *error_;
}

Value IRBuilder::arg(Label label, std::string_view name) {
  if (!ok()) return invalid_value();
  if (!validate_label(label)) return invalid_value();

  Result<Register> reg = function_->argument(name);
  if (!reg) {
    fail(std::move(reg).error());
    return invalid_value();
  }
  return Value(this, label.id(), reg->type(), Operand(*reg));
}

Var IRBuilder::local(Label label, Type type, std::string_view name) {
  if (!ok()) return Var();
  if (!validate_label(label)) return Var();

  Result<Register> reg = function_->create_register(type, name);
  if (!reg) {
    fail(std::move(reg).error());
    return Var();
  }
  return Var(label, Value(this, label.id(), type, Operand(*reg)));
}

Value IRBuilder::i8(Label label, std::int8_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::i8(), Operand::int64(value));
}

Value IRBuilder::u8(Label label, std::uint8_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::u8(), Operand::int64(value));
}

Value IRBuilder::i16(Label label, std::int16_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::i16(), Operand::int64(value));
}

Value IRBuilder::u16(Label label, std::uint16_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::u16(), Operand::int64(value));
}

Value IRBuilder::i32(Label label, std::int32_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::i32(), Operand::int64(value));
}

Value IRBuilder::u32(Label label, std::uint32_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::u32(), Operand::int64(value));
}

Value IRBuilder::i64(Label label, std::int64_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::i64(), Operand::int64(value));
}

Value IRBuilder::u64(Label label, std::uint64_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::u64(), Operand::uint64(value));
}

Value IRBuilder::f32(Label label, float value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::f(), Operand::float32(value));
}

Value IRBuilder::f64(Label label, double value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::d(), Operand::float64(value));
}

Value IRBuilder::ld(Label label, long double value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  return Value(this, label.id(), Type::ld(), Operand::long_double(value));
}

Value IRBuilder::alloca(Label label, std::int64_t size, std::string_view name) {
  if (!ok()) return invalid_value();
  if (size < 0) {
    fail(Error{ErrorCode::InvalidArgument, "alloca size must be non-negative"});
    return invalid_value();
  }
  if (!ensure_block(label)) return invalid_value();

  Result<Register> reg = function_->create_register(Type::p(), name);
  if (!reg) {
    fail(std::move(reg).error());
    return invalid_value();
  }
  function_->append(Instruction(Opcode::Alloca, {Operand(*reg), Operand::int64(size)}));
  return Value(this, label.id(), Type::p(), Operand(*reg));
}

Value IRBuilder::alloca(Label label, Type element_type, std::string_view name) {
  return alloca(label, element_type, 1, name);
}

Value IRBuilder::alloca(Label label, Type element_type, std::int64_t count, std::string_view name) {
  if (!ok()) return invalid_value();
  if (count < 0) {
    fail(Error{ErrorCode::InvalidArgument, "alloca element count must be non-negative"});
    return invalid_value();
  }

  const std::int64_t element_size = type_size(element_type);
  if (count > std::numeric_limits<std::int64_t>::max() / element_size) {
    fail(Error{ErrorCode::InvalidArgument, "alloca size overflow"});
    return invalid_value();
  }

  return alloca(label, element_size * count, name);
}

Memory IRBuilder::mem(Label label, Type type, Value base) {
  if (!ok()) return invalid_memory();
  if (!validate_register_value(label, base, "memory base")) return invalid_memory();
  return Memory(this, label.id(), type, memory_operand(type, base, 0, 0, 1));
}

Memory IRBuilder::mem(Label label, Type type, Value base, Value index, int scale,
                      std::int64_t displacement) {
  if (!ok()) return invalid_memory();
  if (scale != 1 && scale != 2 && scale != 4 && scale != 8) {
    fail(Error{ErrorCode::InvalidArgument, "memory scale must be 1, 2, 4, or 8"});
    return invalid_memory();
  }
  if (!validate_register_value(label, base, "memory base")
      || !validate_register_value(label, index, "memory index")) {
    return invalid_memory();
  }
  return Memory(this, label.id(), type,
                memory_operand(type, base, index.operand_.register_id(), displacement, scale));
}

Value IRBuilder::load(Label label, Memory memory) {
  if (!ok()) return invalid_value();
  if (!validate_memory(label, memory)) return invalid_value();
  const std::optional<Opcode> opcode = typed_move_opcode(memory.type_);
  if (!opcode) return invalid_value();
  if (!ensure_block(label)) return invalid_value();

  Register dst = temp(memory.type_);
  if (!ok()) return invalid_value();
  function_->append(Instruction(*opcode, {Operand(dst), memory.operand_}));
  return Value(this, label.id(), memory.type_, Operand(dst));
}

void IRBuilder::store(Label label, Memory memory, Value value) {
  if (!ok()) return;
  if (!validate_memory(label, memory) || !validate_value(label, value, "store value")) return;
  if (memory.type_ != value.type_) {
    fail(Error{ErrorCode::InvalidOperand, "store value type does not match memory type"});
    return;
  }
  const std::optional<Opcode> opcode = typed_move_opcode(memory.type_);
  if (!opcode) return;
  if (!ensure_block(label)) return;

  function_->append(Instruction(*opcode, {memory.operand_, value.operand_}));
}

void IRBuilder::assign(Label label, Value dst, Value src) {
  if (!ok()) return;
  if (!validate_register_value(label, dst, "assign destination")
      || !validate_value(label, src, "assign source")) {
    return;
  }
  if (dst.type_ != src.type_) {
    fail(Error{ErrorCode::InvalidOperand, "assign source type does not match destination type"});
    return;
  }
  const std::optional<Opcode> opcode = typed_move_opcode(dst.type_);
  if (!opcode) return;
  if (!ensure_block(label)) return;

  function_->append(Instruction(*opcode, {dst.operand_, src.operand_}));
}

void IRBuilder::jmp(Label label, Label target) {
  append_branch(label, Opcode::Jmp, target);
}

void IRBuilder::if_(Label label, Cond cond, Label target) {
  if (!ok()) return;
  if (!validate_label(label)) return;
  const bool label_matches = cond.label_id_ == label.id();
  const bool register_backed = is_register_operand(cond.lhs_) && is_register_operand(cond.rhs_);
  if (!cond.is_valid() || cond.builder_ != this || (!label_matches && !register_backed)
      || !validate_binary(cond.lhs_, cond.rhs_)) {
    fail(Error{ErrorCode::InvalidOperand, "condition does not belong to this label"});
    return;
  }
  append_branch(label, cond.opcode_, target, cond.lhs_.operand_, cond.rhs_.operand_);
}

void IRBuilder::ret(Label label, Value value) {
  if (!ok()) return;
  if (!value.is_valid() || value.builder_ != this) {
    fail(Error{ErrorCode::InvalidOperand, "return value does not belong to this builder"});
    return;
  }
  if (!ensure_block(label)) return;
  function_->append_ret({value.operand_});
}

void IRBuilder::ret(Label label, std::vector<Value> values) {
  if (!ok()) return;

  std::vector<Operand> operands;
  operands.reserve(values.size());
  for (const Value &value : values) {
    if (!value.is_valid() || value.builder_ != this) {
      fail(Error{ErrorCode::InvalidOperand, "return value does not belong to this builder"});
      return;
    }
    operands.push_back(value.operand_);
  }
  if (!ensure_block(label)) return;
  function_->append_ret(std::move(operands));
}

void IRBuilder::ret(Label label) {
  if (!ok()) return;
  if (!ensure_block(label)) return;
  function_->append_ret();
}

void IRBuilder::begin(Label label) {
  if (!ok()) return;
  (void) ensure_block(label);
}

void IRBuilder::end(Label label) {
  if (!ok()) return;
  if (!validate_label(label)) return;
  if (!current_label_id_ || *current_label_id_ != label.id()) {
    fail(Error{ErrorCode::InvalidOperand, "label is not the current insert block"});
    return;
  }
  BlockState *block = find_block(label.id());
  if (block == nullptr) {
    fail(Error{ErrorCode::InvalidOperand, "unknown builder label"});
    return;
  }
  block->closed = true;
  current_label_id_.reset();
}

Value IRBuilder::call(Label label, const Prototype &prototype, const Import &callee,
                      std::vector<Value> args) {
  CallResult results = append_call(label, prototype, Operand::ref(callee), std::move(args), 1);
  if (results.size() != 1) return invalid_value();
  return results[0];
}

Value IRBuilder::call(Label label, const Prototype &prototype, const Function &callee,
                      std::vector<Value> args) {
  CallResult results = append_call(label, prototype, Operand::ref(callee), std::move(args), 1);
  if (results.size() != 1) return invalid_value();
  return results[0];
}

CallResult IRBuilder::call_multi(Label label, const Prototype &prototype, const Import &callee,
                                 std::vector<Value> args) {
  return append_call(label, prototype, Operand::ref(callee), std::move(args), std::nullopt);
}

CallResult IRBuilder::call_multi(Label label, const Prototype &prototype, const Function &callee,
                                 std::vector<Value> args) {
  return append_call(label, prototype, Operand::ref(callee), std::move(args), std::nullopt);
}

void IRBuilder::call_void(Label label, const Prototype &prototype, const Import &callee,
                          std::vector<Value> args) {
  (void) append_call(label, prototype, Operand::ref(callee), std::move(args), 0);
}

void IRBuilder::call_void(Label label, const Prototype &prototype, const Function &callee,
                          std::vector<Value> args) {
  (void) append_call(label, prototype, Operand::ref(callee), std::move(args), 0);
}

void IRBuilder::fail(Error error) {
  if (!error_) error_ = std::move(error);
}

Label IRBuilder::current_or(Label fallback) noexcept {
  if (!current_label_id_) return fallback;
  return Label(function_, *current_label_id_, this, *current_label_id_ == entry_label_id_.value_or(0));
}

Value IRBuilder::integer_literal(Label label, Type type, std::int64_t value) {
  if (!ok() || !validate_label(label)) return invalid_value();
  if (!is_integer_literal_type(type)) {
    fail(Error{ErrorCode::InvalidOperand, "integer assignment literal requires integer-like type"});
    return invalid_value();
  }
  if (type == Type::u64()) return Value(this, label.id(), type, Operand::uint64(static_cast<std::uint64_t>(value)));
  return Value(this, label.id(), type, Operand::int64(value));
}

Value IRBuilder::invalid_value() noexcept {
  return Value();
}

Memory IRBuilder::invalid_memory() noexcept {
  return Memory();
}

Cond IRBuilder::invalid_cond() noexcept {
  return Cond();
}

Register IRBuilder::temp(Type type) {
  while (true) {
    const std::string name = ".t" + std::to_string(next_temp_id_++);
    Result<Register> reg = function_->create_register(type, name);
    if (reg) return reg.value();
    if (reg.error().code != ErrorCode::DuplicateName) {
      fail(std::move(reg).error());
      return Register();
    }
  }
}

std::optional<Opcode> IRBuilder::typed_move_opcode(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::U8:
  case Type::Kind::I16:
  case Type::Kind::U16:
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::P:
    return Opcode::Mov;
  case Type::Kind::F:
    return Opcode::FMov;
  case Type::Kind::D:
    return Opcode::DMov;
  case Type::Kind::LD:
    return Opcode::LDMov;
  }
  return std::nullopt;
}

Operand IRBuilder::memory_operand(Type type, const Value &base, std::size_t index_register,
                                  std::int64_t displacement, int scale) {
  return Operand(type, base.operand_.register_id(), index_register, displacement, scale);
}

bool IRBuilder::validate_value(Label label, const Value &value, std::string_view description) {
  if (!validate_label(label)) return false;
  if (!value.is_valid()) {
    fail(Error{ErrorCode::InvalidOperand, std::string(description) + " is invalid"});
    return false;
  }
  if (value.builder_ != this) {
    fail(Error{ErrorCode::InvalidOperand, std::string(description)
                                         + " does not belong to this builder"});
    return false;
  }
  if (value.label_id_ != label.id() && !is_register_operand(value)) {
    fail(Error{ErrorCode::InvalidOperand, std::string(description)
                                         + " does not belong to this label"});
    return false;
  }
  return true;
}

bool IRBuilder::validate_register_value(Label label, const Value &value,
                                        std::string_view description) {
  if (!validate_value(label, value, description)) return false;
  if (!is_register_operand(value)) {
    fail(Error{ErrorCode::InvalidOperand, std::string(description)
                                         + " must be a register value"});
    return false;
  }
  return true;
}

bool IRBuilder::validate_memory(Label label, const Memory &memory) {
  if (!validate_label(label)) return false;
  if (!memory.is_valid()) {
    fail(Error{ErrorCode::InvalidOperand, "memory operand is invalid"});
    return false;
  }
  if (memory.builder_ != this) {
    fail(Error{ErrorCode::InvalidOperand, "memory operand does not belong to this builder"});
    return false;
  }
  if (memory.label_id_ != label.id()) {
    fail(Error{ErrorCode::InvalidOperand, "memory operand does not belong to this label"});
    return false;
  }
  if (memory.operand_.kind() != Operand::Kind::Memory) {
    fail(Error{ErrorCode::InvalidOperand, "operand is not memory"});
    return false;
  }
  return true;
}

std::optional<std::size_t> IRBuilder::resolve_binary_label(Value lhs, Value rhs) {
  const bool lhs_register = is_register_operand(lhs);
  const bool rhs_register = is_register_operand(rhs);
  if (!lhs_register && !rhs_register) {
    if (lhs.label_id_ != rhs.label_id_) {
      fail(Error{ErrorCode::InvalidOperand, "literal operands must belong to the same label"});
      return std::nullopt;
    }
    return lhs.label_id_;
  }
  if (!lhs_register && rhs_register) return lhs.label_id_;
  if (lhs_register && !rhs_register) return rhs.label_id_;
  if (current_label_id_) return *current_label_id_;
  if (!first_block_started_) return lhs.label_id_;
  fail(Error{ErrorCode::InvalidOperand, "cannot infer current label for register operands"});
  return std::nullopt;
}

CallResult IRBuilder::append_call(Label label, const Prototype &prototype, Operand callee,
                                  std::vector<Value> args,
                                  std::optional<std::size_t> return_count) {
  if (!ok()) return {};
  if (return_count && prototype.return_types().size() != *return_count) {
    fail(Error{ErrorCode::InvalidOperand, "call return count does not match requested shape"});
    return {};
  }
  if (!validate_call_args(label, prototype, args)) return {};
  if (!ensure_block(label)) return {};

  std::vector<Value> results;
  results.reserve(prototype.return_types().size());
  std::vector<Operand> operands;
  operands.reserve(2 + prototype.return_types().size() + args.size());
  operands.push_back(Operand::ref(prototype));
  operands.push_back(std::move(callee));

  for (Type type : prototype.return_types()) {
    Register reg = temp(type);
    if (!ok()) return {};
    operands.push_back(Operand(reg));
    results.push_back(Value(this, label.id(), type, Operand(reg)));
  }
  for (const Value &arg : args) operands.push_back(arg.operand_);

  function_->append(Instruction(Opcode::Call, std::move(operands)));
  return CallResult(std::move(results));
}

Value IRBuilder::append_binary(Opcode opcode, Value lhs, Value rhs) {
  if (!ok()) return invalid_value();
  if (!validate_binary(lhs, rhs)) return invalid_value();

  std::optional<std::size_t> label_id = resolve_binary_label(lhs, rhs);
  if (!label_id) return invalid_value();
  Label label(function_, *label_id, this, *label_id == entry_label_id_.value_or(0));
  if (!ensure_block(label)) return invalid_value();

  Register dst = temp(lhs.type_);
  if (!ok()) return invalid_value();

  function_->append(Instruction(opcode, {Operand(dst), lhs.operand_, rhs.operand_}));
  return Value(this, *label_id, lhs.type_, Operand(dst));
}

Cond IRBuilder::compare(Opcode opcode, Value lhs, Value rhs) {
  if (!ok()) return invalid_cond();
  if (!validate_binary(lhs, rhs)) return invalid_cond();
  std::optional<std::size_t> label_id = resolve_binary_label(lhs, rhs);
  if (!label_id) return invalid_cond();
  return Cond(this, *label_id, opcode, std::move(lhs), std::move(rhs));
}

bool IRBuilder::validate_call_args(Label label, const Prototype &prototype,
                                   const std::vector<Value> &args) {
  if (!validate_label(label)) return false;
  if (args.size() != prototype.parameters().size()) {
    fail(Error{ErrorCode::InvalidOperand, "call argument count does not match prototype"});
    return false;
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    const Value &arg = args[i];
    if (!arg.is_valid()) {
      fail(Error{ErrorCode::InvalidOperand, "invalid call argument"});
      return false;
    }
    if (arg.builder_ != this) {
      fail(Error{ErrorCode::InvalidOperand, "call argument does not belong to this builder"});
      return false;
    }
    if (arg.label_id_ != label.id()) {
      fail(Error{ErrorCode::InvalidOperand, "call argument does not belong to this label"});
      return false;
    }
    if (arg.type_ != prototype.parameters()[i].type) {
      fail(Error{ErrorCode::InvalidOperand, "call argument type does not match prototype"});
      return false;
    }
  }
  return true;
}

bool IRBuilder::validate_binary(Value lhs, Value rhs, bool require_integer) {
  if (!lhs.is_valid() || !rhs.is_valid()) {
    fail(Error{ErrorCode::InvalidOperand, "invalid operand"});
    return false;
  }
  if (lhs.builder_ != this || rhs.builder_ != this) {
    fail(Error{ErrorCode::InvalidOperand, "operands must belong to the same builder"});
    return false;
  }
  if (lhs.label_id_ != rhs.label_id_ && !is_register_operand(lhs) && !is_register_operand(rhs)) {
    fail(Error{ErrorCode::InvalidOperand,
               "cross-label operands must be register-backed values"});
    return false;
  }
  if (lhs.type_ != rhs.type_) {
    fail(Error{ErrorCode::InvalidOperand, "operand types do not match"});
    return false;
  }
  if (!is_numeric_scalar(lhs.type_)) {
    fail(Error{ErrorCode::InvalidOperand, "operands must be numeric scalar values"});
    return false;
  }
  if (require_integer && !is_integer(lhs.type_)) {
    fail(Error{ErrorCode::InvalidOperand, "operator requires integer operands"});
    return false;
  }
  return true;
}

bool IRBuilder::validate_label(Label label) {
  if (!label.is_valid() || label.builder_ != this || label.function_ != function_) {
    fail(Error{ErrorCode::InvalidOperand, "label does not belong to this builder"});
    return false;
  }
  if (find_block(label.id()) == nullptr) {
    fail(Error{ErrorCode::InvalidOperand, "unknown builder label"});
    return false;
  }
  return true;
}

bool IRBuilder::validate_target(Label target) {
  if (!target.is_valid() || target.builder_ != this || target.function_ != function_) {
    fail(Error{ErrorCode::InvalidOperand, "target label does not belong to this builder"});
    return false;
  }
  if (find_block(target.id()) == nullptr) {
    fail(Error{ErrorCode::InvalidOperand, "unknown target label"});
    return false;
  }
  return true;
}

IRBuilder::BlockState *IRBuilder::find_block(std::size_t label_id) noexcept {
  for (BlockState &block : blocks_) {
    if (block.label_id == label_id) return &block;
  }
  return nullptr;
}

bool IRBuilder::ensure_block(Label label) {
  if (!validate_label(label)) return false;

  BlockState *block = find_block(label.id());
  if (block == nullptr) {
    fail(Error{ErrorCode::InvalidOperand, "unknown builder label"});
    return false;
  }
  if (block->closed) {
    fail(Error{ErrorCode::InvalidOperand, "cannot append to a closed label"});
    return false;
  }
  if (!first_block_started_ && !block->entry) {
    fail(Error{ErrorCode::InvalidOperand, "entry label must be emitted first"});
    return false;
  }
  if (current_label_id_ && *current_label_id_ != block->label_id) {
    BlockState *current = find_block(*current_label_id_);
    if (current != nullptr) current->closed = true;
  }
  if (!block->materialized) {
    function_->append_label(label);
    block->materialized = true;
    first_block_started_ = true;
  }
  current_label_id_ = block->label_id;
  return true;
}

Label IRBuilder::create_attached_label(bool entry) {
  Label base = function_->create_label();
  blocks_.push_back(BlockState{base.id(), entry});
  return Label(function_, base.id(), this, entry);
}

void IRBuilder::append_branch(Label label, Opcode opcode, Label target) {
  if (!ok()) return;
  if (!validate_label(label) || !validate_target(target)) return;
  if (!ensure_block(label)) return;
  function_->append(Instruction(opcode, {Operand(target)}));
}

void IRBuilder::append_branch(Label label, Opcode opcode, Label target, Operand lhs, Operand rhs) {
  if (!ok()) return;
  if (!validate_label(label) || !validate_target(target)) return;
  if (!ensure_block(label)) return;
  function_->append(Instruction(opcode, {Operand(target), std::move(lhs), std::move(rhs)}));
}

Value operator+(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Value();
  }
  const std::optional<Opcode> opcode = binary_opcode(BinaryOp::Add, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for addition"});
    return Value();
  }
  return lhs.builder_->append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Value operator-(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Value();
  }
  const std::optional<Opcode> opcode = binary_opcode(BinaryOp::Sub, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for subtraction"});
    return Value();
  }
  return lhs.builder_->append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Value operator*(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Value();
  }
  const std::optional<Opcode> opcode = binary_opcode(BinaryOp::Mul, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for multiplication"});
    return Value();
  }
  return lhs.builder_->append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Value operator/(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Value();
  }
  const std::optional<Opcode> opcode = binary_opcode(BinaryOp::Div, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for division"});
    return Value();
  }
  return lhs.builder_->append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Value operator%(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Value();
  }
  if (!lhs.builder_->validate_binary(lhs, rhs, true)) return Value();
  const std::optional<Opcode> opcode = binary_opcode(BinaryOp::Mod, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for modulo"});
    return Value();
  }
  return lhs.builder_->append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator==(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Eq, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator!=(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Ne, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator<(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Lt, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator<=(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Le, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator>(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Gt, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

Cond operator>=(Value lhs, Value rhs) {
  if (lhs.builder_ == nullptr) {
    if (rhs.builder_ != nullptr) rhs.builder_->fail(Error{ErrorCode::InvalidOperand, "invalid lhs"});
    return Cond();
  }
  const std::optional<Opcode> opcode = branch_opcode(CompareOp::Ge, lhs.type_);
  if (!opcode) {
    lhs.builder_->fail(Error{ErrorCode::InvalidOperand, "unsupported operand type for comparison"});
    return Cond();
  }
  return lhs.builder_->compare(*opcode, std::move(lhs), std::move(rhs));
}

} // namespace mirnext
