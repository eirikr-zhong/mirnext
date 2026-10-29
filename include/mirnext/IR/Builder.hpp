/* Licensed under the MIT License. Explicit construction of canonical MIR objects. */
#pragma once
#include "mirnext/IR/IR.hpp"

namespace mirnext::ir {
namespace detail {
struct NativeAccess;
template <class T>
concept SignatureScalar = (std::is_integral_v<T> && sizeof (T) <= 8) || std::is_floating_point_v<T>
                          || (std::is_pointer_v<T>
                              && (std::is_object_v<std::remove_pointer_t<T>>
                                  || std::is_void_v<std::remove_pointer_t<T>>) );
template <SignatureScalar T>
constexpr Type scalar_type () {
  using U = std::remove_cv_t<T>;
  if constexpr (std::is_same_v<U, bool>)
    return Type (Type::Kind::Bool);
  else if constexpr (std::is_pointer_v<U>)
    return Type::pointer ();
  else if constexpr (std::is_same_v<U, float>)
    return Type (Type::Kind::F32);
  else if constexpr (std::is_same_v<U, double>)
    return Type (Type::Kind::F64);
  else if constexpr (std::is_same_v<U, long double>)
    return Type (Type::Kind::LongDouble);
  else {
    constexpr auto first = std::is_signed_v<U> ? Type::Kind::I8 : Type::Kind::U8;
    constexpr auto offset = sizeof (U) == 1 ? 0 : sizeof (U) == 2 ? 2 : sizeof (U) == 4 ? 4 : 6;
    return Type (static_cast<Type::Kind> (static_cast<unsigned> (first) + offset));
  }
}
template <class T>
struct SignatureTraits {
  static constexpr bool supported = false;
};
template <class R, class... A>
struct SignatureTraits<R (A...)> {
  static constexpr bool supported
    = (std::is_void_v<R> || SignatureScalar<R>) && (SignatureScalar<A> && ...);
  static Signature get ()
    requires supported
  {
    Signature result;
    if constexpr (!std::is_void_v<R>) result.results.push_back (scalar_type<R> ());
    result.parameters = {scalar_type<A> ()...};
    return result;
  }
};
}  // namespace detail
template <class T>
concept BuilderSignature = detail::SignatureTraits<T>::supported;
template <BuilderSignature T>
Signature signature () {
  return detail::SignatureTraits<T>::get ();
}

// Values borrow the canonical function, not the builder. No expression operators
// or implicit conversion to a C++ condition are provided.
class Value {
 public:
  Value () = default;
  bool is_valid () const { return operand_.has_value (); }
  Type type () const { return type_; }
  Result<const Operand &> operand () const;

 private:
  friend class CallResult;
  friend struct detail::NativeAccess;
  friend class MIRBuilder;
  Value (Type type, Operand operand, const Function *owner = nullptr)
    : type_ (type), operand_ (std::move (operand)), owner_ (owner) {}
  Type type_ = Type::i64 ();
  std::optional<Operand> operand_;
  const Function *owner_ = nullptr;
};
class CallResult {
 public:
  bool is_valid () const { return instruction_ != nullptr; }
  std::size_t result_count () const { return count_; }
  const Instruction *instruction () const { return instruction_; }
  Value result (std::size_t) const;
  Value value () const;

 private:
  friend struct detail::NativeAccess;
  friend class MIRBuilder;
  const Instruction *instruction_ = nullptr;
  std::size_t count_ = 0;
};
class CallArgument {
 public:
  CallArgument (Value value) : value_ (std::move (value)) {}

 private:
  friend class MIRBuilder;
  friend struct detail::NativeAccess;
  explicit CallArgument (Memory memory) : value_ (std::move (memory)) {}
  std::variant<Value, Memory> value_;
};
enum class VectorHalf { Low, High };
class SSARewriter;
// A batch owns insertion state and one diagnostic, never a second IR body.
// Creation returns ordinary borrowed node pointers. After failure, later calls
// are safe and perform no writes. finish() validates and closes the batch.
class MIRBuilder {
 public:
  explicit MIRBuilder (Program &);
  explicit MIRBuilder (Function &);
  explicit MIRBuilder (BasicBlock &);
  ~MIRBuilder ();
  MIRBuilder (MIRBuilder &&) noexcept;
  MIRBuilder &operator= (MIRBuilder &&) noexcept;
  MIRBuilder (const MIRBuilder &) = delete;
  MIRBuilder &operator= (const MIRBuilder &) = delete;
  Result<void> finish ();
  Result<void> status () const;
  OptionalRef<const Error> error () const;
  Program &program () const;
  Function *function () const;
  BasicBlock *insert_block () const;
  void set_function (const Function *);
  void set_insert_point (const BasicBlock *);
  void clear_insert_point ();

  Module *create_module (std::string_view);
  Function *create_function (Module *, std::string_view, Signature,
                             std::span<const std::string_view> argument_names = {});
  template <BuilderSignature T>
  Function *create_function (Module *module, std::string_view name) {
    return create_function (module, name, signature<T> ());
  }
  Function *define_function (const Symbol *);
  const Symbol *declare_function (Module *, std::string_view, Signature);
  const Symbol *create_import (Module *, std::string_view, std::optional<Signature> = {});
  const Symbol *create_prototype (Module *, std::string_view, Signature);
  template <BuilderSignature T>
  const Symbol *create_import (Module *m, std::string_view n) {
    return create_import (m, n, signature<T> ());
  }
  template <BuilderSignature T>
  const Symbol *declare_function (Module *m, std::string_view n) {
    return declare_function (m, n, signature<T> ());
  }
  const Symbol *create_forward (Module *, std::string_view);
  const Symbol *declare_data (Module *, std::string_view);
  const GlobalData *create_data (Module *, std::string_view, DataInitializer, bool writable = true);
  const GlobalData *create_anonymous_data (Module *, DataInitializer, bool writable = true);
  const GlobalData *define_data (const Symbol *, DataInitializer, bool writable = true);
  const GlobalVariable *create_variable (Module *, std::string_view, Type,
                                         std::optional<Operand> initial = {}, bool writable = true);
  const GlobalVariable *create_expression_variable (Module *, std::string_view,
                                                    const Function *initializer,
                                                    bool writable = true);
  template <detail::SignatureScalar T>
  const GlobalVariable *create_variable (Module *m, std::string_view n, T initial,
                                         bool writable = true) {
    if constexpr (std::is_pointer_v<T>)
      return create_variable (m, n, Type::pointer (),
                              Operand (static_cast<std::uint64_t> (
                                reinterpret_cast<std::uintptr_t> (initial))),
                              writable);
    else
      return create_variable (m, n, detail::scalar_type<T> (), Operand (initial), writable);
  }
  const GlobalVariable *bind_variable (const Symbol *, Type, bool writable = true);
  const GlobalVariable *create_import_variable (Module *, std::string_view, Type,
                                                bool writable = true);
  void export_symbol (const Symbol *);
  void export_symbol (const Function *f) { export_symbol (f ? &f->symbol () : nullptr); }
  void export_symbol (const GlobalVariable *v) { export_symbol (v ? &v->symbol () : nullptr); }
  void export_symbol (const GlobalData *d) { export_symbol (d ? &d->symbol () : nullptr); }
  void rename_symbol (const Symbol *, std::string_view);

  Value argument (std::size_t) const;
  Value value (const Register &) const;
  Value constant (Type, Operand) const;
  template <detail::SignatureScalar T>
  Value constant (T v) const {
    if constexpr (std::is_pointer_v<T>)
      return constant (Type::pointer (),
                       Operand (static_cast<std::uint64_t> (reinterpret_cast<std::uintptr_t> (v))));
    else
      return constant (detail::scalar_type<T> (), Operand (v));
  }
  Value constant_i64 (std::int64_t v) const { return constant (Type::i64 (), Operand (v)); }
  Value create_register (Type, std::string_view name = {}, std::string_view hardware = {});
  BasicBlock *create_block (std::string_view name = {});
  const Label *create_label (const BasicBlock *, std::string_view name = {});
  const Instruction *create_instruction (Opcode, std::span<const Operand>);
  const Instruction *create_instruction (Opcode op, std::initializer_list<Operand> operands) {
    return create_instruction (op, std::span<const Operand> (operands.begin (), operands.size ()));
  }
  Value create_unary (Opcode, Value, std::string_view name = {});
  Value constant_vector (Type, std::span<const std::byte>) const;
  Value create_vector_splat (Type, Value);
  Value create_extract_lane (Value, unsigned lane);
  Value create_insert_lane (Value, Value element, unsigned lane);
  Value create_shuffle (Value, Value, std::span<const unsigned> lanes);
  Value create_shuffle (Value a, Value b, std::initializer_list<unsigned> lanes) {
    return create_shuffle (a, b, std::span (lanes));
  }
  Value create_abs (Value);
  Value create_sqrt (Value);
  Value create_popcount (Value);
  Value create_ceil (Value);
  Value create_floor (Value);
  Value create_trunc (Value);
  Value create_nearest (Value);
  Value create_any_true (Value);
  Value create_all_true (Value);
  Value create_bitmask (Value);
  Value create_min (Value, Value);
  Value create_max (Value, Value);
  Value create_pmin (Value, Value);
  Value create_pmax (Value, Value);
  Value create_add_sat (Value, Value);
  Value create_sub_sat (Value, Value);
  Value create_avgr (Value, Value);
  Value create_andnot (Value, Value);
  Value create_bitcast (Value, Type);
  Value create_bitselect (Value mask, Value if_true, Value if_false);
  Value create_swizzle (Value, Value indices);
  Value create_extend (Value, VectorHalf);
  Value create_extmul (Value, Value, VectorHalf);
  Value create_extadd_pairwise (Value);
  Value create_narrow_sat (Value, Value, Type);
  Value create_dot (Value, Value);
  Value create_q15mulr_sat (Value, Value);
  Value create_convert_low (Value, Type);
  Value create_convert_zero (Value, Type);
  Value create_trunc_sat (Value, Type);
  Value create_trunc_sat_zero (Value, Type);
  Value create_load_extend (Type, Value address, std::int64_t displacement = 0);
  Value create_load_splat (Type, Memory);
  Value create_load_zero (Type, Memory);
  Value create_load_lane (Value, Memory, unsigned lane);
  void create_store_lane (Memory, Value, unsigned lane);
  Value create_binary (Opcode, Value, Value, std::string_view name = {});
#define MIRNEXT_EXPLICIT_BINARY(name) Value create_##name (Value, Value);
  MIRNEXT_EXPLICIT_BINARY (add)
  MIRNEXT_EXPLICIT_BINARY (sub)
  MIRNEXT_EXPLICIT_BINARY (mul)
  MIRNEXT_EXPLICIT_BINARY (div)
  MIRNEXT_EXPLICIT_BINARY (mod)
  MIRNEXT_EXPLICIT_BINARY (and)
  MIRNEXT_EXPLICIT_BINARY (or)
  MIRNEXT_EXPLICIT_BINARY (xor)
  MIRNEXT_EXPLICIT_BINARY (shl)
  MIRNEXT_EXPLICIT_BINARY (shr)
  MIRNEXT_EXPLICIT_BINARY (eq)
  MIRNEXT_EXPLICIT_BINARY (ne)
  MIRNEXT_EXPLICIT_BINARY (lt)
  MIRNEXT_EXPLICIT_BINARY (le)
  MIRNEXT_EXPLICIT_BINARY (gt)
  MIRNEXT_EXPLICIT_BINARY (ge)
#undef MIRNEXT_EXPLICIT_BINARY
  Value create_neg (Value);
  Value create_bit_not (Value);
  Value create_logical_not (Value);
  Value create_convert (Value, Type);
  void create_assign (Value destination, Value);
  void create_return ();
  void create_return (Value);
  void create_return (std::span<const Value>);
  void create_return (std::initializer_list<Value> values) { create_return (std::span (values)); }
  void create_jump (const BasicBlock *);
  void create_jump (const Label *);
  void create_cond_branch (Value, const BasicBlock *, bool when_true = true);
  void create_compare_branch (Comparison, Value, Value, const BasicBlock *);
  void create_switch (Value, std::span<const BasicBlock *const>);
  void create_switch (Value v, std::initializer_list<const BasicBlock *> targets) {
    create_switch (v, std::span (targets));
  }
  CallResult create_call (const Symbol *, std::span<const CallArgument> = {});
  CallResult create_call (const Function *, std::span<const CallArgument> = {});
  CallResult create_inline_call (const Symbol *, std::span<const CallArgument> = {});
  CallResult create_inline_call (const Function *, std::span<const CallArgument> = {});
  CallResult create_indirect_call (const Symbol *, Value, std::span<const CallArgument> = {});
#define MIRNEXT_EXPLICIT_CALL_LIST(name, type)                              \
  CallResult name (type callee, std::initializer_list<CallArgument> args) { \
    return name (callee, std::span (args));                                 \
  }
  MIRNEXT_EXPLICIT_CALL_LIST (create_call, const Symbol *)
  MIRNEXT_EXPLICIT_CALL_LIST (create_call, const Function *)
  MIRNEXT_EXPLICIT_CALL_LIST (create_inline_call, const Symbol *)
  MIRNEXT_EXPLICIT_CALL_LIST (create_inline_call, const Function *)
#undef MIRNEXT_EXPLICIT_CALL_LIST
  CallResult create_indirect_call (const Symbol *s, Value v,
                                   std::initializer_list<CallArgument> args) {
    return create_indirect_call (s, v, std::span (args));
  }
  CallResult create_indirect_call (const Function *f, Value address,
                                   std::span<const CallArgument> args = {}) {
    return create_indirect_call (f ? &f->symbol () : nullptr, address, args);
  }
  CallResult create_indirect_call (const Function *f, Value address,
                                   std::initializer_list<CallArgument> args) {
    return create_indirect_call (f, address, std::span (args));
  }
  void create_jump_call (const Symbol *);
  void create_jump_call (const Symbol *, Value);
  void create_return_to (Value);
  Memory memory (Type, Value, std::int64_t displacement = 0, std::optional<Value> index = {},
                 unsigned scale = 1);
  Value create_load (Memory, std::string_view name = {});
  Value create_load (const GlobalVariable *, std::string_view name = {});
  void create_store (Memory, Value);
  void create_store (const GlobalVariable *, Value);
  Value create_address_of (Value, std::string_view name = {});
  Value create_address_of (const Symbol *, std::int64_t displacement = 0);
  Value create_address_of (const Function *, std::int64_t displacement = 0);
  Value create_address_of (const GlobalVariable *);
  Value create_address_of (const GlobalData *d, std::int64_t displacement = 0) {
    return create_address_of (d ? &d->symbol () : nullptr, displacement);
  }
  Value create_address_of (const Label *);
  CallArgument block_argument (Value address, Type, std::int64_t displacement = 0);
  CallArgument return_buffer (Value address, std::size_t size);
  Value create_stack_alloc (Value);
  Value create_stack_save ();
  void create_stack_restore (Value);
  const StackScope *begin_stack_scope (std::string_view name = {});
  void end_stack_scope (const StackScope *);
  void create_va_start (Value);
  void create_va_end (Value);
  Value create_va_arg (Type, Value);
  Value create_va_address (Type, Value);
  void create_va_block (Value destination, Value list, Type);

  void replace_instruction (const Instruction *, Opcode, std::span<const Operand>);
  void replace_instruction (const Instruction *i, Opcode op,
                            std::initializer_list<Operand> values) {
    replace_instruction (i, op, std::span (values));
  }
  void replace_operand (const Instruction *, std::size_t, Operand);
  void erase_instruction (const Instruction *);
  const Instruction *insert_before (const Instruction *, Opcode, std::span<const Operand>);
  const Instruction *insert_before (const Instruction *i, Opcode op,
                                    std::initializer_list<Operand> values) {
    return insert_before (i, op, std::span (values));
  }
  void move_before (const Instruction *, const BasicBlock *, const Instruction *before = nullptr);
  void replace_uses (Value from, Value to);
  BasicBlock *split_before (const Instruction *, std::string_view name = {});
  void merge_fallthrough (const BasicBlock *);
  void move_block_before (const BasicBlock *, const BasicBlock *before = nullptr);
  std::size_t separate_label_anchors (const BasicBlock *);
  void erase_blocks (std::span<const BasicBlock *const>);
  std::size_t erase_unused_labels ();
  void set_stack_scope (const BasicBlock *, const StackScope * = nullptr);
  void materialize_register_storage (std::span<const Value>);
  void set_memory_location (const Instruction *, std::size_t, OptionalRef<const MemoryLocation>);
  Memory literal_memory (const GlobalData *, Value address);
  SSARewriter ssa ();
  SSARewriter create_ssa ();

 private:
  Value create_vector_operation (Opcode, Type, std::span<const Value>);
  friend struct detail::NativeAccess;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace mirnext::ir
