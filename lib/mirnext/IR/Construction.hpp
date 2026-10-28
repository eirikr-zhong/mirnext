/* Licensed under the MIT License. Checked construction with one error boundary. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include "mirnext/IR/Builder.hpp"
#include "mirnext/IR/ConstructionState.hpp"
#include "mirnext/IR/SSARewriter.hpp"

namespace mirnext::ir {
namespace detail {
struct BuildState;
struct FunctionBuildState;
struct BuildAccess;

}  // namespace detail
class FunctionBuilder;
class FunctionEditor;
class SSARewriter;
class ModuleBuilder;
class ModuleEditor;
class ProgramEditor;
class Call;

// Borrowed handles are valid while their ProgramBuilder state lives. No handle
// exposes an unchecked pointer or an implicit conversion to a host condition.
class SymbolHandle {
 public:
  std::string_view name () const;
  Result<const Symbol &> get () const;

 private:
  friend struct detail::BuildAccess;
  SymbolHandle (detail::BuildState *s, const Symbol *v) : state_ (s), symbol_ (v) {}
  detail::BuildState *state_;
  const Symbol *symbol_;
};
class VariableHandle {
 public:
  Result<const GlobalVariable &> get () const;
  operator SymbolHandle () const;

 private:
  friend struct detail::BuildAccess;
  VariableHandle (detail::BuildState *s, const GlobalVariable *v) : state_ (s), variable_ (v) {}
  detail::BuildState *state_;
  const GlobalVariable *variable_;
};
class DataHandle {
 public:
  operator SymbolHandle () const;
  Result<const GlobalData &> get () const;

 private:
  friend struct detail::BuildAccess;
  DataHandle (detail::BuildState *s, const GlobalData *v) : state_ (s), data_ (v) {}
  detail::BuildState *state_;
  const GlobalData *data_;
};
class BlockHandle {
 public:
  Result<const BasicBlock &> get () const;

 private:
  friend struct detail::BuildAccess;
  BlockHandle (detail::FunctionBuildState *s, BasicBlock *b)
    : function_ (s), block_ (b), revision_ (b ? b->function ().revisions ().instructions : 0) {}
  detail::FunctionBuildState *function_;
  BasicBlock *block_;
  std::uint64_t revision_;
};
class LabelHandle {
 public:
  Result<const Label &> get () const;

 private:
  friend struct detail::BuildAccess;
  LabelHandle (detail::FunctionBuildState *s, const Label *v)
    : function_ (s), label_ (v), revision_ (v ? v->function ().revisions ().instructions : 0) {}
  detail::FunctionBuildState *function_;
  const Label *label_;
  std::uint64_t revision_;
};
class ScopeHandle {
 public:
  Result<const StackScope &> get () const;

 private:
  friend struct detail::BuildAccess;
  ScopeHandle (detail::FunctionBuildState *s, const StackScope *v) : function_ (s), scope_ (v) {}
  detail::FunctionBuildState *function_;
  const StackScope *scope_;
};
class InstructionHandle {
 public:
  Result<const Instruction &> get () const;

 private:
  friend struct detail::BuildAccess;
  InstructionHandle (detail::FunctionBuildState *s, Instruction *v)
    : function_ (s),
      instruction_ (v),
      revision_ (v ? v->function ().revisions ().instructions : 0) {}
  detail::FunctionBuildState *function_;
  Instruction *instruction_;
  std::uint64_t revision_;
};

class ConstructionValue {
 public:
  bool is_valid () const;
  Type type () const;
  Result<Operand> get () const;
  ConstructionValue convert (Type) const;
  ConstructionValue operator- () const;
  ConstructionValue operator~() const;
  ConstructionValue operator!() const;
#define MIRNEXT_BUILD_OPERATOR(op, name)                                                          \
  friend ConstructionValue operator op (const ConstructionValue &a, const ConstructionValue &b) { \
    return a.apply (Operation::name, b);                                                          \
  }                                                                                               \
  template <class T>                                                                              \
    requires std::is_arithmetic_v<T>                                                              \
  friend ConstructionValue operator op (const ConstructionValue &a, T b) {                        \
    return a.apply (Operation::name, a.literal_like (Operand (b)));                               \
  }                                                                                               \
  template <class T>                                                                              \
    requires std::is_arithmetic_v<T>                                                              \
  friend ConstructionValue operator op (T a, const ConstructionValue &b) {                        \
    return b.literal_like (Operand (a)).apply (Operation::name, b);                               \
  }
  MIRNEXT_BUILD_OPERATOR (+, Add)
  MIRNEXT_BUILD_OPERATOR (-, Sub)
  MIRNEXT_BUILD_OPERATOR (*, Mul)
  MIRNEXT_BUILD_OPERATOR (/, Div)
  MIRNEXT_BUILD_OPERATOR (%, Mod)
  MIRNEXT_BUILD_OPERATOR (&, And)
  MIRNEXT_BUILD_OPERATOR (|, Or)
  MIRNEXT_BUILD_OPERATOR (^, Xor)
  MIRNEXT_BUILD_OPERATOR (<<, LeftShift)
  MIRNEXT_BUILD_OPERATOR (>>, RightShift)
  MIRNEXT_BUILD_OPERATOR (==, Equal)
  MIRNEXT_BUILD_OPERATOR (!=, NotEqual)
  MIRNEXT_BUILD_OPERATOR (<, Less)
  MIRNEXT_BUILD_OPERATOR (<=, LessEqual)
  MIRNEXT_BUILD_OPERATOR (>, Greater)
  MIRNEXT_BUILD_OPERATOR (>=, GreaterEqual)
#undef MIRNEXT_BUILD_OPERATOR
 private:
  friend class MIRBuilder;
  friend struct detail::NativeAccess;
  friend struct detail::BuildAccess;
  enum class Operation {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    And,
    Or,
    Xor,
    LeftShift,
    RightShift,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual
  };
  using Storage = std::variant<::mirnext::ir::detail::ExpressionState, const Register *>;
  ConstructionValue (detail::FunctionBuildState *s, Type t, std::optional<Storage> v)
    : function_ (s), type_ (t), value_ (std::move (v)) {}
  ConstructionValue apply (Operation, const ConstructionValue &) const;
  ConstructionValue literal_like (Operand) const;
  detail::FunctionBuildState *function_;
  Type type_;
  std::optional<Storage> value_;
};
class MemoryHandle {
 private:
  friend struct detail::BuildAccess;
  MemoryHandle (detail::FunctionBuildState *s, std::optional<Memory> v)
    : function_ (s), memory_ (std::move (v)) {}
  detail::FunctionBuildState *function_;
  std::optional<Memory> memory_;
};
class BlockInput {
 private:
  friend struct detail::BuildAccess;
  BlockInput (detail::FunctionBuildState *s,
              std::optional<::mirnext::ir::detail::BlockArgumentState> v)
    : function_ (s), block_ (std::move (v)) {}
  detail::FunctionBuildState *function_;
  std::optional<::mirnext::ir::detail::BlockArgumentState> block_;
};
class Call {
 public:
  ConstructionValue result (std::size_t) const;
  ConstructionValue value () const;
  operator ConstructionValue () const { return value (); }
  ConstructionValue convert (Type t) const { return value ().convert (t); }
  ConstructionValue operator- () const { return -value (); }
  ConstructionValue operator~() const { return ~value (); }
  ConstructionValue operator!() const { return !value (); }
#define MIRNEXT_CALL_OPERATOR(op)                                                    \
  friend ConstructionValue operator op (const Call &a, const Call &b) {              \
    return a.value () op b.value ();                                                 \
  }                                                                                  \
  friend ConstructionValue operator op (const Call &a, const ConstructionValue &b) { \
    return a.value () op b;                                                          \
  }                                                                                  \
  friend ConstructionValue operator op (const ConstructionValue &a, const Call &b) { \
    return a op b.value ();                                                          \
  }                                                                                  \
  template <class T>                                                                 \
    requires std::is_arithmetic_v<T>                                                 \
  friend ConstructionValue operator op (const Call &a, T b) {                        \
    return a.value () op b;                                                          \
  }                                                                                  \
  template <class T>                                                                 \
    requires std::is_arithmetic_v<T>                                                 \
  friend ConstructionValue operator op (T a, const Call &b) {                        \
    return a op b.value ();                                                          \
  }
  MIRNEXT_CALL_OPERATOR (+)
  MIRNEXT_CALL_OPERATOR (-)
  MIRNEXT_CALL_OPERATOR (*)
  MIRNEXT_CALL_OPERATOR (/)
  MIRNEXT_CALL_OPERATOR (%)
  MIRNEXT_CALL_OPERATOR (&)
  MIRNEXT_CALL_OPERATOR (|)
  MIRNEXT_CALL_OPERATOR (^)
  MIRNEXT_CALL_OPERATOR (<<)
  MIRNEXT_CALL_OPERATOR (>>)
  MIRNEXT_CALL_OPERATOR (==)
  MIRNEXT_CALL_OPERATOR (!=)
  MIRNEXT_CALL_OPERATOR (<)
  MIRNEXT_CALL_OPERATOR (<=)
  MIRNEXT_CALL_OPERATOR (>)
  MIRNEXT_CALL_OPERATOR (>=)
#undef MIRNEXT_CALL_OPERATOR
 private:
  friend struct detail::BuildAccess;
  Call (detail::FunctionBuildState *s, std::optional<::mirnext::ir::detail::CallResultState> v)
    : function_ (s), call_ (std::move (v)) {}
  detail::FunctionBuildState *function_;
  std::optional<::mirnext::ir::detail::CallResultState> call_;
};

// Inputs retain their typed handle until the receiving function checks ownership.
class BuildOperand {
 public:
  BuildOperand (Operand v) : value_ (std::move (v)) {}
  template <class T>
    requires std::is_arithmetic_v<T>
  BuildOperand (T v) : value_ (Operand (v)) {}
  BuildOperand (ConstructionValue v) : value_ (std::move (v)) {}
  BuildOperand (const Call &v) : value_ (v.value ()) {}
  BuildOperand (SymbolHandle v) : value_ (v) {}
  BuildOperand (const FunctionBuilder &);
  BuildOperand (LabelHandle v) : value_ (v) {}
  BuildOperand (MemoryHandle v) : value_ (std::move (v)) {}

 private:
  friend struct detail::BuildAccess;
  std::variant<Operand, ConstructionValue, SymbolHandle, LabelHandle, MemoryHandle> value_;
};
class BuildArgument {
 public:
  BuildArgument (BlockInput v) : value_ (std::move (v)) {}
  template <class T>
    requires std::is_constructible_v<BuildOperand, T>
  BuildArgument (T &&v) : value_ (BuildOperand (std::forward<T> (v))) {}

 private:
  friend struct detail::BuildAccess;
  std::variant<BuildOperand, BlockInput> value_;
};

class FunctionBuilder {
 public:
  operator SymbolHandle () const;
  SymbolHandle symbol () const;
  Result<const Function &> get () const;
  ConstructionValue arg (std::size_t) const;
  ConstructionValue literal (BuildOperand) const;
  template <class T>
    requires std::is_same_v<T, bool>
  ConstructionValue literal (T value) const {
    return literal (Type (Type::Kind::Bool), BuildOperand (value));
  }
  ConstructionValue literal (Type, BuildOperand) const;
  ConstructionValue local (Type, std::string_view name = {}, std::string_view hardware = {}) const;
  void assign (ConstructionValue destination, BuildOperand) const;
  BlockHandle entry () const;
  BlockHandle block (std::string_view name = {}) const;
  void position_at (BlockHandle) const;
  LabelHandle label (BlockHandle, std::string_view name = {}) const;
  void jump (BlockHandle) const;
  void jump (LabelHandle) const;
  void branch_if (BuildOperand, BlockHandle, bool when_true = true) const;
  void branch (Comparison, ConstructionValue, ConstructionValue, BlockHandle) const;
  void switch_to (BuildOperand, std::span<const BlockHandle>) const;
  void switch_to (BuildOperand v, std::initializer_list<BlockHandle> b) const {
    switch_to (std::move (v), std::span<const BlockHandle> (b.begin (), b.size ()));
  }
  InstructionHandle emit (Opcode, std::span<const BuildOperand>) const;
  InstructionHandle emit (Opcode op, std::initializer_list<BuildOperand> inputs) const {
    return emit (op, std::span<const BuildOperand> (inputs.begin (), inputs.size ()));
  }
  ConstructionValue unary (Opcode, BuildOperand, std::string_view name = {}) const;
  ConstructionValue binary (Opcode, BuildOperand, BuildOperand, std::string_view name = {}) const;
  Call call (SymbolHandle, std::span<const BuildArgument> = {}) const;
  Call call (SymbolHandle c, std::initializer_list<BuildArgument> args) const {
    return call (c, std::span<const BuildArgument> (args.begin (), args.size ()));
  }
  Call inline_call (SymbolHandle, std::span<const BuildArgument> = {}) const;
  Call inline_call (SymbolHandle c, std::initializer_list<BuildArgument> args) const {
    return inline_call (c, std::span<const BuildArgument> (args.begin (), args.size ()));
  }
  Call call_indirect (SymbolHandle signature, BuildOperand address,
                      std::span<const BuildArgument> = {}) const;
  Call call_indirect (SymbolHandle s, BuildOperand p,
                      std::initializer_list<BuildArgument> a) const {
    return call_indirect (s, std::move (p), std::span<const BuildArgument> (a.begin (), a.size ()));
  }
  void ret () const;
  void ret (BuildOperand) const;
  void ret (std::span<const BuildOperand>) const;
  void ret (std::initializer_list<BuildOperand> v) const {
    ret (std::span<const BuildOperand> (v.begin (), v.size ()));
  }
  MemoryHandle memory (Type, ConstructionValue base, std::int64_t displacement = 0,
                       std::optional<ConstructionValue> index = {}, unsigned scale = 1) const;
  ConstructionValue load (MemoryHandle, std::string_view name = {}) const;
  ConstructionValue load (VariableHandle, std::string_view name = {}) const;
  void store (MemoryHandle, BuildOperand) const;
  void store (VariableHandle, BuildOperand) const;
  ConstructionValue address_of (ConstructionValue, std::string_view name = {}) const;
  ConstructionValue address_of (SymbolHandle, std::int64_t displacement = 0) const;
  ConstructionValue address_of (VariableHandle) const;
  ConstructionValue address_of (LabelHandle) const;
  BlockInput block_arg (ConstructionValue address, Type, std::int64_t displacement = 0) const;
  BlockInput return_buffer (ConstructionValue address, std::size_t size) const;
  ConstructionValue allocate_stack (BuildOperand size) const;
  ConstructionValue stack_save () const;
  void stack_restore (ConstructionValue) const;
  ScopeHandle begin_stack_scope (std::string_view name = {}) const;
  void end_stack_scope (ScopeHandle) const;
  void start_varargs (BuildOperand) const;
  void end_varargs (BuildOperand) const;
  ConstructionValue next_vararg (Type, BuildOperand) const;
  ConstructionValue next_vararg_address (Type, BuildOperand) const;
  void next_vararg_block (ConstructionValue destination, BuildOperand list, Type) const;
  void jump_call (SymbolHandle) const;
  void jump_call (SymbolHandle, BuildOperand address) const;
  void ret_to (BuildOperand) const;

 private:
  friend struct detail::BuildAccess;
  friend class FunctionEditor;
  FunctionBuilder (detail::BuildState *s, detail::FunctionBuildState *f)
    : state_ (s), function_ (f) {}
  detail::BuildState *state_;
  detail::FunctionBuildState *function_;
};

// Initializers own constants and borrow typed handles from the same builder.
class DataInput {
 public:
  static DataInput values (Type type, std::span<const Operand> values) {
    return DataInput (InitializedData{type, {values.begin (), values.end ()}});
  }
  static DataInput values (Type type, std::initializer_list<Operand> values) {
    return DataInput::values (type, std::span<const Operand> (values.begin (), values.size ()));
  }
  static DataInput zero (std::size_t bytes) { return DataInput (ZeroData{bytes}); }
  static DataInput reference (SymbolHandle symbol, std::int64_t displacement = 0) {
    return DataInput (SymbolValue{symbol, displacement});
  }
  static DataInput label (LabelHandle label, std::int64_t displacement = 0) {
    return DataInput (LabelValue{label, {}, displacement});
  }
  static DataInput difference (LabelHandle label, LabelHandle base, std::int64_t displacement = 0) {
    return DataInput (LabelValue{label, base, displacement});
  }
  static DataInput expression (FunctionBuilder function) { return DataInput (function); }

 private:
  friend struct detail::BuildAccess;
  struct SymbolValue {
    SymbolHandle symbol;
    std::int64_t displacement;
  };
  struct LabelValue {
    LabelHandle label;
    std::optional<LabelHandle> base;
    std::int64_t displacement;
  };
  using Storage = std::variant<InitializedData, ZeroData, SymbolValue, LabelValue, FunctionBuilder>;
  explicit DataInput (Storage value) : value_ (std::move (value)) {}
  Storage value_;
};

class ModuleBuilder {
 public:
  Result<const Module &> get () const;
  FunctionBuilder function (std::string_view, Signature,
                            std::span<const std::string_view> argument_names = {}) const;
  template <BuilderSignature T>
  FunctionBuilder function (std::string_view name) const {
    return function (name, signature<T> ());
  }
  SymbolHandle declare_function (std::string_view, Signature) const;
  FunctionBuilder define_function (SymbolHandle) const;
  SymbolHandle import (std::string_view, Signature) const;
  SymbolHandle prototype (std::string_view, Signature) const;
  template <BuilderSignature T>
  SymbolHandle import (std::string_view n) const {
    return import (n, signature<T> ());
  }
  template <BuilderSignature T>
  SymbolHandle declare_function (std::string_view n) const {
    return declare_function (n, signature<T> ());
  }
  template <BuilderSignature T>
  SymbolHandle prototype (std::string_view n) const {
    return prototype (n, signature<T> ());
  }
  VariableHandle variable (std::string_view, Type) const;
  VariableHandle variable (std::string_view, Type, Operand initial, bool writable = true) const;
  VariableHandle variable (std::string_view, Type, SymbolHandle initial,
                           bool writable = true) const;
  template <detail::SignatureScalar T>
  VariableHandle variable (std::string_view n) const {
    return variable (n, detail::scalar_type<T> ());
  }
  template <detail::SignatureScalar T>
  VariableHandle variable (std::string_view n, T v, bool writable = true) const {
    if constexpr (std::is_pointer_v<T>)
      return variable (n, Type::pointer (),
                       Operand (static_cast<std::uint64_t> (reinterpret_cast<std::uintptr_t> (v))),
                       writable);
    else
      return variable (n, detail::scalar_type<T> (), Operand (v), writable);
  }
  SymbolHandle forward (std::string_view) const;
  SymbolHandle import (std::string_view) const;
  SymbolHandle declare_data (std::string_view) const;
  DataHandle data (std::string_view, DataInput, bool writable = true) const;
  DataHandle anonymous_data (DataInput, bool writable = true) const;
  DataHandle define_data (SymbolHandle, DataInput, bool writable = true) const;
  VariableHandle bind_variable (SymbolHandle, Type, bool writable = true) const;
  VariableHandle variable (std::string_view, FunctionBuilder initializer,
                           bool writable = true) const;
  void rename_symbol (SymbolHandle, std::string_view) const;
  VariableHandle import_variable (std::string_view, Type, bool writable = true) const;
  void export_symbol (SymbolHandle) const;

 private:
  friend struct detail::BuildAccess;
  friend class ModuleEditor;
  ModuleBuilder (detail::BuildState *s, Module *m) : state_ (s), module_ (m) {}
  detail::BuildState *state_;
  Module *module_;
};

// An editor borrows its program. All nested handles share one first-error boundary.
// Successful edits remain on failure; finish verifies the batch before further stages.
class FunctionEditor : public FunctionBuilder {
 public:
  SSARewriter ssa () const;
  SSARewriter create_ssa () const;
  Result<const Function &> get () const;
  InstructionHandle instruction (const Instruction &) const;
  void replace (const Instruction &, Opcode, std::span<const BuildOperand>) const;
  void replace (const Instruction &i, Opcode op,
                std::initializer_list<BuildOperand> operands) const {
    replace (i, op, std::span<const BuildOperand> (operands.begin (), operands.size ()));
  }
  void replace_operand (const Instruction &, std::size_t, BuildOperand) const;
  void erase (const Instruction &) const;
  InstructionHandle insert_before (const Instruction &, Opcode,
                                   std::span<const BuildOperand>) const;
  InstructionHandle insert_before (const Instruction &i, Opcode op,
                                   std::initializer_list<BuildOperand> operands) const {
    return insert_before (i, op,
                          std::span<const BuildOperand> (operands.begin (), operands.size ()));
  }
  void move_before (const Instruction &, BlockHandle,
                    OptionalRef<const Instruction> before = {}) const;
  void replace (InstructionHandle, Opcode, std::span<const BuildOperand>) const;
  void replace (InstructionHandle instruction, Opcode opcode,
                std::initializer_list<BuildOperand> operands) const {
    replace (instruction, opcode,
             std::span<const BuildOperand> (operands.begin (), operands.size ()));
  }
  void replace_operand (InstructionHandle, std::size_t, BuildOperand) const;
  void erase (InstructionHandle) const;
  InstructionHandle insert_before (InstructionHandle, Opcode, std::span<const BuildOperand>) const;
  InstructionHandle insert_before (InstructionHandle instruction, Opcode opcode,
                                   std::initializer_list<BuildOperand> operands) const {
    return insert_before (instruction, opcode,
                          std::span<const BuildOperand> (operands.begin (), operands.size ()));
  }
  void move_before (InstructionHandle, BlockHandle,
                    OptionalRef<const Instruction> before = {}) const;
  void replace_uses (ConstructionValue from, ConstructionValue to) const;
  ConstructionValue value (const Register &) const;
  BlockHandle bind_block (const BasicBlock &) const;
  BlockHandle split_before (const Instruction &, std::string_view name = {}) const;
  void merge_fallthrough (BlockHandle) const;
  void move_block_before (BlockHandle, std::optional<BlockHandle> before = {}) const;
  std::size_t separate_label_anchors (BlockHandle) const;
  void erase_blocks (std::span<const BlockHandle>) const;
  std::size_t erase_unused_labels () const;
  void set_stack_scope (BlockHandle, std::optional<ScopeHandle> = {}) const;
  void materialize_register_storage (std::span<const ConstructionValue>) const;
  void set_memory_location (const Instruction &, std::size_t,
                            OptionalRef<const MemoryLocation>) const;
  MemoryHandle literal_memory (const GlobalData &, ConstructionValue address) const;

 private:
  const Instruction *resolve (InstructionHandle, std::string_view) const;
  friend struct detail::BuildAccess;
  explicit FunctionEditor (FunctionBuilder builder) : FunctionBuilder (builder) {}
};
class ModuleEditor : public ModuleBuilder {
 public:
  using ModuleBuilder::data;
  using ModuleBuilder::variable;
  SymbolHandle symbol (std::string_view) const;
  DataHandle data (std::string_view) const;
  VariableHandle variable (std::string_view) const;
  FunctionEditor edit (std::string_view name) const;
  FunctionEditor edit (const Function &) const;

 private:
  friend struct detail::BuildAccess;
  explicit ModuleEditor (ModuleBuilder builder) : ModuleBuilder (builder) {}
};
class ProgramEditor {
  friend class MIRBuilder;
  friend struct detail::NativeAccess;

 public:
  explicit ProgramEditor (Program &);
  ~ProgramEditor ();
  ProgramEditor (ProgramEditor &&) noexcept;
  ProgramEditor &operator= (ProgramEditor &&) noexcept;
  ProgramEditor (const ProgramEditor &) = delete;
  ProgramEditor &operator= (const ProgramEditor &) = delete;
  ModuleEditor module (std::string_view);
  FunctionEditor edit (const Function &);
  OptionalRef<const Error> error () const;
  Result<void> finish ();

 private:
  std::unique_ptr<detail::BuildState> state_;
};

class ProgramBuilder {
 public:
  ProgramBuilder ();
  ~ProgramBuilder ();
  ProgramBuilder (ProgramBuilder &&) noexcept;
  // Assignment invalidates handles borrowing the previous state.
  ProgramBuilder &operator= (ProgramBuilder &&) noexcept;
  ProgramBuilder (const ProgramBuilder &) = delete;
  ProgramBuilder &operator= (const ProgramBuilder &) = delete;
  ModuleBuilder module (std::string_view);
  OptionalRef<const Error> error () const;
  Result<Program> finish ();

 private:
  std::unique_ptr<detail::BuildState> state_;
};
}  // namespace mirnext::ir
