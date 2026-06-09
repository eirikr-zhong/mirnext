/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_HPP
#define MIRNEXT_HPP

#include <cstdint>
#include <cstdlib>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mirnext {

class Function;
class Import;
class Prototype;

enum class ErrorCode { InvalidArgument, DuplicateName, UnknownName, NoInsertPoint, InvalidOperand };

struct Error {
  ErrorCode code;
  std::string message;
};

template <class T> class Result {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(Error error) : storage_(std::move(error)) {}

  bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const noexcept { return has_value(); }

  T &value() noexcept {
    if (T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  const T &value() const noexcept {
    if (const T *value = std::get_if<T>(&storage_)) return *value;
    std::abort();
  }

  Error &error() noexcept {
    if (Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

  const Error &error() const noexcept {
    if (const Error *error = std::get_if<Error>(&storage_)) return *error;
    std::abort();
  }

private:
  std::variant<T, Error> storage_;
};

class Type {
public:
  enum class Kind { I8, U8, I16, U16, I32, U32, I64, U64, F, D, LD, P };

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

class Label {
public:
  Label() = default;

  std::size_t id() const noexcept;
  bool is_valid() const noexcept;

private:
  friend class Function;

  Label(const Function *function, std::size_t id) noexcept;

  const Function *function_ = nullptr;
  std::size_t id_ = 0;
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
  enum class Kind { Int64, Float32, Float64, LongDouble, Register, LabelRef, Memory, Reference };
  enum class ReferenceKind { None, Prototype, Import, Function };

  Operand(Register reg);
  Operand(Label label);
  Operand(std::int64_t value);

  static Operand reg(Register value);
  static Operand int64(std::int64_t value);
  static Operand float32(float value);
  static Operand float64(double value);
  static Operand long_double(long double value);
  static Operand label_ref(Label label);
  static Operand label_ref(std::size_t label_id);
  static Operand mem(Type type, Register base, Register index, std::int64_t displacement = 0,
                     int scale = 1);
  static Operand ref(const Prototype &prototype);
  static Operand ref(const Import &import);
  static Operand ref(const Function &function);

  Kind kind() const noexcept;
  std::size_t register_id() const noexcept;
  std::int64_t int64_value() const noexcept;
  float float32_value() const noexcept;
  double float64_value() const noexcept;
  long double long_double_value() const noexcept;
  std::size_t label_id() const noexcept;
  Type memory_type() const noexcept;
  std::int64_t memory_displacement() const noexcept;
  std::size_t memory_base_register_id() const noexcept;
  std::size_t memory_index_register_id() const noexcept;
  int memory_scale() const noexcept;
  ReferenceKind reference_kind() const noexcept;
  const void *reference_pointer() const noexcept;

private:
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

class Function {
public:
  struct Parameter {
    Type type;
    std::string name;
  };

  explicit Function(std::string name);
  Function(std::string name, std::vector<Type> return_types, std::vector<Parameter> parameters);

  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Function(Function &&) = delete;
  Function &operator=(Function &&) = delete;

  std::string_view name() const noexcept;
  const std::vector<Type> &return_types() const noexcept;
  const std::vector<Register> &arguments() const noexcept;
  Result<Register> argument(std::string_view name) const;
  const Register *find_argument(std::string_view name) const noexcept;
  Result<Register> create_register(Type type, std::string_view name);
  const std::vector<Register> &local_registers() const noexcept;
  const Register *find_register(std::string_view name) const noexcept;
  const Register *find_register(std::size_t id) const noexcept;
  Label create_label();
  Instruction &append(Instruction instruction);
  Instruction &append_label();
  Instruction &append_label(Label label);
  Instruction &append_ret(std::vector<Operand> values = {});
  std::size_t instruction_count() const noexcept;
  const std::vector<std::unique_ptr<Instruction>> &instructions() const noexcept;

private:
  Result<Register> create_register_internal(Type type, std::string_view name, bool argument);
  bool has_register_name(std::string_view name) const noexcept;

  std::string name_;
  std::vector<Type> return_types_;
  std::vector<Register> arguments_;
  std::vector<Register> local_registers_;
  std::vector<std::unique_ptr<Instruction>> instructions_;
  std::size_t next_register_id_ = 1;
  std::size_t next_label_id_ = 1;
};

class Module {
public:
  explicit Module(std::string name);

  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Module(Module &&) = delete;
  Module &operator=(Module &&) = delete;

  std::string_view name() const noexcept;
  Function &new_function(std::string_view name);
  Function &new_function(std::string_view name, std::vector<Type> return_types,
                         std::vector<Function::Parameter> parameters);
  Prototype &new_prototype(std::string_view name, std::vector<Type> return_types,
                           std::vector<Prototype::Parameter> parameters);
  Import &new_import(std::string_view name);
  const std::vector<std::unique_ptr<Prototype>> &prototypes() const noexcept;
  const std::vector<std::unique_ptr<Import>> &imports() const noexcept;
  const std::vector<std::unique_ptr<Function>> &functions() const noexcept;

private:
  std::string name_;
  std::vector<std::unique_ptr<Prototype>> prototypes_;
  std::vector<std::unique_ptr<Import>> imports_;
  std::vector<std::unique_ptr<Function>> functions_;
};

class Context {
public:
  Context() = default;
  ~Context() = default;

  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Context(Context &&) = delete;
  Context &operator=(Context &&) = delete;

  Module &new_module(std::string_view name);
  const std::vector<std::unique_ptr<Module>> &modules() const noexcept;
  void dump(std::ostream &out) const;

private:
  std::vector<std::unique_ptr<Module>> modules_;
};

class IRBuilder {
public:
  explicit IRBuilder(Context &context) noexcept;

  void set_insert_point(Function &function) noexcept;
  Function *insert_point() const noexcept;

  Result<Label> create_label();
  Result<Instruction *> bind(Label label);
  Result<Instruction *> create_alloca(Register dst, Operand size);
  Result<Instruction *> create_mov(Operand dst, Operand src);
  Result<Instruction *> create_mov(Register dst, Operand src);
  Result<Instruction *> create_fmov(Register dst, Operand src);
  Result<Instruction *> create_dmov(Register dst, Operand src);
  Result<Instruction *> create_ldmov(Register dst, Operand src);
  Result<Instruction *> create_ext8(Register dst, Operand src);
  Result<Instruction *> create_ext16(Register dst, Operand src);
  Result<Instruction *> create_ext32(Register dst, Operand src);
  Result<Instruction *> create_uext8(Register dst, Operand src);
  Result<Instruction *> create_uext16(Register dst, Operand src);
  Result<Instruction *> create_uext32(Register dst, Operand src);
  Result<Instruction *> create_i2f(Register dst, Operand src);
  Result<Instruction *> create_i2d(Register dst, Operand src);
  Result<Instruction *> create_i2ld(Register dst, Operand src);
  Result<Instruction *> create_ui2f(Register dst, Operand src);
  Result<Instruction *> create_ui2d(Register dst, Operand src);
  Result<Instruction *> create_ui2ld(Register dst, Operand src);
  Result<Instruction *> create_f2i(Register dst, Operand src);
  Result<Instruction *> create_d2i(Register dst, Operand src);
  Result<Instruction *> create_ld2i(Register dst, Operand src);
  Result<Instruction *> create_f2d(Register dst, Operand src);
  Result<Instruction *> create_f2ld(Register dst, Operand src);
  Result<Instruction *> create_d2f(Register dst, Operand src);
  Result<Instruction *> create_d2ld(Register dst, Operand src);
  Result<Instruction *> create_ld2f(Register dst, Operand src);
  Result<Instruction *> create_ld2d(Register dst, Operand src);
  Result<Instruction *> create_neg(Register dst, Operand src);
  Result<Instruction *> create_negs(Register dst, Operand src);
  Result<Instruction *> create_fneg(Register dst, Operand src);
  Result<Instruction *> create_dneg(Register dst, Operand src);
  Result<Instruction *> create_ldneg(Register dst, Operand src);
  Result<Instruction *> create_add(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_adds(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fadd(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dadd(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldadd(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_sub(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_subs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fsub(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dsub(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldsub(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_mul(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_muls(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fmul(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dmul(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldmul(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_div(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_divs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_udiv(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_udivs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fdiv(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ddiv(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_lddiv(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_mod(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_mods(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_umod(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_umods(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_and(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ands(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_or(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ors(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_xor(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_xors(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_lsh(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_lshs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_rsh(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_rshs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ursh(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_urshs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_eq(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_eqs(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_feq(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_deq(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldeq(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ne(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_nes(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fne(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dne(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldne(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_lt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_lts(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ult(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ults(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_flt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dlt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldlt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_le(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_les(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ule(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ules(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fle(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dle(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldle(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_gt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_gts(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ugt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ugts(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fgt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dgt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldgt(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ge(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ges(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_uge(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_uges(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_fge(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_dge(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldge(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_addo(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_addos(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_subo(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_subos(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_mulo(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_mulos(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_umulo(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_umulos(Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_jmp(Label target);
  Result<Instruction *> create_bt(Label target, Operand value);
  Result<Instruction *> create_bts(Label target, Operand value);
  Result<Instruction *> create_bf(Label target, Operand value);
  Result<Instruction *> create_bfs(Label target, Operand value);
  Result<Instruction *> create_beq(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_beqs(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fbeq(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dbeq(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldbeq(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bne(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bnes(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fbne(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dbne(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldbne(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bges(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ubge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ubges(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fbge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dbge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldbge(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_blt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_blts(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ublt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ublts(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fblt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dblt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldblt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ble(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bles(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_uble(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ubles(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fble(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dble(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldble(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bgt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bgts(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ubgt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ubgts(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_fbgt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_dbgt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_ldbgt(Label target, Operand lhs, Operand rhs);
  Result<Instruction *> create_bo(Label target);
  Result<Instruction *> create_ubo(Label target);
  Result<Instruction *> create_bno(Label target);
  Result<Instruction *> create_ubno(Label target);
  Result<Instruction *> create_call(const Prototype &prototype, Operand callee,
                                    std::vector<Operand> results,
                                    std::vector<Operand> arguments);
  Result<Instruction *> create_ret(std::vector<Operand> values = {});

private:
  Result<Instruction *> create_unary(Opcode opcode, Register dst, Operand src);
  Result<Instruction *> create_binary(Opcode opcode, Register dst, Operand lhs, Operand rhs);
  Result<Instruction *> create_branch(Opcode opcode, Label target);
  Result<Instruction *> create_branch(Opcode opcode, Label target, Operand value);
  Result<Instruction *> create_branch(Opcode opcode, Label target, Operand lhs, Operand rhs);
  Result<Function *> require_insert_point() const;

  Context *context_;
  Function *insert_point_ = nullptr;
};

const char *opcode_name(Opcode opcode) noexcept;
const char *type_name(Type type) noexcept;

} // namespace mirnext

#endif
