/* Licensed under the MIT License. Expressions eagerly emit canonical register IR. */
#include "mirnext/IR/IR.hpp"
#include <array>
#include <bit>

namespace mirnext::ir {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
bool narrow (Type type) {
  return type.kind () >= Type::Kind::I8 && type.kind () <= Type::Kind::U32;
}
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
bool unsigned_integer (Type type) {
  auto kind = type.kind ();
  return kind == Type::Kind::U8 || kind == Type::Kind::U16 || kind == Type::Kind::U32
         || kind == Type::Kind::U64 || kind == Type::Kind::Pointer;
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
Opcode comparison_opcode (Type type, Comparison comparison, bool branch) {
  using O = Opcode;
  struct Pair {
    O value, branch;
  };
  static constexpr Pair table[7][6] = {{{O::Eq, O::Beq},
                                        {O::Ne, O::Bne},
                                        {O::Lt, O::Blt},
                                        {O::Le, O::Ble},
                                        {O::Gt, O::Bgt},
                                        {O::Ge, O::Bge}},
                                       {{O::Eq, O::Beq},
                                        {O::Ne, O::Bne},
                                        {O::ULt, O::UBlt},
                                        {O::ULe, O::UBle},
                                        {O::UGt, O::UBgt},
                                        {O::UGe, O::UBge}},
                                       {{O::Eqs, O::Beqs},
                                        {O::Nes, O::Bnes},
                                        {O::Lts, O::Blts},
                                        {O::Les, O::Bles},
                                        {O::Gts, O::Bgts},
                                        {O::Ges, O::Bges}},
                                       {{O::Eqs, O::Beqs},
                                        {O::Nes, O::Bnes},
                                        {O::ULts, O::UBlts},
                                        {O::ULes, O::UBles},
                                        {O::UGts, O::UBgts},
                                        {O::UGes, O::UBges}},
                                       {{O::FEq, O::FBeq},
                                        {O::FNe, O::FBne},
                                        {O::FLt, O::FBlt},
                                        {O::FLe, O::FBle},
                                        {O::FGt, O::FBgt},
                                        {O::FGe, O::FBge}},
                                       {{O::DEq, O::DBeq},
                                        {O::DNe, O::DBne},
                                        {O::DLt, O::DBlt},
                                        {O::DLe, O::DBle},
                                        {O::DGt, O::DBgt},
                                        {O::DGe, O::DBge}},
                                       {{O::LDEq, O::LDBeq},
                                        {O::LDNe, O::LDBne},
                                        {O::LDLt, O::LDBlt},
                                        {O::LDLe, O::LDBle},
                                        {O::LDGt, O::LDBgt},
                                        {O::LDGe, O::LDBge}}};
  unsigned row = type.is_float () ? 4 + static_cast<unsigned> (type.kind ())
                                      - static_cast<unsigned> (Type::Kind::F32)
                                  : (narrow (type) ? 2 : 0) + unsigned_integer (type);
  auto pair = table[row][static_cast<unsigned> (comparison)];
  return branch ? pair.branch : pair.value;
}
std::optional<Opcode> arithmetic_opcode (Type type, unsigned operation) {
  using O = Opcode;
  if (type.kind () == Type::Kind::Bool || type.kind () == Type::Kind::Pointer) {
    if (type.kind () == Type::Kind::Bool && operation >= 5 && operation <= 7)
      return std::array{O::And, O::Or, O::Xor}[operation - 5];
    return {};
  }
  if (operation < 4) {
    static constexpr O table[7][4]
      = {{O::Add, O::Sub, O::Mul, O::Div},        {O::Add, O::Sub, O::Mul, O::UDiv},
         {O::Adds, O::Subs, O::Muls, O::Divs},    {O::Adds, O::Subs, O::Muls, O::UDivs},
         {O::FAdd, O::FSub, O::FMul, O::FDiv},    {O::DAdd, O::DSub, O::DMul, O::DDiv},
         {O::LDAdd, O::LDSub, O::LDMul, O::LDDiv}};
    unsigned row = type.is_float () ? 4 + static_cast<unsigned> (type.kind ())
                                        - static_cast<unsigned> (Type::Kind::F32)
                                    : (narrow (type) ? 2 : 0) + unsigned_integer (type);
    return table[row][operation];
  }
  if (!type.is_integer ()) return {};
  if (operation == 4)
    return unsigned_integer (type) ? (narrow (type) ? O::UMods : O::UMod)
                                   : (narrow (type) ? O::Mods : O::Mod);
  if (operation <= 8) {
    static constexpr O table[2][4]
      = {{O::And, O::Or, O::Xor, O::Lsh}, {O::Ands, O::Ors, O::Xors, O::Lshs}};
    return table[narrow (type)][operation - 5];
  }
  return unsigned_integer (type) ? (narrow (type) ? O::URshs : O::URsh)
                                 : (narrow (type) ? O::Rshs : O::Rsh);
}
Opcode move_opcode (Type type) {
  return type.kind () == Type::Kind::F32          ? Opcode::FMov
         : type.kind () == Type::Kind::F64        ? Opcode::DMov
         : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
                                                  : Opcode::Mov;
}
}  // namespace

Expression::Expression (IRBuilder &builder, Type type, std::optional<Operand> value)
  : builder_ (&builder), type_ (type) {
  if (value) {
    std::visit (
      [&] (const auto &scalar) {
        using T = std::decay_t<decltype (scalar)>;
        if constexpr (std::is_arithmetic_v<T> || std::is_same_v<T, const Register *>)
          value_.emplace (scalar);
      },
      value->storage ());
    if (!value_) builder.fail (invalid ("expression requires a scalar immediate or register"));
  }
}
bool Expression::is_valid () const {
  return value_.has_value () && !builder_->function_->module ().error ();
}
const Function &Expression::function () const { return *builder_->function_; }
Result<Operand> Expression::value () const {
  if (auto &error = function ().module ().error ()) return *error;
  if (!value_) return invalid ("invalid expression");
  return std::visit (
    [] (const auto &scalar) -> Operand {
      if constexpr (std::is_same_v<std::decay_t<decltype (scalar)>, const Register *>)
        return Operand (*scalar);
      else
        return Operand (scalar);
    },
    *value_);
}
Expression IRBuilder::expression (const Register &reg) {
  if (function_->module ().error ()) return {*this, reg.type (), {}};
  if (&reg.function () != function_ || !reg.type ().valid () || !reg.type ().is_scalar ()) {
    fail (invalid ("expression requires a scalar register from this function"));
    return {*this, reg.type (), {}};
  }
  return {*this, reg.type (), Operand (reg)};
}
Expression IRBuilder::expression (Memory memory) {
  auto type = memory.type;
  if (type.kind () == Type::Kind::Bool) {
    memory.type = Type::i64 ();
    auto reg = load (std::move (memory));
    return reg ? expression (**reg).convert (type) : Expression (*this, type, {});
  }
  auto reg = load (std::move (memory));
  return reg ? expression (**reg) : Expression (*this, type, {});
}
Expression IRBuilder::expression (const GlobalVariable &variable) {
  auto reg = load (variable);
  return reg ? expression (**reg) : Expression (*this, variable.type (), {});
}
Expression IRBuilder::literal (Operand value) {
  auto type = value.get<std::uint64_t> () ? Type (Type::Kind::U64)
              : value.get<float> ()       ? Type (Type::Kind::F32)
              : value.get<double> ()      ? Type (Type::Kind::F64)
              : value.get<long double> () ? Type (Type::Kind::LongDouble)
                                          : Type::i64 ();
  return literal (type, std::move (value));
}
Expression IRBuilder::literal (Type type, Operand value) {
  if (function_->module ().error ()) return {*this, type, {}};
  if (!type.valid () || !type.is_scalar ()
      || (!value.get<std::int64_t> () && !value.get<std::uint64_t> () && !value.get<float> ()
          && !value.get<double> () && !value.get<long double> ())) {
    fail (invalid ("literal requires a scalar type and an immediate number"));
    return {*this, type, {}};
  }
  if (type.kind () == Type::Kind::Bool) {
    if (auto number = value.get<std::int64_t> ())
      value = Operand (*number != 0);
    else if (auto number = value.get<std::uint64_t> ())
      value = Operand (*number != 0);
  }
  if (auto bits = value.get<std::uint64_t> ())
    value = Operand (std::bit_cast<std::int64_t> (*bits));
  Register candidate (*function_, type, {}, {}, false);
  const Operand operands[]{Operand (candidate), value};
  auto checked = validate_instruction (*function_, move_opcode (type), operands);
  if (!checked) {
    fail (checked.error ());
    return {*this, type, {}};
  }
  return {*this, type, std::move (value)};
}
Expression Expression::literal_like (Operand value) const {
  if (type_.is_float ()) {
    auto floating = [&] (auto number) -> Operand {
      if (type_.kind () == Type::Kind::F32) return Operand (static_cast<float> (number));
      if (type_.kind () == Type::Kind::F64) return Operand (static_cast<double> (number));
      return Operand (static_cast<long double> (number));
    };
    if (auto number = value.get<std::int64_t> ())
      value = floating (*number);
    else if (auto number = value.get<std::uint64_t> ())
      value = floating (*number);
  }
  return builder_->literal (type_, std::move (value));
}
Result<Operand> IRBuilder::expression_operand (const Expression &expression,
                                               std::optional<Type> expected) {
  if (function_->module ().error ()) return *function_->module ().error ();
  if (expression.builder_ != this) return fail (invalid ("expression belongs to another Builder"));
  auto value = expression.value ();
  if (!value) return fail (value.error ());
  if (expected && expression.type () != *expected)
    return fail (invalid ("expression type mismatch; use convert explicitly"));
  return value;
}
Expression Expression::apply (Operation operation, const Expression &rhs) const {
  auto lhs_value = builder_->expression_operand (*this);
  if (!lhs_value) return {*builder_, type_, {}};
  auto rhs_value = builder_->expression_operand (rhs, type_);
  if (!rhs_value) return {*builder_, type_, {}};
  bool comparison = operation >= Operation::Equal;
  auto opcode
    = comparison
        ? std::optional<Opcode> (
            comparison_opcode (type_,
                               static_cast<Comparison> (static_cast<unsigned> (operation)
                                                        - static_cast<unsigned> (Operation::Equal)),
                               false))
        : arithmetic_opcode (type_, static_cast<unsigned> (operation));
  auto type = comparison ? Type (Type::Kind::Bool) : type_;
  if (!opcode) {
    builder_->fail (invalid ("operation is not defined for this expression type"));
    return {*builder_, type, {}};
  }
  const Operand inputs[]{std::move (*lhs_value), std::move (*rhs_value)};
  auto reg = builder_->value_operation (*opcode, inputs, type, {});
  return reg ? builder_->expression (**reg) : Expression (*builder_, type, {});
}
Expression Expression::operator- () const {
  auto input = builder_->expression_operand (*this);
  if (!input) return {*builder_, type_, {}};
  if (type_.kind () == Type::Kind::Bool || type_.kind () == Type::Kind::Pointer) {
    builder_->fail (invalid ("negation requires a numeric expression"));
    return {*builder_, type_, {}};
  }
  auto opcode = type_.kind () == Type::Kind::F32          ? Opcode::FNeg
                : type_.kind () == Type::Kind::F64        ? Opcode::DNeg
                : type_.kind () == Type::Kind::LongDouble ? Opcode::LDNeg
                : narrow (type_)                          ? Opcode::Negs
                                                          : Opcode::Neg;
  auto reg = builder_->value_operation (opcode, std::span<const Operand> (&*input, 1), type_, {});
  return reg ? builder_->expression (**reg) : Expression (*builder_, type_, {});
}
Expression Expression::operator~() const {
  if (!type_.is_integer () || type_.kind () == Type::Kind::Bool
      || type_.kind () == Type::Kind::Pointer) {
    builder_->fail (invalid ("bitwise complement requires an integer expression"));
    return {*builder_, type_, {}};
  }
  return apply (Operation::Xor, literal_like (Operand (-1)));
}
Expression Expression::operator!() const {
  return apply (Operation::Equal, literal_like (Operand (0)));
}
Expression Expression::convert (Type type) const {
  return builder_->convert_expression (*this, type);
}

Expression IRBuilder::convert_expression (const Expression &expression, Type type) {
  auto value = expression_operand (expression);
  if (!value) return {*this, type, {}};
  if (!type.valid () || !type.is_scalar ()) {
    fail (invalid ("conversion requires a scalar destination"));
    return {*this, type, {}};
  }
  auto from = expression.type ();
  if (from == type) return expression;
  if (type.kind () == Type::Kind::Bool) {
    auto zero = expression.literal_like (Operand (0));
    return expression.apply (Expression::Operation::NotEqual, zero);
  }
  const bool pointer = from.kind () == Type::Kind::Pointer || type.kind () == Type::Kind::Pointer;
  auto wide_integer = [] (Type t) {
    return t.kind () == Type::Kind::I64 || t.kind () == Type::Kind::U64
           || t.kind () == Type::Kind::Pointer;
  };
  if (pointer && (!wide_integer (from) || !wide_integer (type))) {
    fail (invalid ("pointer conversion requires a 64-bit integer"));
    return {*this, type, {}};
  }
  if (from.is_float () && type.kind () == Type::Kind::U64) {
    fail (invalid ("MIR has no full-range floating-to-u64 conversion"));
    return {*this, type, {}};
  }
  if (!block_ || block_->terminator ()) {
    fail (invalid ("conversion requires an open insertion block"));
    return {*this, type, {}};
  }
  struct Operation {
    Opcode opcode;
    std::vector<Operand> operands;
  };
  std::vector<Operation> operations;
  operations.reserve (3);
  std::vector<std::unique_ptr<Register>> pending;
  pending.reserve (3);
  auto stage = [&] (Opcode opcode, Type result_type) {
    auto reg = std::unique_ptr<Register> (new Register (*function_, result_type, {}, {}, false));
    operations.push_back ({opcode, {Operand (*reg), *value}});
    value = Operand (*reg);
    pending.push_back (std::move (reg));
  };
  if (!pointer && narrow (from) && type.is_integer ()) {
    // Narrowing discards source high bits. Signed-to-wider-unsigned needs both extensions.
    if (narrow (type) && integer_width (type) > integer_width (from) && unsigned_integer (type)
        && !unsigned_integer (from)) {
      stage (*extension (from), Type::i64 ());
      stage (*extension (type), type);
    } else {
      auto opcode = narrow (type) && integer_width (type) <= integer_width (from)
                      ? extension (type)
                      : extension (from);
      stage (*opcode, type);
    }
  } else if (from.is_float () && type.is_float ()) {
    static constexpr Opcode conversions[3][3] = {{Opcode::FMov, Opcode::F2D, Opcode::F2LD},
                                                 {Opcode::D2F, Opcode::DMov, Opcode::D2LD},
                                                 {Opcode::LD2F, Opcode::LD2D, Opcode::LDMov}};
    auto offset = static_cast<unsigned> (Type::Kind::F32);
    stage (conversions[static_cast<unsigned> (from.kind ()) - offset]
                      [static_cast<unsigned> (type.kind ()) - offset],
           type);
  } else if (type.is_float ()) {
    if (narrow (from))
      stage (*extension (from), unsigned_integer (from) ? Type (Type::Kind::U64) : Type::i64 ());
    static constexpr Opcode conversions[2][3]
      = {{Opcode::I2F, Opcode::I2D, Opcode::I2LD}, {Opcode::UI2F, Opcode::UI2D, Opcode::UI2LD}};
    stage (conversions[unsigned_integer (from)][static_cast<unsigned> (type.kind ())
                                                - static_cast<unsigned> (Type::Kind::F32)],
           type);
  } else {
    if (from.is_float ())
      stage (from.kind () == Type::Kind::F32   ? Opcode::F2I
             : from.kind () == Type::Kind::F64 ? Opcode::D2I
                                               : Opcode::LD2I,
             narrow (type) ? Type::i64 () : type);
    if (auto opcode = extension (type))
      stage (*opcode, type);
    else if (pending.empty () || pending.back ()->type () != type)
      stage (Opcode::Mov, type);
  }
  for (const auto &operation : operations) {
    auto checked = validate_instruction (*function_, operation.opcode, operation.operands);
    if (!checked) {
      fail (checked.error ());
      return {*this, type, {}};
    }
  }
  for (auto &reg : pending) {
    function_->registers_.push_back (std::move (reg));
    function_->registers_changed ();
  }
  for (auto &operation : operations)
    append_validated (operation.opcode, std::move (operation.operands));
  return {*this, type, std::move (*value)};
}
Result<Instruction *> IRBuilder::assign (const Register &target, const Expression &value) {
  auto operand = expression_operand (value, target.type ());
  if (!operand) return operand.error ();
  return assign (target, std::move (*operand));
}
Result<Instruction *> IRBuilder::store (Memory target, const Expression &value) {
  auto operand = expression_operand (value, target.type);
  if (!operand) return operand.error ();
  return store (std::move (target), std::move (*operand));
}
Result<Instruction *> IRBuilder::store (const GlobalVariable &target, const Expression &value) {
  auto operand = expression_operand (value, target.type ());
  if (!operand) return operand.error ();
  return store (target, std::move (*operand));
}
Result<Instruction *> IRBuilder::ret (const Expression &value) {
  const auto &results = function_->signature ().results;
  if (results.size () != 1) return fail (invalid ("expression return requires exactly one result"));
  auto operand = expression_operand (value, results[0]);
  if (!operand) return operand.error ();
  return ret (std::move (*operand));
}
Result<Instruction *> IRBuilder::branch_if (const Expression &value, const Label &target,
                                            bool when_true) {
  auto operand = expression_operand (value);
  if (!operand) return operand.error ();
  return branch_if (std::move (*operand), target, when_true);
}
Result<Instruction *> IRBuilder::branch (Comparison comparison, const Expression &lhs,
                                         const Expression &rhs, const Label &target) {
  auto left = expression_operand (lhs);
  if (!left) return left.error ();
  auto right = expression_operand (rhs, lhs.type ());
  if (!right) return right.error ();
  if (static_cast<unsigned> (comparison) > static_cast<unsigned> (Comparison::GreaterEqual))
    return fail (invalid ("invalid comparison"));
  return emit (comparison_opcode (lhs.type (), comparison, true),
               {Operand (target), std::move (*left), std::move (*right)});
}
}  // namespace mirnext::ir
