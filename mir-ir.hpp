/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_IR_HPP
#define MIRNEXT_IR_HPP

#include "mir-result.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mirnext {

class Block;
class Module;
class Function;
class Label;
class Data;
class Prototype;
class Import;
class Instruction;
class Value;
class Var;
class Memory;
class Expr;
class VarRef;
class CallResult;

class Type {
public:
  enum class Kind { B, I8, U8, I16, U16, I32, U32, I64, U64, F, D, LD, P };

  static Type b() noexcept;
  static Type i8() noexcept;
  static Type u8() noexcept;
  static Type i16() noexcept;
  static Type u16() noexcept;
  static Type i32() noexcept;
  static Type u32() noexcept;
  static Type i64() noexcept;
  static Type u64() noexcept;
  static Type f() noexcept;
  static Type d() noexcept;
  static Type ld() noexcept;
  static Type p() noexcept;
  static Type from_kind(Kind kind) noexcept;

  Kind kind() const noexcept;

  friend bool operator==(Type lhs, Type rhs) noexcept { return lhs.kind_ == rhs.kind_; }
  friend bool operator!=(Type lhs, Type rhs) noexcept { return !(lhs == rhs); }

private:
  explicit Type(Kind kind) noexcept;

  Kind kind_;
};

class Register {
public:
  Register() = default;

  std::size_t id() const noexcept;
  Type type() const noexcept;
  std::string_view name() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class Function;

  Register(const Function *function, std::size_t id, Type type, std::string name);

  const Function *function_ = nullptr;
  std::size_t id_ = 0;
  Type type_ = Type::i64();
  std::string name_;
};

class Prototype {
public:
  struct Parameter {
    Type type;
    std::string name;
  };

  Prototype(std::string name, std::vector<Type> return_types, std::vector<Parameter> parameters);

  std::string_view name() const noexcept;
  const std::vector<Type> &return_types() const noexcept;
  const std::vector<Parameter> &parameters() const noexcept;

private:
  std::string name_;
  std::vector<Type> return_types_;
  std::vector<Parameter> parameters_;
};

class Import {
public:
  explicit Import(std::string name);

  std::string_view name() const noexcept;

private:
  std::string name_;
};

class Operand {
public:
  enum class Kind {
    Poison,
    Int64,
    UInt64,
    Float32,
    Float64,
    LongDouble,
    Register,
    LabelRef,
    Memory,
    ModuleSlot,
    Reference
  };
  enum class ReferenceKind { None, Prototype, Import, Function, Data };

  Operand(Register reg);
  Operand(const Label &label);
  Operand(std::int64_t value);
  Operand(std::uint64_t value);

  static Operand poison();
  static Operand reg(Register value);
  static Operand int64(std::int64_t value);
  static Operand uint64(std::uint64_t value);
  static Operand float32(float value);
  static Operand float64(double value);
  static Operand long_double(long double value);
  static Operand label_ref(const Label &label);
  static Operand label_ref(std::size_t label_id);
  static Operand mem(Type type, Register base, Register index, std::int64_t displacement = 0,
                     int scale = 1);
  static Operand mem(Type type, std::size_t base_register, std::size_t index_register,
                     std::int64_t displacement = 0, int scale = 1);
  static Operand module_slot(std::size_t id);
  static Operand ref(const Prototype &prototype);
  static Operand ref(const Import &import);
  static Operand ref(const Function &function);
  static Operand ref(const Data &data);

  Kind kind() const noexcept;
  std::size_t register_id() const noexcept;
  std::int64_t int64_value() const noexcept;
  std::uint64_t uint64_value() const noexcept;
  float float32_value() const noexcept;
  double float64_value() const noexcept;
  long double long_double_value() const noexcept;
  std::size_t label_id() const noexcept;
  std::size_t module_slot_id() const noexcept;
  Type memory_type() const noexcept;
  std::int64_t memory_displacement() const noexcept;
  std::size_t memory_base_register_id() const noexcept;
  std::size_t memory_index_register_id() const noexcept;
  int memory_scale() const noexcept;
  ReferenceKind reference_kind() const noexcept;
  const void *reference_pointer() const noexcept;

private:
  friend class Block;

  Operand(Kind kind, std::int64_t int_value, std::size_t reg, std::size_t label);
  Operand(Type memory_type, std::size_t base_register, std::size_t index_register,
          std::int64_t displacement, int scale);
  Operand(ReferenceKind reference_kind, const void *reference);

  Kind kind_;
  std::int64_t int_value_;
  float float32_value_ = 0.0f;
  double float64_value_ = 0.0;
  long double long_double_value_ = 0.0;
  std::size_t register_id_;
  std::size_t label_id_;
  std::size_t module_slot_id_ = 0;
  Type memory_type_ = Type::i64();
  std::int64_t memory_displacement_ = 0;
  std::size_t memory_base_register_id_ = 0;
  std::size_t memory_index_register_id_ = 0;
  int memory_scale_ = 1;
  ReferenceKind reference_kind_ = ReferenceKind::None;
  const void *reference_ = nullptr;
};

enum class Opcode {
  Label,
  Ret,
  Nop,
  Mov,
  FMov,
  DMov,
  LDMov,
  Ext8,
  Ext16,
  Ext32,
  UExt8,
  UExt16,
  UExt32,
  I2F,
  I2D,
  I2LD,
  UI2F,
  UI2D,
  UI2LD,
  F2I,
  D2I,
  LD2I,
  F2D,
  F2LD,
  D2F,
  D2LD,
  LD2F,
  LD2D,
  Addr,
  Alloca,
  Neg,
  Negs,
  FNeg,
  DNeg,
  LDNeg,
  Add,
  Adds,
  FAdd,
  DAdd,
  LDAdd,
  Sub,
  Subs,
  FSub,
  DSub,
  LDSub,
  Mul,
  Muls,
  FMul,
  DMul,
  LDMul,
  Div,
  Divs,
  UDiv,
  UDivs,
  FDiv,
  DDiv,
  LDDiv,
  Mod,
  Mods,
  UMod,
  UMods,
  And,
  Ands,
  Or,
  Ors,
  Xor,
  Xors,
  Lsh,
  Lshs,
  Rsh,
  Rshs,
  URsh,
  URshs,
  Eq,
  Eqs,
  FEq,
  DEq,
  LDEq,
  Ne,
  Nes,
  FNe,
  DNe,
  LDNe,
  Lt,
  Lts,
  ULt,
  ULts,
  FLt,
  DLt,
  LDLt,
  Le,
  Les,
  ULe,
  ULes,
  FLe,
  DLe,
  LDLe,
  Gt,
  Gts,
  UGt,
  UGts,
  FGt,
  DGt,
  LDGt,
  Ge,
  Ges,
  UGe,
  UGes,
  FGe,
  DGe,
  LDGe,
  Addo,
  Addos,
  Subo,
  Subos,
  Mulo,
  Mulos,
  UMulo,
  UMulos,
  Jmp,
  Bt,
  Bts,
  Bf,
  Bfs,
  Beq,
  Beqs,
  FBeq,
  DBeq,
  LDBeq,
  Bne,
  Bnes,
  FBne,
  DBne,
  LDBne,
  Blt,
  Blts,
  UBlt,
  UBlts,
  FBlt,
  DBlt,
  LDBlt,
  Ble,
  Bles,
  UBle,
  UBles,
  FBle,
  DBle,
  LDBle,
  Bgt,
  Bgts,
  UBgt,
  UBgts,
  FBgt,
  DBgt,
  LDBgt,
  Bge,
  Bges,
  UBge,
  UBges,
  FBge,
  DBge,
  LDBge,
  Bo,
  UBo,
  Bno,
  UBno,
  Call,
  Switch,
};

class Data {
public:
  enum class Kind { Bss, Typed, String, Ref };
  enum class ValueKind { Int64, UInt64, Float32, Float64, LongDouble };

  struct RefTarget {
    Operand::ReferenceKind kind = Operand::ReferenceKind::None;
    const void *pointer = nullptr;
    std::int64_t displacement = 0;
  };

  Data(std::string name, std::size_t size);
  Data(std::string name, Type element_type, ValueKind value_kind,
       std::vector<Operand> values);
  Data(std::string name, std::string value);
  Data(std::string name, RefTarget target);

  std::string_view name() const noexcept;
  bool is_valid() const noexcept;
  Kind kind() const noexcept;
  Type element_type() const noexcept;
  ValueKind value_kind() const noexcept;
  std::size_t size() const noexcept;
  const std::vector<Operand> &values() const noexcept;
  std::string_view string_value() const noexcept;
  const RefTarget &ref_target() const noexcept;

private:
  std::string name_;
  Kind kind_;
  Type element_type_ = Type::i64();
  ValueKind value_kind_ = ValueKind::Int64;
  std::size_t size_ = 0;
  std::vector<Operand> values_;
  std::string string_value_;
  RefTarget ref_target_;
};

class Instruction {
public:
  using Opcode = mirnext::Opcode;

  Instruction(Opcode opcode, std::vector<Operand> operands = {});

  Opcode opcode() const noexcept;
  const std::vector<Operand> &operands() const noexcept;
  std::size_t label_id() const noexcept;

private:
  friend class Function;

  void set_label_id(std::size_t label_id) noexcept;

  Opcode opcode_;
  std::vector<Operand> operands_;
  std::size_t label_id_ = 0;
};

} // namespace mirnext

#endif
