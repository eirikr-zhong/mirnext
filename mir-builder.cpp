/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mirnext {

namespace {

enum class UnaryOp { Neg };
enum class BinaryOp { Add, Sub, Mul, Div, Mod };
enum class IntegerBinaryOp { And = 5, Or, Xor, Lsh, Rsh };
enum class CompareOp { Eq, Ne, Lt, Le, Gt, Ge };

bool is_integer(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return false;
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

bool is_signed_integer(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return false;
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
    return true;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
  case Type::Kind::P:
    return false;
  }
  return false;
}

bool is_unsigned_integer(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return false;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
    return true;
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
  case Type::Kind::P:
    return false;
  }
  return false;
}

bool is_float(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
    return true;
  case Type::Kind::B:
  case Type::Kind::I8:
  case Type::Kind::U8:
  case Type::Kind::I16:
  case Type::Kind::U16:
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::P:
    return false;
  }
  return false;
}

bool is_numeric_scalar(Type type) noexcept {
  return is_integer(type) || is_float(type);
}

bool is_integer_literal_type(Type type) noexcept {
  return is_integer(type) || type == Type::p();
}

bool is_switch_index_type(Type type) noexcept {
  return is_integer(type);
}

bool is_typed_integer_data_type(Type type) noexcept {
  return is_integer(type) || type == Type::p();
}

bool is_memory_type(Type type) noexcept {
  return is_numeric_scalar(type) || type == Type::p();
}

bool is_signed_data_type(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return false;
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
    return true;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
  case Type::Kind::P:
    return false;
  }
  return false;
}

std::int64_t signed_min(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8: return std::numeric_limits<std::int8_t>::min();
  case Type::Kind::I16: return std::numeric_limits<std::int16_t>::min();
  case Type::Kind::I32: return std::numeric_limits<std::int32_t>::min();
  case Type::Kind::I64: return std::numeric_limits<std::int64_t>::min();
  default: return 0;
  }
}

std::int64_t signed_max(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8: return std::numeric_limits<std::int8_t>::max();
  case Type::Kind::I16: return std::numeric_limits<std::int16_t>::max();
  case Type::Kind::I32: return std::numeric_limits<std::int32_t>::max();
  case Type::Kind::I64: return std::numeric_limits<std::int64_t>::max();
  default: return 0;
  }
}

std::uint64_t unsigned_max(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::U8: return std::numeric_limits<std::uint8_t>::max();
  case Type::Kind::U16: return std::numeric_limits<std::uint16_t>::max();
  case Type::Kind::U32: return std::numeric_limits<std::uint32_t>::max();
  case Type::Kind::U64: return std::numeric_limits<std::uint64_t>::max();
  case Type::Kind::P: return std::numeric_limits<std::uint64_t>::max();
  default: return 0;
  }
}

bool is_register_operand(const Value &value) noexcept {
  if (value.is_poison()) return false;
  return value.operand().kind() == Operand::Kind::Register;
}

bool is_reference_operand(const Value &value) noexcept {
  if (value.is_poison()) return false;
  return value.operand().kind() == Operand::Kind::Reference;
}

bool is_module_slot_operand(const Value &value) noexcept {
  if (value.is_poison()) return false;
  return value.operand().kind() == Operand::Kind::ModuleSlot;
}

bool module_contains_reference(const Module &module, const Operand &operand) noexcept {
  const void *pointer = operand.reference_pointer();
  if (pointer == nullptr) return false;
  switch (operand.reference_kind()) {
  case Operand::ReferenceKind::Prototype:
    for (const auto &prototype : module.prototypes()) {
      if (prototype.get() == pointer) return true;
    }
    return false;
  case Operand::ReferenceKind::Import:
    for (const auto &import : module.imports()) {
      if (import.get() == pointer) return true;
    }
    return false;
  case Operand::ReferenceKind::Function:
    for (const auto &function : module.functions()) {
      if (function.get() == pointer) return true;
    }
    return false;
  case Operand::ReferenceKind::Data:
    for (const auto &data : module.data_items()) {
      if (data.get() == pointer) return true;
    }
    return false;
  case Operand::ReferenceKind::None:
    return false;
  }
  return false;
}

std::int64_t type_size(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return 8;
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

std::optional<Opcode> typed_move_opcode(Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
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

std::optional<Opcode> convert_opcode(Type from, Type to) noexcept {
  if (from == to) return Opcode::Mov;
  if (from == Type::b() || to == Type::b()) return std::nullopt;
  if (from == Type::p() || to == Type::p()) return std::nullopt;

  if (is_integer(from) && is_integer(to)) {
    if (type_size(from) >= type_size(to)) return std::nullopt;
    if (is_signed_integer(from)) {
      switch (from.kind()) {
      case Type::Kind::I8: return Opcode::Ext8;
      case Type::Kind::I16: return Opcode::Ext16;
      case Type::Kind::I32: return Opcode::Ext32;
      default: return std::nullopt;
      }
    }
    if (is_unsigned_integer(from)) {
      switch (from.kind()) {
      case Type::Kind::U8: return Opcode::UExt8;
      case Type::Kind::U16: return Opcode::UExt16;
      case Type::Kind::U32: return Opcode::UExt32;
      default: return std::nullopt;
      }
    }
    return std::nullopt;
  }

  if (is_integer(from) && is_float(to)) {
    if (is_signed_integer(from)) {
      switch (to.kind()) {
      case Type::Kind::F: return Opcode::I2F;
      case Type::Kind::D: return Opcode::I2D;
      case Type::Kind::LD: return Opcode::I2LD;
      default: return std::nullopt;
      }
    }
    switch (to.kind()) {
    case Type::Kind::F: return Opcode::UI2F;
    case Type::Kind::D: return Opcode::UI2D;
    case Type::Kind::LD: return Opcode::UI2LD;
    default: return std::nullopt;
    }
  }

  if (is_float(from) && is_integer(to)) {
    switch (from.kind()) {
    case Type::Kind::F: return Opcode::F2I;
    case Type::Kind::D: return Opcode::D2I;
    case Type::Kind::LD: return Opcode::LD2I;
    default: return std::nullopt;
    }
  }

  if (is_float(from) && is_float(to)) {
    switch (from.kind()) {
    case Type::Kind::F:
      if (to == Type::d()) return Opcode::F2D;
      if (to == Type::ld()) return Opcode::F2LD;
      return std::nullopt;
    case Type::Kind::D:
      if (to == Type::f()) return Opcode::D2F;
      if (to == Type::ld()) return Opcode::D2LD;
      return std::nullopt;
    case Type::Kind::LD:
      if (to == Type::f()) return Opcode::LD2F;
      if (to == Type::d()) return Opcode::LD2D;
      return std::nullopt;
    default: return std::nullopt;
    }
  }

  return std::nullopt;
}

std::optional<Opcode> binary_opcode(BinaryOp op, Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    return std::nullopt;
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

std::optional<Opcode> overflow_binary_opcode(BinaryOp op, Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
    switch (op) {
    case BinaryOp::Add: return Opcode::Addos;
    case BinaryOp::Sub: return Opcode::Subos;
    case BinaryOp::Mul: return Opcode::Mulos;
    case BinaryOp::Div:
    case BinaryOp::Mod:
      return std::nullopt;
    }
    break;
  case Type::Kind::I64:
    switch (op) {
    case BinaryOp::Add: return Opcode::Addo;
    case BinaryOp::Sub: return Opcode::Subo;
    case BinaryOp::Mul: return Opcode::Mulo;
    case BinaryOp::Div:
    case BinaryOp::Mod:
      return std::nullopt;
    }
    break;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
    switch (op) {
    case BinaryOp::Mul: return Opcode::UMulos;
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Div:
    case BinaryOp::Mod:
      return std::nullopt;
    }
    break;
  case Type::Kind::U64:
    switch (op) {
    case BinaryOp::Mul: return Opcode::UMulo;
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Div:
    case BinaryOp::Mod:
      return std::nullopt;
    }
    break;
  case Type::Kind::B:
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
  case Type::Kind::P:
    return std::nullopt;
  }
  return std::nullopt;
}

bool is_overflow_opcode(Opcode opcode) noexcept {
  switch (opcode) {
  case Opcode::Addo:
  case Opcode::Addos:
  case Opcode::Subo:
  case Opcode::Subos:
  case Opcode::Mulo:
  case Opcode::Mulos:
  case Opcode::UMulo:
  case Opcode::UMulos:
    return true;
  default:
    return false;
  }
}

std::optional<Opcode> unary_opcode(UnaryOp op, Type type) noexcept {
  switch (op) {
  case UnaryOp::Neg:
    switch (type.kind()) {
    case Type::Kind::B:
      return std::nullopt;
    case Type::Kind::I8:
    case Type::Kind::U8:
    case Type::Kind::I16:
    case Type::Kind::U16:
    case Type::Kind::I32:
    case Type::Kind::U32:
    case Type::Kind::I64:
    case Type::Kind::U64:
      return Opcode::Neg;
    case Type::Kind::F:
      return Opcode::FNeg;
    case Type::Kind::D:
      return Opcode::DNeg;
    case Type::Kind::LD:
      return Opcode::LDNeg;
    case Type::Kind::P:
      return std::nullopt;
    }
    break;
  }
  return std::nullopt;
}

std::optional<Opcode> integer_binary_opcode(IntegerBinaryOp op, Type type) noexcept {
  if (!is_integer(type)) return std::nullopt;
  switch (op) {
  case IntegerBinaryOp::And:
    return Opcode::And;
  case IntegerBinaryOp::Or:
    return Opcode::Or;
  case IntegerBinaryOp::Xor:
    return Opcode::Xor;
  case IntegerBinaryOp::Lsh:
    return Opcode::Lsh;
  case IntegerBinaryOp::Rsh:
    return is_signed_integer(type) ? Opcode::Rsh : Opcode::URsh;
  }
  return std::nullopt;
}

std::optional<Opcode> compare_opcode(CompareOp op, Type type) noexcept {
  switch (type.kind()) {
  case Type::Kind::B:
    switch (op) {
    case CompareOp::Eq: return Opcode::Eq;
    case CompareOp::Ne: return Opcode::Ne;
    case CompareOp::Lt:
    case CompareOp::Le:
    case CompareOp::Gt:
    case CompareOp::Ge:
      return std::nullopt;
    }
    break;
  case Type::Kind::I8:
  case Type::Kind::I16:
  case Type::Kind::I32:
  case Type::Kind::I64:
    switch (op) {
    case CompareOp::Eq: return Opcode::Eq;
    case CompareOp::Ne: return Opcode::Ne;
    case CompareOp::Lt: return Opcode::Lt;
    case CompareOp::Le: return Opcode::Le;
    case CompareOp::Gt: return Opcode::Gt;
    case CompareOp::Ge: return Opcode::Ge;
    }
    break;
  case Type::Kind::U8:
  case Type::Kind::U16:
  case Type::Kind::U32:
  case Type::Kind::U64:
    switch (op) {
    case CompareOp::Eq: return Opcode::Eq;
    case CompareOp::Ne: return Opcode::Ne;
    case CompareOp::Lt: return Opcode::ULt;
    case CompareOp::Le: return Opcode::ULe;
    case CompareOp::Gt: return Opcode::UGt;
    case CompareOp::Ge: return Opcode::UGe;
    }
    break;
  case Type::Kind::F:
    switch (op) {
    case CompareOp::Eq: return Opcode::FEq;
    case CompareOp::Ne: return Opcode::FNe;
    case CompareOp::Lt: return Opcode::FLt;
    case CompareOp::Le: return Opcode::FLe;
    case CompareOp::Gt: return Opcode::FGt;
    case CompareOp::Ge: return Opcode::FGe;
    }
    break;
  case Type::Kind::D:
    switch (op) {
    case CompareOp::Eq: return Opcode::DEq;
    case CompareOp::Ne: return Opcode::DNe;
    case CompareOp::Lt: return Opcode::DLt;
    case CompareOp::Le: return Opcode::DLe;
    case CompareOp::Gt: return Opcode::DGt;
    case CompareOp::Ge: return Opcode::DGe;
    }
    break;
  case Type::Kind::LD:
    switch (op) {
    case CompareOp::Eq: return Opcode::LDEq;
    case CompareOp::Ne: return Opcode::LDNe;
    case CompareOp::Lt: return Opcode::LDLt;
    case CompareOp::Le: return Opcode::LDLe;
    case CompareOp::Gt: return Opcode::LDGt;
    case CompareOp::Ge: return Opcode::LDGe;
    }
    break;
  case Type::Kind::P:
    return std::nullopt;
  }
  return std::nullopt;
}

Operand memory_operand(Type type, const Value &base, std::size_t index_register,
                       std::int64_t displacement, int scale) {
  return Operand::mem(type, base.operand().register_id(), index_register, displacement, scale);
}

bool valid_memory_scale(std::int64_t scale) noexcept {
  return scale == 1 || scale == 2 || scale == 4 || scale == 8;
}

Error invalid_operand(std::string message) {
  return Error{ErrorCode::InvalidOperand, std::move(message)};
}

} // namespace

Value::Value() noexcept : block_(nullptr), type_(Type::i64()), operand_(Operand::int64(0)) {}

Value::Value(Block &block, Type type, Operand operand) noexcept
    : block_(&block), type_(type), operand_(std::move(operand)) {}

Block &Value::block() const noexcept { return *block_; }
Type Value::type() const noexcept { return type_; }
Operand Value::operand() const noexcept { return operand_; }
bool Value::is_valid() const noexcept { return block_ != nullptr; }
bool Value::is_poison() const noexcept {
  return is_valid() && operand_.kind() == Operand::Kind::Poison;
}

Value &Value::overflow(bool enabled) & noexcept {
  overflow_enabled_ = enabled;
  if (!enabled) {
    overflow_pending_ = false;
    overflow_sequence_ = 0;
    unsigned_overflow_branch_ = false;
  }
  return *this;
}

Value &&Value::overflow(bool enabled) && noexcept {
  overflow_enabled_ = enabled;
  if (!enabled) {
    overflow_pending_ = false;
    overflow_sequence_ = 0;
    unsigned_overflow_branch_ = false;
  }
  return std::move(*this);
}

bool Value::checks_overflow() const noexcept { return overflow_enabled_; }

bool Value::has_overflow_result() const noexcept { return overflow_pending_; }

Value Value::poison(Block &block, Type type) noexcept {
  return Value(block, type, Operand::poison());
}

Value Value::convert(Type::Kind to) const {
  if (!is_valid()) return Value();
  return block_->convert(*this, to);
}

Value Value::convert(Type to) const {
  if (!is_valid()) return Value();
  return block_->convert(*this, to);
}

Memory Value::as_mem(Type element_type) const {
  if (!is_valid()) return Memory();
  return block_->mem(element_type, *this);
}

Var::Var() noexcept : block_(nullptr), storage_() {}
Var::Var(Block &block, Value storage) noexcept : block_(&block), storage_(std::move(storage)) {}
Var::operator Value() const noexcept { return storage_; }
Block &Var::block() const noexcept { return *block_; }
Type Var::type() const noexcept { return storage_.type(); }
Value Var::value() const noexcept { return storage_; }
bool Var::is_valid() const noexcept { return block_ != nullptr && storage_.is_valid(); }

Var Var::poison(Block &block, Type type) noexcept {
  return Var(block, Value::poison(block, type));
}

void Var::assign(Value rhs) {
  if (!is_valid()) return;
  if (block_->error()) return;
  if (block_->kind() == Block::Kind::Module) {
    init(std::move(rhs));
    return;
  }
  block_->assign(storage_, std::move(rhs));
}

void Var::init(Value rhs) {
  if (!is_valid()) return;
  if (block_->error()) return;
  if (block_->kind() != Block::Kind::Module) {
    assign(std::move(rhs));
    return;
  }
  block_->init_module_var(storage_, std::move(rhs));
}

Memory::Memory() noexcept : block_(nullptr), type_(Type::i64()), operand_(Operand::int64(0)) {}
Memory::Memory(Block &block, Type type, Operand operand) noexcept
    : block_(&block), type_(type), operand_(std::move(operand)) {}
Block &Memory::block() const noexcept { return *block_; }
Type Memory::type() const noexcept { return type_; }
Operand Memory::operand() const noexcept { return operand_; }
bool Memory::is_valid() const noexcept { return block_ != nullptr; }
Memory Memory::poison(Block &block, Type type) noexcept {
  return Memory(block, type, Operand::poison());
}

Memory Memory::cast_ptr(Type element_type) const {
  if (!is_valid()) return Memory();
  return block_->cast_memory(*this, element_type);
}

Memory Memory::operator[](std::int64_t index) const {
  if (!is_valid()) return Memory();
  return block_->index_memory(*this, index);
}

Memory Memory::operator[](Value index) const {
  if (!is_valid()) return Memory();
  return block_->index_memory(*this, std::move(index));
}

Expr::Expr() noexcept : block_(nullptr), type_(Type::i64()), value_() {}
Expr::Expr(Value value) noexcept
    : block_(value.is_valid() ? &value.block() : nullptr),
      type_(value.is_valid() ? value.type() : Type::i64()), value_(std::move(value)) {}
Expr::Expr(Block &block, Type type) noexcept : block_(&block), type_(type), value_() {}
Expr::Expr(Block &block, Value value) noexcept
    : block_(&block), type_(value.is_valid() ? value.type() : Type::i64()),
      value_(std::move(value)) {}
Block &Expr::block() const noexcept { return *block_; }
Type Expr::type() const noexcept { return type_; }
bool Expr::is_valid() const noexcept { return block_ != nullptr && value_.is_valid(); }
Value Expr::value() const noexcept { return value_; }
Block *Expr::owner() const noexcept { return block_; }

Expr Expr::invalid(Block *block, Type type, Error error) {
  if (block != nullptr) {
    block->fail(std::move(error));
    return Expr(*block, block->poison_value(type));
  }
  return Expr();
}

Expr Expr::literal_like(const Expr &expr, std::int64_t value) {
  if (Block *block = expr.owner()) {
    return Expr(*block, block->integer_literal(expr.type(), value));
  }
  return Expr();
}

Expr Expr::unary(int op_value, Expr value) {
  UnaryOp op = static_cast<UnaryOp>(op_value);
  Block *block = value.owner();
  const Type type = value.type();
  if (block == nullptr) return Expr();
  if (block->error()) return Expr(*block, block->poison_value(type));
  if (!value.is_valid()) {
    return invalid(block, type, invalid_operand("invalid expression operand"));
  }
  const std::optional<Opcode> opcode = unary_opcode(op, type);
  if (!opcode) {
    return invalid(block, type, invalid_operand("unsupported operand type for expression"));
  }
  return Expr(*block, block->append_unary_in_current(*opcode, value.value()));
}

Expr Expr::binary(int op_value, Expr lhs, Expr rhs) {
  BinaryOp op = static_cast<BinaryOp>(op_value);
  Block *block = lhs.owner() != nullptr ? lhs.owner() : rhs.owner();
  const Type type = lhs.owner() != nullptr ? lhs.type() : rhs.type();
  if (block == nullptr) return Expr();
  if (block->error()) return Expr(*block, block->poison_value(type));
  if (!lhs.is_valid() || !rhs.is_valid()) {
    return invalid(block, type, invalid_operand("invalid expression operand"));
  }
  const bool overflow_checked = lhs.value().checks_overflow() || rhs.value().checks_overflow();
  if (overflow_checked) {
    if (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul) {
      const std::optional<Opcode> overflow_opcode = overflow_binary_opcode(op, lhs.type());
      if (!overflow_opcode) {
        return invalid(block, lhs.type(), invalid_operand("checked overflow requires signed integer operands"));
      }
      return Expr(*block, block->append_binary_in_current(*overflow_opcode, lhs.value(),
                                                          rhs.value()));
    }
    return invalid(block, lhs.type(), invalid_operand("checked overflow value cannot be used with this operator"));
  }
  const std::optional<Opcode> opcode = binary_opcode(op, lhs.type());
  if (op == BinaryOp::Mod && !is_integer(lhs.type())) {
    return invalid(block, lhs.type(), invalid_operand("operator requires integer operands"));
  }
  if (!opcode) {
    const auto integer_op = static_cast<IntegerBinaryOp>(op_value);
    const std::optional<Opcode> integer_opcode = integer_binary_opcode(integer_op, lhs.type());
    if (!integer_opcode) {
      return invalid(block, lhs.type(), invalid_operand("unsupported operand type for expression"));
    }
    return Expr(*block, block->append_integer_binary_in_current(*integer_opcode, lhs.value(),
                                                               rhs.value()));
  }
  return Expr(*block, block->append_binary_in_current(*opcode, lhs.value(), rhs.value()));
}

Value Expr::invalid_compare(Block *block, Error error) {
  if (block != nullptr) {
    block->fail(std::move(error));
    return block->poison_value(Type::b());
  }
  return Value();
}

Value Expr::compare(int op_value, Expr lhs, Expr rhs) {
  CompareOp op = static_cast<CompareOp>(op_value);
  Block *block = lhs.owner() != nullptr ? lhs.owner() : rhs.owner();
  if (block == nullptr) return Value();
  if (block->error()) return block->poison_value(Type::b());
  if (!lhs.is_valid() || !rhs.is_valid()) {
    return invalid_compare(block, invalid_operand("invalid comparison operand"));
  }
  if (lhs.value().checks_overflow() || rhs.value().checks_overflow()) {
    return invalid_compare(block, invalid_operand("checked overflow value cannot be used in comparison"));
  }
  const std::optional<Opcode> opcode = compare_opcode(op, lhs.type());
  if (!opcode) {
    return invalid_compare(block, invalid_operand("unsupported operand type for comparison"));
  }
  return block->compare_in_current(*opcode, lhs.value(), rhs.value());
}

namespace detail {

Expr expr_literal_like(const Expr &expr, std::int64_t value) {
  return Expr::literal_like(expr, value);
}

Expr expr_unary(int op, Expr value) {
  return Expr::unary(op, std::move(value));
}

Expr expr_binary(int op, Expr lhs, Expr rhs) {
  return Expr::binary(op, std::move(lhs), std::move(rhs));
}

Value expr_compare(int op, Expr lhs, Expr rhs) {
  return Expr::compare(op, std::move(lhs), std::move(rhs));
}

} // namespace detail

Data &Module::bss(std::string_view name, std::size_t size) {
  data_items_.push_back(std::make_unique<Data>(std::string(name), size));
  return *data_items_.back();
}

Data &Module::data(std::string_view name, Type element_type, std::vector<std::int64_t> values) {
  std::vector<Operand> operands;
  operands.reserve(values.size());
  if (!is_typed_integer_data_type(element_type)) {
    fail(invalid_operand("integer data requires integer-like element type"));
  } else if (!is_signed_data_type(element_type)) {
    for (std::int64_t value : values) {
      if (value < 0) {
        fail(invalid_operand("integer data value is out of range"));
        break;
      }
      const std::uint64_t unsigned_value = static_cast<std::uint64_t>(value);
      if (unsigned_value > unsigned_max(element_type)) {
        fail(invalid_operand("integer data value is out of range"));
        break;
      }
    }
  } else {
    for (std::int64_t value : values) {
      if (value < signed_min(element_type) || value > signed_max(element_type)) {
        fail(invalid_operand("integer data value is out of range"));
        break;
      }
    }
  }
  for (std::int64_t value : values) operands.push_back(Operand::int64(value));
  data_items_.push_back(std::make_unique<Data>(std::string(name), element_type,
                                               Data::ValueKind::Int64,
                                               std::move(operands)));
  return *data_items_.back();
}

Data &Module::data(std::string_view name, Type element_type, std::vector<std::uint64_t> values) {
  std::vector<Operand> operands;
  operands.reserve(values.size());
  if (!is_typed_integer_data_type(element_type)) {
    fail(invalid_operand("unsigned integer data requires integer-like element type"));
  } else if (is_signed_data_type(element_type)) {
    for (std::uint64_t value : values) {
      if (value > static_cast<std::uint64_t>(signed_max(element_type))) {
        fail(invalid_operand("integer data value is out of range"));
        break;
      }
    }
  } else {
    for (std::uint64_t value : values) {
      if (value > unsigned_max(element_type)) {
        fail(invalid_operand("integer data value is out of range"));
        break;
      }
    }
  }
  for (std::uint64_t value : values) operands.push_back(Operand::uint64(value));
  data_items_.push_back(std::make_unique<Data>(std::string(name), element_type,
                                               Data::ValueKind::UInt64,
                                               std::move(operands)));
  return *data_items_.back();
}

Data &Module::data(std::string_view name, Type element_type, std::vector<float> values) {
  std::vector<Operand> operands;
  operands.reserve(values.size());
  if (element_type != Type::f()) {
    fail(invalid_operand("float data requires f element type"));
  }
  for (float value : values) operands.push_back(Operand::float32(value));
  data_items_.push_back(std::make_unique<Data>(std::string(name), element_type,
                                               Data::ValueKind::Float32,
                                               std::move(operands)));
  return *data_items_.back();
}

Data &Module::data(std::string_view name, Type element_type, std::vector<double> values) {
  std::vector<Operand> operands;
  operands.reserve(values.size());
  if (element_type != Type::d()) {
    fail(invalid_operand("double data requires d element type"));
  }
  for (double value : values) operands.push_back(Operand::float64(value));
  data_items_.push_back(std::make_unique<Data>(std::string(name), element_type,
                                               Data::ValueKind::Float64,
                                               std::move(operands)));
  return *data_items_.back();
}

Data &Module::data(std::string_view name, Type element_type,
                   std::vector<long double> values) {
  std::vector<Operand> operands;
  operands.reserve(values.size());
  if (element_type != Type::ld()) {
    fail(invalid_operand("long double data requires ld element type"));
  }
  for (long double value : values) operands.push_back(Operand::long_double(value));
  data_items_.push_back(std::make_unique<Data>(std::string(name), element_type,
                                               Data::ValueKind::LongDouble,
                                               std::move(operands)));
  return *data_items_.back();
}

Data &Module::string_data(std::string_view name, std::string_view value) {
  data_items_.push_back(std::make_unique<Data>(std::string(name), std::string(value)));
  return *data_items_.back();
}

Data &Module::ref_data(std::string_view name, const Import &target,
                       std::int64_t displacement) {
  data_items_.push_back(std::make_unique<Data>(
      std::string(name),
      Data::RefTarget{Operand::ReferenceKind::Import, &target, displacement}));
  return *data_items_.back();
}

Data &Module::ref_data(std::string_view name, const Function &target,
                       std::int64_t displacement) {
  data_items_.push_back(std::make_unique<Data>(
      std::string(name),
      Data::RefTarget{Operand::ReferenceKind::Function, &target, displacement}));
  return *data_items_.back();
}

Data &Module::ref_data(std::string_view name, const Prototype &target,
                       std::int64_t displacement) {
  data_items_.push_back(std::make_unique<Data>(
      std::string(name),
      Data::RefTarget{Operand::ReferenceKind::Prototype, &target, displacement}));
  return *data_items_.back();
}

VarRef::VarRef() noexcept : block_(nullptr), var_() {}
VarRef::VarRef(Block &block, Var var) noexcept : block_(&block), var_(std::move(var)) {}

VarRef::operator Expr() const {
  if (block_ == nullptr) return Expr();
  if (block_->error()) {
    return Expr(*block_, block_->poison_value(var_.is_valid() ? var_.type() : Type::i64()));
  }
  return Expr(*block_, block_->load(var_));
}

void VarRef::operator=(Expr rhs) const {
  if (block_ == nullptr) return;
  if (block_->error()) return;
  if (!rhs.is_valid()) {
    block_->fail(invalid_operand("assignment source is invalid"));
    return;
  }
  block_->store(var_, rhs.value());
}

void VarRef::operator=(Value rhs) const { *this = block_ != nullptr ? block_->expr(std::move(rhs)) : Expr(); }

void VarRef::operator=(std::int64_t rhs) const {
  if (block_ == nullptr) return;
  if (block_->error()) return;
  *this = Expr(*block_, block_->integer_literal(var_.is_valid() ? var_.type() : Type::i64(), rhs));
}

MemoryRef::MemoryRef() noexcept : block_(nullptr), memory_() {}
MemoryRef::MemoryRef(Block &block, Memory memory) noexcept
    : block_(&block), memory_(std::move(memory)) {}

MemoryRef::operator Value() const {
  if (block_ == nullptr) return Value();
  if (block_->error()) {
    return block_->poison_value(memory_.is_valid() ? memory_.type() : Type::i64());
  }
  return block_->load(memory_);
}

void MemoryRef::operator=(Value rhs) const {
  if (block_ == nullptr) return;
  if (block_->error()) return;
  block_->store(memory_, std::move(rhs));
}

Value Block::i8(std::int8_t value) { return integer_literal(Type::i8(), value); }
Value Block::u8(std::uint8_t value) { return integer_literal(Type::u8(), value); }
Value Block::i16(std::int16_t value) { return integer_literal(Type::i16(), value); }
Value Block::u16(std::uint16_t value) { return integer_literal(Type::u16(), value); }
Value Block::i32(std::int32_t value) { return integer_literal(Type::i32(), value); }
Value Block::u32(std::uint32_t value) { return integer_literal(Type::u32(), value); }
Value Block::i64(std::int64_t value) { return integer_literal(Type::i64(), value); }
Value Block::u64(std::uint64_t value) {
  if (error()) return poison_value(Type::u64());
  return Value(*this, Type::u64(), Operand::uint64(value));
}
Value Block::b(bool value) {
  if (error()) return poison_value(Type::b());
  return Value(*this, Type::b(), Operand::int64(value ? 1 : 0));
}
Value Block::f32(float value) {
  if (error()) return poison_value(Type::f());
  return Value(*this, Type::f(), Operand::float32(value));
}
Value Block::f64(double value) {
  if (error()) return poison_value(Type::d());
  return Value(*this, Type::d(), Operand::float64(value));
}
Value Block::ld(long double value) {
  if (error()) return poison_value(Type::ld());
  return Value(*this, Type::ld(), Operand::long_double(value));
}

VarRef Block::operator[](Var var) { return VarRef(*this, std::move(var)); }

MemoryRef Block::operator[](Memory memory) { return MemoryRef(*this, std::move(memory)); }

Expr Block::expr(Value value) {
  if (error()) return Expr(*this, poison_value(value.is_valid() ? value.type() : Type::i64()));
  if (!value.is_valid()) {
    fail(invalid_operand("expression value is invalid"));
    return Expr(*this, poison_value(Type::i64()));
  }
  return Expr(*this, std::move(value));
}

Expr Block::expr(std::int64_t value) {
  return Expr(*this, i64(value));
}

Value Block::integer_literal(Type type, std::int64_t value) {
  if (error()) return poison_value(type);
  if (!is_integer_literal_type(type)) {
    fail(invalid_operand("integer literal requires integer-like type"));
    return poison_value(type);
  }
  if (type == Type::u64()) {
    return Value(*this, type, Operand::uint64(static_cast<std::uint64_t>(value)));
  }
  return Value(*this, type, Operand::int64(value));
}

Var Block::var(Type type, std::string_view name) {
  if (error()) return poison_var(type);
  if (kind_ == Kind::Module) return module_var(type, name, true);
  if (function_ == nullptr) {
    fail(invalid_operand("block cannot create a local"));
    return poison_var(type);
  }
  Result<Register> reg = function_->create_register_internal(type, name, false);
  if (!reg) {
    fail(std::move(reg).error());
    return poison_var(type);
  }
  Register reg_value = *reg;
  return Var(*this, Value(*this, type, Operand(reg_value)));
}

Value Block::value(Type type, std::string_view name) {
  if (error()) return poison_value(type);
  if (kind_ == Kind::Module) {
    return module_var(type, name, false).value();
  }
  return var(type, name).value();
}

Value Block::addr(Value ref, std::string_view name) {
  if (error()) return poison_value(Type::p());
  if (function_ == nullptr) {
    fail(invalid_operand("addr requires a function block"));
    return poison_value(Type::p());
  }
  if (!validate_reference_value(ref, "addr reference")) return poison_value(Type::p());

  Result<Register> dst = name.empty()
                              ? temp(Type::p())
                              : function_->create_register_internal(Type::p(), name, false);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(Type::p());
  }
  Register dst_value = *dst;
  append_operation(Instruction(Opcode::Addr, {Operand(dst_value), ref.operand_}));
  return Value(*this, Type::p(), Operand(dst_value));
}

Var Block::module_var(Type type, std::string_view name, bool writable) {
  if (error()) return poison_var(type);
  if (kind_ != Kind::Module) {
    fail(invalid_operand("module binding requires module block"));
    return poison_var(type);
  }
  if (name.empty()) {
    fail(Error{ErrorCode::InvalidArgument, "module binding name must not be empty"});
    return poison_var(type);
  }
  for (const ModuleBinding &binding : module_bindings_) {
    if (binding.name == name) {
      fail(Error{ErrorCode::DuplicateName, "duplicate module binding name"});
      return poison_var(type);
    }
  }
  module_bindings_.push_back(ModuleBinding(type, std::string(name), writable));
  const std::size_t id = module_bindings_.size();
  return Var(*this, Value(*this, type, Operand::module_slot(id)));
}

void Block::init_module_var(Value storage, Value rhs) {
  if (error()) return;
  if (kind_ != Kind::Module) {
    fail(invalid_operand("module initializer requires module block"));
    return;
  }
  if (!storage.is_valid() || &storage.block() != this
      || storage.operand().kind() != Operand::Kind::ModuleSlot) {
    fail(invalid_operand("initializer destination is not a module binding"));
    return;
  }
  if (!rhs.is_valid() || &rhs.block().module() != &module()) {
    fail(invalid_operand("module initializer does not belong to this module"));
    return;
  }
  if (rhs.type() != storage.type()) {
    fail(invalid_operand("module initializer type does not match"));
    return;
  }
  const std::size_t id = storage.operand().module_slot_id();
  if (id == 0 || id > module_bindings_.size()) {
    fail(invalid_operand("unknown module binding"));
    return;
  }
  ModuleBinding &binding = module_bindings_[id - 1];
  if (!binding.writable) {
    fail(invalid_operand("module binding is read-only"));
    return;
  }
  binding.initializer = rhs.operand();
}

Memory Block::alloca(std::int64_t size, std::string_view name) {
  if (error()) return poison_memory(Type::u8());
  if (function_ == nullptr) {
    fail(invalid_operand("alloca requires a function block"));
    return poison_memory(Type::u8());
  }
  if (size < 0) {
    fail(Error{ErrorCode::InvalidArgument, "alloca size must be non-negative"});
    return poison_memory(Type::u8());
  }

  Result<Register> reg = function_->create_register_internal(Type::p(), name, false);
  if (!reg) {
    fail(std::move(reg).error());
    return poison_memory(Type::u8());
  }
  Register reg_value = *reg;
  append_operation(Instruction(Opcode::Alloca, {Operand(reg_value), Operand::int64(size)}));
  Value base(*this, Type::p(), Operand(reg_value));
  return Memory(*this, Type::u8(), memory_operand(Type::u8(), base, 0, 0, 1));
}

Memory Block::alloca(Type element_type, std::string_view name) {
  return alloca(element_type, 1, name);
}

Memory Block::alloca(Type element_type, std::int64_t count, std::string_view name) {
  if (error()) return poison_memory(element_type);
  if (count < 0) {
    fail(Error{ErrorCode::InvalidArgument, "alloca element count must be non-negative"});
    return poison_memory(element_type);
  }
  if (!is_memory_type(element_type)) {
    fail(invalid_operand("alloca element type is not supported"));
    return poison_memory(element_type);
  }
  const std::int64_t element_size = type_size(element_type);
  if (count > std::numeric_limits<std::int64_t>::max() / element_size) {
    fail(Error{ErrorCode::InvalidArgument, "alloca size overflow"});
    return poison_memory(element_type);
  }
  return alloca(element_size * count, name).cast_ptr(element_type);
}

Memory Block::mem(Type type, Value base, std::int64_t displacement) {
  if (error()) return poison_memory(type);
  if (!is_memory_type(type)) {
    fail(invalid_operand("memory type is not supported"));
    return poison_memory(type);
  }
  if (!validate_register_value(base, "memory base")) return poison_memory(type);
  return Memory(*this, type, memory_operand(type, base, 0, displacement, 1));
}

Memory Block::mem(Type type, Value base, Value index, int scale,
                  std::int64_t displacement) {
  if (error()) return poison_memory(type);
  if (!is_memory_type(type)) {
    fail(invalid_operand("memory type is not supported"));
    return poison_memory(type);
  }
  if (!valid_memory_scale(scale)) {
    fail(Error{ErrorCode::InvalidArgument, "memory scale must be 1, 2, 4, or 8"});
    return poison_memory(type);
  }
  if (!validate_register_value(base, "memory base")
      || !validate_register_value(index, "memory index")) {
    return poison_memory(type);
  }
  return Memory(*this, type, memory_operand(type, base, index.operand_.register_id(),
                                            displacement, scale));
}

Value Block::load(Memory memory) {
  if (error()) return poison_value(memory.is_valid() ? memory.type() : Type::i64());
  if (!validate_memory(memory)) return poison_value(memory.is_valid() ? memory.type() : Type::i64());
  const std::optional<Opcode> opcode = typed_move_opcode(memory.type_);
  if (!opcode) {
    fail(invalid_operand("unsupported memory load type"));
    return poison_value(memory.type_);
  }

  Result<Register> dst = temp(memory.type_);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(memory.type_);
  }
  Register dst_value = *dst;
  append_operation(Instruction(*opcode, {Operand(dst_value), memory.operand_}));
  return Value(*this, memory.type_, Operand(dst_value));
}

Value Block::load(Var var) {
  if (error()) return poison_value(var.is_valid() ? var.type() : Type::i64());
  if (!var.is_valid()) {
    fail(invalid_operand("load source is invalid"));
    return poison_value(Type::i64());
  }
  if (is_module_slot_operand(var.value())) {
    fail(invalid_operand("module data load is not supported by materialization yet"));
    return poison_value(var.type());
  }
  if (var.value().block().function() != function_) {
    fail(invalid_operand("load source does not belong to this function"));
    return poison_value(var.type());
  }
  return Value(*this, var.type(), var.value().operand());
}

void Block::store(Memory memory, Value value) {
  if (error()) return;
  if (!validate_memory(memory) || !validate_value(value, "store value")) return;
  if (memory.type_ != value.type_) {
    fail(invalid_operand("store value type does not match memory type"));
    return;
  }
  const std::optional<Opcode> opcode = typed_move_opcode(memory.type_);
  if (!opcode) {
    fail(invalid_operand("unsupported memory store type"));
    return;
  }
  append_operation(Instruction(*opcode, {memory.operand_, value.operand_}));
}

void Block::store(Var var, Value value) {
  if (error()) return;
  if (!var.is_valid()) {
    fail(invalid_operand("store destination is invalid"));
    return;
  }
  if (is_module_slot_operand(var.value())) {
    fail(invalid_operand("module data store is not supported by materialization yet"));
    return;
  }
  assign(var.value(), std::move(value));
}

void Block::assign(Value dst, Value src) {
  if (error()) return;
  if (!validate_register_value(dst, "assign destination")
      || !validate_value(src, "assign source")) {
    return;
  }
  if (dst.type_ != src.type_) {
    fail(invalid_operand("assign source type does not match destination type"));
    return;
  }
  const std::optional<Opcode> opcode = typed_move_opcode(dst.type_);
  if (!opcode) {
    fail(invalid_operand("unsupported assignment type"));
    return;
  }
  append_operation(Instruction(*opcode, {dst.operand_, src.operand_}));
}

Value Block::convert(Value value, Type::Kind to) {
  return convert(std::move(value), Type::from_kind(to));
}

Value Block::convert(Value value, Type to) {
  if (error()) return poison_value(to);
  if (!validate_value(value, "convert source")) return poison_value(to);
  if (value.type_ == to) return value;
  const std::optional<Opcode> opcode = convert_opcode(value.type_, to);
  if (!opcode) {
    fail(invalid_operand("unsupported conversion"));
    return poison_value(to);
  }

  Result<Register> dst = temp(to);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(to);
  }
  Register dst_value = *dst;
  append_operation(Instruction(*opcode, {Operand(dst_value), value.operand_}));
  return Value(*this, to, Operand(dst_value));
}

Value Block::call(const Prototype &prototype, const Import &callee,
                  std::vector<Value> args) {
  CallResult values = append_call(prototype, Operand::ref(callee), std::move(args), 1);
  if (values.size() != 1) {
    if (!error()) fail(invalid_operand("call return count does not match requested shape"));
    return poison_value(Type::i64());
  }
  return values[0];
}

Value Block::call(const Prototype &prototype, const Function &callee,
                  std::vector<Value> args) {
  CallResult values = append_call(prototype, Operand::ref(callee), std::move(args), 1);
  if (values.size() != 1) {
    if (!error()) fail(invalid_operand("call return count does not match requested shape"));
    return poison_value(Type::i64());
  }
  return values[0];
}

CallResult Block::call_multi(const Prototype &prototype, const Import &callee,
                             std::vector<Value> args) {
  return append_call(prototype, Operand::ref(callee), std::move(args), std::nullopt);
}

CallResult Block::call_multi(const Prototype &prototype, const Function &callee,
                             std::vector<Value> args) {
  return append_call(prototype, Operand::ref(callee), std::move(args), std::nullopt);
}

void Block::call_void(const Prototype &prototype, const Import &callee,
                      std::vector<Value> args) {
  append_call(prototype, Operand::ref(callee), std::move(args), 0);
}

void Block::call_void(const Prototype &prototype, const Function &callee,
                      std::vector<Value> args) {
  append_call(prototype, Operand::ref(callee), std::move(args), 0);
}

void Block::jmp(Label &target) {
  if (error()) return;
  if (!validate_target(target)) return;
  append_operation(Instruction(Opcode::Jmp, {Operand(target)}));
}

void Block::if_(Value cond, Label &target) {
  if (error()) return;
  if (!validate_value(cond, "condition")) return;
  if (cond.type_ != Type::b()) {
    fail(invalid_operand("condition must be a bool value"));
    return;
  }
  if (!validate_target(target)) return;
  append_operation(Instruction(Opcode::Bt, {Operand(target), cond.operand_}));
}

void Block::if_not(Value cond, Label &target) {
  if (error()) return;
  if (!validate_value(cond, "condition")) return;
  if (cond.type_ != Type::b()) {
    fail(invalid_operand("condition must be a bool value"));
    return;
  }
  if (!validate_target(target)) return;
  append_operation(Instruction(Opcode::Bf, {Operand(target), cond.operand_}));
}

void Block::if_overflow(Value value, Label &target) {
  if (error()) return;
  if (!validate_overflow_branch_value(value)) return;
  if (!validate_target(target)) return;
  append_operation(Instruction(value.unsigned_overflow_branch_ ? Opcode::UBo : Opcode::Bo,
                               {Operand(target)}));
}

void Block::if_no_overflow(Value value, Label &target) {
  if (error()) return;
  if (!validate_overflow_branch_value(value)) return;
  if (!validate_target(target)) return;
  append_operation(Instruction(value.unsigned_overflow_branch_ ? Opcode::UBno : Opcode::Bno,
                               {Operand(target)}));
}

void Block::switch_(Value index, std::vector<Label *> targets) {
  if (error()) return;
  if (!validate_switch_index(index)) return;
  if (targets.empty()) {
    fail(Error{ErrorCode::InvalidArgument, "switch target list must not be empty"});
    return;
  }
  std::vector<Operand> operands;
  operands.reserve(targets.size() + 1);
  operands.push_back(index.operand_);
  for (Label *target : targets) {
    if (target == nullptr) {
      fail(invalid_operand("switch target is null"));
      return;
    }
    if (!validate_target(*target)) return;
    operands.push_back(Operand(*target));
  }
  append_operation(Instruction(Opcode::Switch, std::move(operands)));
}

void Block::switch_(Value index, std::initializer_list<Label *> targets) {
  switch_(std::move(index), std::vector<Label *>(targets));
}

void Block::switch_(Value index, std::vector<SwitchCase> cases, Label &default_target) {
  if (error()) return;
  if (!validate_switch_index(index)) return;
  if (cases.empty()) {
    fail(Error{ErrorCode::InvalidArgument, "switch case list must not be empty"});
    return;
  }
  if (!validate_target(default_target)) return;

  std::vector<std::int64_t> seen;
  seen.reserve(cases.size());
  for (const SwitchCase &switch_case : cases) {
    if (switch_case.target == nullptr) {
      fail(invalid_operand("switch case target is null"));
      return;
    }
    if (!validate_target(*switch_case.target)) return;
    if (std::find(seen.begin(), seen.end(), switch_case.value) != seen.end()) {
      fail(Error{ErrorCode::DuplicateName, "duplicate switch case value"});
      return;
    }
    seen.push_back(switch_case.value);
  }

  for (const SwitchCase &switch_case : cases) {
    if_(index == integer_literal(index.type(), switch_case.value), *switch_case.target);
    if (error()) return;
  }
  jmp(default_target);
}

void Block::switch_(Value index, std::initializer_list<SwitchCase> cases,
                    Label &default_target) {
  switch_(std::move(index), std::vector<SwitchCase>(cases), default_target);
}

void Block::ret(Value value) {
  if (error()) return;
  if (!validate_value(value, "return value")) return;
  append_operation(Instruction(Opcode::Ret, {value.operand_}));
}

void Block::ret(Expr value) {
  if (error()) return;
  if (!value.is_valid()) {
    fail(invalid_operand("return value is invalid"));
    return;
  }
  ret(value.value());
}

void Block::ret(std::vector<Value> values) {
  if (error()) return;
  std::vector<Operand> operands;
  operands.reserve(values.size());
  for (const Value &value : values) {
    if (!validate_value(value, "return value")) return;
    operands.push_back(value.operand_);
  }
  append_operation(Instruction(Opcode::Ret, std::move(operands)));
}

void Block::ret() {
  if (error()) return;
  append_operation(Instruction(Opcode::Ret));
}

Result<Register> Block::temp(Type type) {
  if (error()) return Error{ErrorCode::InvalidOperand, "module is already in error state"};
  if (function_ == nullptr) return invalid_operand("temporary register requires a function block");
  std::size_t attempt = 0;
  while (true) {
    const std::string name = ".t" + std::to_string(function_->local_registers().size() + attempt++);
    Result<Register> reg = function_->create_register_internal(type, name, false);
    if (reg) return reg;
    if (reg.error().code != ErrorCode::DuplicateName) return std::move(reg).error();
  }
}

bool Block::validate_value(const Value &value, std::string_view description) {
  if (!value.is_valid()) {
    fail(invalid_operand(std::string(description) + " is invalid"));
    return false;
  }
  if (value.is_poison()) {
    fail(invalid_operand(std::string(description) + " is poison"));
    return false;
  }
  if (&value.block().module() != &module()) {
    fail(invalid_operand(std::string(description) + " does not belong to this module"));
    return false;
  }
  if (function_ != nullptr && value.block().function() != function_ && !is_reference_operand(value)) {
    fail(invalid_operand(std::string(description) + " does not belong to this function"));
    return false;
  }
  return true;
}

bool Block::validate_register_value(const Value &value, std::string_view description) {
  if (!validate_value(value, description)) return false;
  if (!is_register_operand(value)) {
    fail(invalid_operand(std::string(description) + " must be a register value"));
    return false;
  }
  return true;
}

bool Block::validate_reference_value(const Value &value, std::string_view description) {
  if (!validate_value(value, description)) return false;
  if (!is_reference_operand(value)) {
    fail(invalid_operand(std::string(description) + " must be a reference value"));
    return false;
  }
  switch (value.operand_.reference_kind()) {
  case Operand::ReferenceKind::Data:
  case Operand::ReferenceKind::Function:
  case Operand::ReferenceKind::Import:
  case Operand::ReferenceKind::Prototype:
    if (module_contains_reference(module(), value.operand_)) return true;
    fail(invalid_operand(std::string(description) + " does not belong to this module"));
    return false;
  case Operand::ReferenceKind::None:
    break;
  }
  fail(invalid_operand(std::string(description) + " has an unsupported reference kind"));
  return false;
}

bool Block::validate_memory(const Memory &memory) {
  if (!memory.is_valid()) {
    fail(invalid_operand("memory operand is invalid"));
    return false;
  }
  if (&memory.block().module() != &module()) {
    fail(invalid_operand("memory operand does not belong to this module"));
    return false;
  }
  if (function_ != nullptr && memory.block().function() != function_) {
    fail(invalid_operand("memory operand does not belong to this function"));
    return false;
  }
  if (memory.operand_.kind() != Operand::Kind::Memory) {
    fail(invalid_operand("operand is not memory"));
    return false;
  }
  return true;
}

Memory Block::cast_memory(Memory memory, Type element_type) {
  if (error()) return poison_memory(element_type);
  if (!is_memory_type(element_type)) {
    fail(invalid_operand("memory type is not supported"));
    return poison_memory(element_type);
  }
  if (!validate_memory(memory)) return poison_memory(element_type);
  const Operand &operand = memory.operand_;
  return Memory(*this, element_type,
                Operand::mem(element_type, operand.memory_base_register_id(),
                             operand.memory_index_register_id(),
                             operand.memory_displacement(), operand.memory_scale()));
}

Memory Block::index_memory(Memory memory, std::int64_t index) {
  if (error()) return poison_memory(memory.is_valid() ? memory.type() : Type::i64());
  if (!validate_memory(memory)) return poison_memory(memory.is_valid() ? memory.type() : Type::i64());
  const std::int64_t element_size = type_size(memory.type_);
  if (index > 0 && element_size > std::numeric_limits<std::int64_t>::max() / index) {
    fail(Error{ErrorCode::InvalidArgument, "memory index displacement overflow"});
    return poison_memory(memory.type_);
  }
  if (index < 0 && index < std::numeric_limits<std::int64_t>::min() / element_size) {
    fail(Error{ErrorCode::InvalidArgument, "memory index displacement overflow"});
    return poison_memory(memory.type_);
  }
  const std::int64_t offset = index * element_size;
  const std::int64_t displacement = memory.operand_.memory_displacement();
  if ((offset > 0 && displacement > std::numeric_limits<std::int64_t>::max() - offset)
      || (offset < 0 && displacement < std::numeric_limits<std::int64_t>::min() - offset)) {
    fail(Error{ErrorCode::InvalidArgument, "memory index displacement overflow"});
    return poison_memory(memory.type_);
  }
  const Operand &operand = memory.operand_;
  return Memory(*this, memory.type_,
                Operand::mem(memory.type_, operand.memory_base_register_id(),
                             operand.memory_index_register_id(), displacement + offset,
                             operand.memory_scale()));
}

Memory Block::index_memory(Memory memory, Value index) {
  if (error()) return poison_memory(memory.is_valid() ? memory.type() : Type::i64());
  if (!validate_memory(memory)) return poison_memory(memory.is_valid() ? memory.type() : Type::i64());
  if (memory.operand_.memory_index_register_id() != 0) {
    fail(invalid_operand("memory operand already has an index register"));
    return poison_memory(memory.type_);
  }
  const std::int64_t scale64 = type_size(memory.type_);
  if (!valid_memory_scale(scale64)) {
    fail(Error{ErrorCode::InvalidArgument, "memory scale must be 1, 2, 4, or 8"});
    return poison_memory(memory.type_);
  }
  if (!validate_register_value(index, "memory index")) return poison_memory(memory.type_);
  const Operand &operand = memory.operand_;
  Block &owner = index.block();
  return Memory(owner, memory.type_,
                Operand::mem(memory.type_, operand.memory_base_register_id(),
                             index.operand_.register_id(), operand.memory_displacement(),
                             static_cast<int>(scale64)));
}

bool Block::validate_target(const Label &target) {
  if (!target.is_valid() || target.function() != function_) {
    fail(invalid_operand("target label does not belong to this function"));
    return false;
  }
  return true;
}

bool Block::validate_overflow_branch_value(const Value &value) {
  if (!validate_value(value, "overflow value")) return false;
  if (!value.has_overflow_result()) {
    fail(invalid_operand("overflow branch requires a checked overflow result"));
    return false;
  }
  if (&value.block() != this) {
    fail(invalid_operand("overflow result does not belong to this block"));
    return false;
  }
  if (value.overflow_sequence_ == 0 || pending_overflow_sequence_ == 0
      || value.overflow_sequence_ != pending_overflow_sequence_) {
    fail(invalid_operand("overflow result is not the current pending overflow value"));
    return false;
  }
  return true;
}

bool Block::validate_switch_index(const Value &index) {
  if (!validate_value(index, "switch index")) return false;
  if (!is_switch_index_type(index.type_)) {
    fail(invalid_operand("switch index must be an integer scalar"));
    return false;
  }
  return true;
}

bool Block::validate_call_args(const Prototype &prototype, const std::vector<Value> &args) {
  if (args.size() != prototype.parameters().size()) {
    fail(invalid_operand("call argument count does not match prototype"));
    return false;
  }
  for (std::size_t i = 0; i < args.size(); ++i) {
    const Value &arg = args[i];
    if (!validate_value(arg, "call argument")) return false;
    if (&arg.block() != this && !is_register_operand(arg)) {
      fail(invalid_operand("call literal argument does not belong to this block"));
      return false;
    }
    if (arg.type_ != prototype.parameters()[i].type) {
      fail(invalid_operand("call argument type does not match prototype"));
      return false;
    }
  }
  return true;
}

Value Block::poison_value(Type type) noexcept {
  return Value::poison(*this, type);
}

Var Block::poison_var(Type type) noexcept {
  return Var::poison(*this, type);
}

Memory Block::poison_memory(Type type) noexcept {
  return Memory::poison(*this, type);
}

CallResult Block::poison_call_result(const Prototype &prototype) noexcept {
  std::vector<Value> values;
  values.reserve(prototype.return_types().size());
  for (Type type : prototype.return_types()) values.push_back(poison_value(type));
  return CallResult(std::move(values));
}

Block *Block::binary_emit_block(Value lhs, Value rhs) {
  if (error()) return this;
  if (!lhs.is_valid() || !rhs.is_valid()) {
    fail(invalid_operand("invalid operand"));
    return this;
  }
  if (&lhs.block().module() != &rhs.block().module()) {
    fail(invalid_operand("operands must belong to the same module"));
    return &lhs.block();
  }
  if (lhs.block().function() != rhs.block().function()) {
    fail(invalid_operand("operands must belong to the same function"));
    return &lhs.block();
  }
  if (&lhs.block() == &rhs.block()) return &lhs.block();
  if (is_register_operand(lhs) && !is_register_operand(rhs)) return &rhs.block();
  if (!is_register_operand(lhs) && is_register_operand(rhs)) return &lhs.block();
  if (is_register_operand(lhs) && is_register_operand(rhs)) return &lhs.block();
  fail(invalid_operand("literal operands must belong to the same block"));
  return &lhs.block();
}

std::size_t Block::mark_overflow_pending() noexcept {
  pending_overflow_sequence_ = ++overflow_sequence_;
  return pending_overflow_sequence_;
}

CallResult Block::append_call(const Prototype &prototype, Operand callee,
                              std::vector<Value> args,
                              std::optional<std::size_t> return_count) {
  if (error()) return poison_call_result(prototype);
  if (return_count && prototype.return_types().size() != *return_count) {
    fail(invalid_operand("call return count does not match requested shape"));
    return poison_call_result(prototype);
  }
  if (!validate_call_args(prototype, args)) return poison_call_result(prototype);

  std::vector<Value> results;
  results.reserve(prototype.return_types().size());
  std::vector<Operand> operands;
  operands.reserve(2 + prototype.return_types().size() + args.size());
  operands.push_back(Operand::ref(prototype));
  operands.push_back(std::move(callee));

  for (Type type : prototype.return_types()) {
    Result<Register> reg = temp(type);
    if (!reg) {
      fail(std::move(reg).error());
      return poison_call_result(prototype);
    }
    Register reg_value = *reg;
    operands.push_back(Operand(reg_value));
    results.push_back(Value(*this, type, Operand(reg_value)));
  }
  for (const Value &arg : args) operands.push_back(arg.operand_);

  append_operation(Instruction(Opcode::Call, std::move(operands)));
  return CallResult(std::move(results));
}

Value Block::append_unary(Opcode opcode, Value value) {
  if (error()) return poison_value(value.is_valid() ? value.type_ : Type::i64());
  if (!value.is_valid()) {
    fail(invalid_operand("operand is invalid"));
    return poison_value(Type::i64());
  }
  Block *emit_block = &value.block();
  if (emit_block->error()) return emit_block->poison_value(value.type_);
  if (!emit_block->validate_value(value, "operand")) {
    return emit_block->poison_value(value.type_);
  }
  if (!is_numeric_scalar(value.type_)) {
    emit_block->fail(invalid_operand("operand must be a numeric scalar value"));
    return emit_block->poison_value(value.type_);
  }

  Result<Register> dst = emit_block->temp(value.type_);
  if (!dst) {
    emit_block->fail(std::move(dst).error());
    return emit_block->poison_value(value.type_);
  }
  Register dst_value = *dst;
  emit_block->append_operation(Instruction(opcode, {Operand(dst_value), value.operand_}));
  return Value(*emit_block, value.type_, Operand(dst_value));
}

Value Block::append_unary_in_current(Opcode opcode, Value value) {
  if (error()) return poison_value(value.is_valid() ? value.type_ : Type::i64());
  if (!validate_value(value, "operand")) {
    return poison_value(value.is_valid() ? value.type_ : Type::i64());
  }
  if (!is_numeric_scalar(value.type_)) {
    fail(invalid_operand("operand must be a numeric scalar value"));
    return poison_value(value.type_);
  }

  Result<Register> dst = temp(value.type_);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(value.type_);
  }
  Register dst_value = *dst;
  append_operation(Instruction(opcode, {Operand(dst_value), value.operand_}));
  return Value(*this, value.type_, Operand(dst_value));
}

Value Block::append_binary(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  Block *emit_block = binary_emit_block(lhs, rhs);
  if (error()) return emit_block->poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  if (!emit_block->validate_value(lhs, "left operand")
      || !emit_block->validate_value(rhs, "right operand")) {
    return emit_block->poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  }
  if (lhs.type_ != rhs.type_) {
    emit_block->fail(invalid_operand("operand types do not match"));
    return emit_block->poison_value(lhs.type_);
  }
  if (!is_numeric_scalar(lhs.type_)) {
    emit_block->fail(invalid_operand("operands must be numeric scalar values"));
    return emit_block->poison_value(lhs.type_);
  }

  Result<Register> dst = emit_block->temp(lhs.type_);
  if (!dst) {
    emit_block->fail(std::move(dst).error());
    return emit_block->poison_value(lhs.type_);
  }
  Register dst_value = *dst;
  emit_block->append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  Value result(*emit_block, lhs.type_, Operand(dst_value));
  if (is_overflow_opcode(opcode)) {
    result.overflow(true);
    result.overflow_pending_ = true;
    result.overflow_sequence_ = emit_block->mark_overflow_pending();
    result.unsigned_overflow_branch_ = opcode == Opcode::UMulo || opcode == Opcode::UMulos;
  }
  return result;
}

Value Block::append_binary_in_current(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  if (!validate_value(lhs, "left operand") || !validate_value(rhs, "right operand")) {
    return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  }
  if (lhs.type_ != rhs.type_) {
    fail(invalid_operand("operand types do not match"));
    return poison_value(lhs.type_);
  }
  if (!is_numeric_scalar(lhs.type_)) {
    fail(invalid_operand("operands must be numeric scalar values"));
    return poison_value(lhs.type_);
  }

  Result<Register> dst = temp(lhs.type_);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(lhs.type_);
  }
  Register dst_value = *dst;
  append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  Value result(*this, lhs.type_, Operand(dst_value));
  if (is_overflow_opcode(opcode)) {
    result.overflow(true);
    result.overflow_pending_ = true;
    result.overflow_sequence_ = mark_overflow_pending();
    result.unsigned_overflow_branch_ = opcode == Opcode::UMulo || opcode == Opcode::UMulos;
  }
  return result;
}

Value Block::append_integer_binary(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  Block *emit_block = binary_emit_block(lhs, rhs);
  if (error()) return emit_block->poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  if (!emit_block->validate_value(lhs, "left operand")
      || !emit_block->validate_value(rhs, "right operand")) {
    return emit_block->poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  }
  if (lhs.type_ != rhs.type_) {
    emit_block->fail(invalid_operand("operand types do not match"));
    return emit_block->poison_value(lhs.type_);
  }
  if (!is_integer(lhs.type_)) {
    emit_block->fail(invalid_operand("operator requires integer operands"));
    return emit_block->poison_value(lhs.type_);
  }

  Result<Register> dst = emit_block->temp(lhs.type_);
  if (!dst) {
    emit_block->fail(std::move(dst).error());
    return emit_block->poison_value(lhs.type_);
  }
  Register dst_value = *dst;
  emit_block->append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  return Value(*emit_block, lhs.type_, Operand(dst_value));
}

Value Block::append_integer_binary_in_current(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  if (!validate_value(lhs, "left operand") || !validate_value(rhs, "right operand")) {
    return poison_value(lhs.is_valid() ? lhs.type_ : Type::i64());
  }
  if (lhs.type_ != rhs.type_) {
    fail(invalid_operand("operand types do not match"));
    return poison_value(lhs.type_);
  }
  if (!is_integer(lhs.type_)) {
    fail(invalid_operand("operator requires integer operands"));
    return poison_value(lhs.type_);
  }

  Result<Register> dst = temp(lhs.type_);
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(lhs.type_);
  }
  Register dst_value = *dst;
  append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  return Value(*this, lhs.type_, Operand(dst_value));
}

Value Block::compare(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(Type::b());
  Block *emit_block = binary_emit_block(lhs, rhs);
  if (error()) return emit_block->poison_value(Type::b());
  if (!emit_block->validate_value(lhs, "left operand")
      || !emit_block->validate_value(rhs, "right operand")) {
    return emit_block->poison_value(Type::b());
  }
  if (lhs.type_ != rhs.type_) {
    emit_block->fail(invalid_operand("operand types do not match"));
    return emit_block->poison_value(Type::b());
  }
  if (!is_numeric_scalar(lhs.type_) && lhs.type_ != Type::b()) {
    emit_block->fail(invalid_operand("operands must be scalar values"));
    return emit_block->poison_value(Type::b());
  }
  Result<Register> dst = emit_block->temp(Type::b());
  if (!dst) {
    emit_block->fail(std::move(dst).error());
    return emit_block->poison_value(Type::b());
  }
  Register dst_value = *dst;
  emit_block->append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  return Value(*emit_block, Type::b(), Operand(dst_value));
}

Value Block::compare_in_current(Opcode opcode, Value lhs, Value rhs) {
  if (error()) return poison_value(Type::b());
  if (!validate_value(lhs, "left operand") || !validate_value(rhs, "right operand")) {
    return poison_value(Type::b());
  }
  if (lhs.type_ != rhs.type_) {
    fail(invalid_operand("operand types do not match"));
    return poison_value(Type::b());
  }
  if (!is_numeric_scalar(lhs.type_) && lhs.type_ != Type::b()) {
    fail(invalid_operand("operands must be scalar values"));
    return poison_value(Type::b());
  }
  Result<Register> dst = temp(Type::b());
  if (!dst) {
    fail(std::move(dst).error());
    return poison_value(Type::b());
  }
  Register dst_value = *dst;
  append_operation(Instruction(opcode, {Operand(dst_value), lhs.operand_, rhs.operand_}));
  return Value(*this, Type::b(), Operand(dst_value));
}

namespace detail {

Value value_unary(int op_value, Value value) {
  UnaryOp op = static_cast<UnaryOp>(op_value);
  if (!value.is_valid()) return Value();
  if (value.block().error()) return Value::poison(value.block(), value.type_);
  const std::optional<Opcode> opcode = unary_opcode(op, value.type_);
  if (!opcode) {
    value.block().fail(invalid_operand("unsupported operand type for unary negation"));
    return Value::poison(value.block(), value.type_);
  }
  return value.block().append_unary(*opcode, std::move(value));
}

Value value_binary(int op_value, Value lhs, Value rhs) {
  if (op_value >= detail::kBinaryAnd) {
    const auto op = static_cast<IntegerBinaryOp>(op_value);
    if (!lhs.is_valid()) return Value();
    if (lhs.block().error()) return Value::poison(lhs.block(), lhs.type_);
    if (lhs.checks_overflow() || rhs.checks_overflow()) {
      lhs.block().fail(invalid_operand("checked overflow value cannot be used with this operator"));
      return Value::poison(lhs.block(), lhs.type_);
    }
    const std::optional<Opcode> opcode = integer_binary_opcode(op, lhs.type_);
    if (!opcode) {
      lhs.block().fail(invalid_operand("operator requires integer operands"));
      return Value::poison(lhs.block(), lhs.type_);
    }
    return lhs.block().append_integer_binary(*opcode, std::move(lhs), std::move(rhs));
  }

  BinaryOp op = static_cast<BinaryOp>(op_value);
  if (!lhs.is_valid()) return Value();
  if (lhs.block().error()) return Value::poison(lhs.block(), lhs.type_);
  const bool overflow_checked = lhs.checks_overflow() || rhs.checks_overflow();
  if (overflow_checked) {
    if (op == BinaryOp::Add || op == BinaryOp::Sub || op == BinaryOp::Mul) {
      const std::optional<Opcode> overflow_opcode = overflow_binary_opcode(op, lhs.type_);
      if (!overflow_opcode) {
        lhs.block().fail(invalid_operand("checked overflow requires signed integer operands"));
        return Value::poison(lhs.block(), lhs.type_);
      }
      return lhs.block().append_binary(*overflow_opcode, std::move(lhs), std::move(rhs));
    }
    lhs.block().fail(invalid_operand("checked overflow value cannot be used with this operator"));
    return Value::poison(lhs.block(), lhs.type_);
  }
  if (op == BinaryOp::Mod && !is_integer(lhs.type_)) {
    lhs.block().fail(invalid_operand("operator requires integer operands"));
    return Value::poison(lhs.block(), lhs.type_);
  }
  const std::optional<Opcode> opcode = binary_opcode(op, lhs.type_);
  if (!opcode) {
    lhs.block().fail(invalid_operand("unsupported operand type for expression"));
    return Value::poison(lhs.block(), lhs.type_);
  }
  return lhs.block().append_binary(*opcode, std::move(lhs), std::move(rhs));
}

Value value_compare(int op_value, Value lhs, Value rhs) {
  CompareOp op = static_cast<CompareOp>(op_value);
  if (!lhs.is_valid()) return Value();
  if (lhs.block().error()) return Value::poison(lhs.block(), Type::b());
  if (lhs.checks_overflow() || rhs.checks_overflow()) {
    lhs.block().fail(invalid_operand("checked overflow value cannot be used in comparison"));
    return Value::poison(lhs.block(), Type::b());
  }
  const std::optional<Opcode> opcode = compare_opcode(op, lhs.type_);
  if (!opcode) {
    lhs.block().fail(invalid_operand("unsupported operand type for comparison"));
    return Value::poison(lhs.block(), Type::b());
  }
  return lhs.block().compare(*opcode, std::move(lhs), std::move(rhs));
}

} // namespace detail

Value operator-(Value value) { return detail::value_unary(detail::kUnaryNeg, std::move(value)); }
Value operator+(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryAdd, std::move(lhs), std::move(rhs)); }
Value operator-(Value lhs, Value rhs) { return detail::value_binary(detail::kBinarySub, std::move(lhs), std::move(rhs)); }
Value operator*(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryMul, std::move(lhs), std::move(rhs)); }
Value operator/(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryDiv, std::move(lhs), std::move(rhs)); }
Value operator%(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryMod, std::move(lhs), std::move(rhs)); }
Value operator&(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryAnd, std::move(lhs), std::move(rhs)); }
Value operator|(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryOr, std::move(lhs), std::move(rhs)); }
Value operator^(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryXor, std::move(lhs), std::move(rhs)); }
Value operator<<(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryLsh, std::move(lhs), std::move(rhs)); }
Value operator>>(Value lhs, Value rhs) { return detail::value_binary(detail::kBinaryRsh, std::move(lhs), std::move(rhs)); }
Value operator==(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareEq, std::move(lhs), std::move(rhs)); }
Value operator!=(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareNe, std::move(lhs), std::move(rhs)); }
Value operator<(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareLt, std::move(lhs), std::move(rhs)); }
Value operator<=(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareLe, std::move(lhs), std::move(rhs)); }
Value operator>(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareGt, std::move(lhs), std::move(rhs)); }
Value operator>=(Value lhs, Value rhs) { return detail::value_compare(detail::kCompareGe, std::move(lhs), std::move(rhs)); }

} // namespace mirnext
