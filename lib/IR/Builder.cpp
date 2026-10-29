#include "mirnext/IR/Vector.hpp"
/* Licensed under the MIT License. Thin handles over the canonical IR builder. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/Construction.hpp"
#include "mirnext/IR/CheckedSSA.hpp"
#include <algorithm>
#include <array>

namespace mirnext::ir {
namespace detail {
struct FunctionBuildState {
  BuildState *program;
  Function *function;
  BasicBlock *entry;
  InstructionBuilder builder;
  std::unique_ptr<SSAForm> pending_ssa;
  std::string location;
  std::unordered_map<const BasicBlock *, const Label *> anchors;
  std::unordered_map<const void *, std::uint64_t> erased;
  FunctionBuildState (BuildState &p, Function &f, BasicBlock &b)
    : program (&p),
      function (&f),
      entry (&b),
      builder (b),
      location (std::string (f.module ().name ()) + "/" + std::string (f.symbol ().name ())) {}
};
struct BuildState {
  std::optional<Program> owned;
  ProgramStorage *storage;
  bool editing = false;
  BuildState () : owned (std::in_place), storage (owned->storage_.get ()) {}
  explicit BuildState (Program &program) : storage (program.storage_.get ()), editing (true) {
    if (!storage) {
      error = Error{ErrorCode::InvalidArgument, "cannot edit a moved-from program"};
      return;
    }
    ++storage->pending_edits;
    if (storage->edit_error) error = storage->edit_error;
  }
  Program &context () const { return *storage->owner; }
  static void value_error (const Instruction *instruction, std::string_view operation,
                           std::string_view reason) {
    if (!instruction) return;
    auto &function = instruction->function ();
    auto &program = function.module ().program ();
    if (program.storage_ && !program.storage_->edit_error)
      program.storage_->edit_error
        = Error{ErrorCode::InvalidOperand, std::string (function.module ().name ()) + "/"
                                             + std::string (function.symbol ().name ()) + " "
                                             + std::string (operation) + ": "
                                             + std::string (reason)};
  }
  void close () {
    if (editing && storage && !closed) --storage->pending_edits;
    closed = true;
  }
  ~BuildState () {
    if (editing && !closed) {
      fail ("edit", {ErrorCode::InvalidArgument, "edit batch was not finished"});
      close ();
    }
  }
  std::vector<std::unique_ptr<FunctionBuildState>> functions;
  std::vector<Module *> edited_modules;
  std::optional<Error> error;
  bool closed = false;
  void fail (std::string_view where, Error e) {
    if (!error) {
      e.message = std::string (where) + ": " + e.message;
      error = std::move (e);
      if (editing && storage && !storage->edit_error) storage->edit_error = error;
    }
  }
  bool ready (std::string_view where) {
    if (editing && storage && storage->edit_error && !error) error = storage->edit_error;
    if (closed) fail (where, {ErrorCode::InvalidArgument, "construction already finished"});
    return !closed && !error;
  }
};
struct BuildAccess {
  static SSARewriter ssa (FunctionBuildState *f) { return SSARewriter (f); }
  static FunctionEditor editor (FunctionBuilder b) { return FunctionEditor (b); }
  static ModuleEditor editor (ModuleBuilder b) { return ModuleEditor (b); }
  static FunctionBuildState *owner (const FunctionEditor &v) { return v.function_; }
  static DataHandle data (BuildState *p, const GlobalData *d = nullptr) { return {p, d}; }
  static const auto &raw (const DataInput &v) { return v.value_; }
  static FunctionBuildState *owner (const FunctionBuilder &v) { return v.function_; }
  static ConstructionValue value (FunctionBuildState *f) { return {f, Type::i64 (), {}}; }
  static ConstructionValue value (FunctionBuildState *f, detail::ExpressionState e) {
    return {f, e.type (),
            e.is_valid () ? std::optional<ConstructionValue::Storage> (std::move (e))
                          : std::nullopt};
  }
  static ConstructionValue value (FunctionBuildState *f, const Register *r) {
    if (!r) return value (f);
    // Block parameters are address values, but are not scalar expressions.
    if (r->type ().is_block ()) return {f, r->type (), ConstructionValue::Storage (r)};
    return value (f, f->builder.expression (*r));
  }
  static SymbolHandle symbol (BuildState *p, const Symbol *s = nullptr) { return {p, s}; }
  static VariableHandle variable (BuildState *p, const GlobalVariable *v = nullptr) {
    return {p, v};
  }
  static ModuleBuilder module (BuildState *p, Module *m = nullptr) {
    if (p && p->editing && m
        && std::find (p->edited_modules.begin (), p->edited_modules.end (), m)
             == p->edited_modules.end ())
      p->edited_modules.push_back (m);
    return {p, m};
  }
  static FunctionBuilder function (BuildState *p, FunctionBuildState *f = nullptr) {
    return {p, f};
  }
  static BlockHandle block (FunctionBuildState *f, BasicBlock *b = nullptr) {
    if (f && b) detail::access (*f->function).track_handles ();
    return {f, b};
  }
  static LabelHandle label (FunctionBuildState *f, const Label *l = nullptr) {
    if (f && l) detail::access (*f->function).track_handles ();
    return {f, l};
  }
  static ScopeHandle scope (FunctionBuildState *f, const StackScope *s = nullptr) { return {f, s}; }
  static MemoryHandle memory (FunctionBuildState *f, std::optional<Memory> m = {}) {
    return {f, std::move (m)};
  }
  static BlockInput block_input (FunctionBuildState *f,
                                 std::optional<detail::BlockArgumentState> b = {}) {
    return {f, std::move (b)};
  }
  static InstructionHandle instruction (FunctionBuildState *f, Instruction *i = nullptr) {
    if (f && i) detail::access (*f->function).track_handles ();
    return {f, i};
  }
  static const auto &raw (const Call &v) { return v.call_; }
  static Call call (FunctionBuildState *f, std::optional<detail::CallResultState> c = {}) {
    return {f, std::move (c)};
  }
  static bool present (const ConstructionValue &v) { return v.value_.has_value (); }
  static const detail::ExpressionState *scalar (const ConstructionValue &v) {
    return v.value_ ? std::get_if<detail::ExpressionState> (&*v.value_) : nullptr;
  }
  static std::optional<Operand> raw (const ConstructionValue &v) {
    if (!v.value_) return {};
    if (auto e = scalar (v)) {
      auto result = e->value ();
      return (result).has_value () ? std::optional<Operand> (std::move ((result).value ()))
                                   : std::nullopt;
    }
    return Operand (*std::get<const Register *> (*v.value_));
  }
  static FunctionBuildState *owner (const ConstructionValue &v) { return v.function_; }
  static FunctionBuildState *owner (BlockHandle v) { return v.function_; }
  static FunctionBuildState *owner (LabelHandle v) { return v.function_; }
  static FunctionBuildState *owner (ScopeHandle v) { return v.function_; }
  static FunctionBuildState *owner (const MemoryHandle &v) { return v.function_; }
  static const Symbol *raw (SymbolHandle v) { return v.symbol_; }
  static const GlobalVariable *raw (VariableHandle v) { return v.variable_; }
  static BuildState *owner (SymbolHandle v) { return v.state_; }
  static BuildState *owner (VariableHandle v) { return v.state_; }
  static BasicBlock *raw (BlockHandle v) { return v.block_; }
  static const Label *raw (LabelHandle v) { return v.label_; }
  static const StackScope *raw (ScopeHandle v) { return v.scope_; }
  static const auto &raw (const MemoryHandle &v) { return v.memory_; }
  static const auto &raw (const BuildOperand &v) { return v.value_; }
  static const auto &raw (const BuildArgument &v) { return v.value_; }
  static const auto &raw (const BlockInput &v) { return v.block_; }
  static FunctionBuildState *owner (const BlockInput &v) { return v.function_; }
};
}  // namespace detail
namespace {
using detail::BuildState;
using F = detail::FunctionBuildState;
using A = detail::BuildAccess;
Error invalid (std::string text) { return {ErrorCode::InvalidOperand, std::move (text)}; }
bool ready (F *f, std::string_view operation) {
  if (!f) return false;
  if (f->program->editing && f->program->storage && f->program->storage->edit_error
      && !f->program->error)
    f->program->error = f->program->storage->edit_error;
  if (f->program->closed)
    f->program->fail (f->location + ": " + std::string (operation),
                      {ErrorCode::InvalidArgument, "construction already finished"});
  return !f->program->closed && !f->program->error;
}
void fail (F *f, std::string_view op, Error error) {
  if (f) f->program->fail (f->location + ": " + std::string (op), std::move (error));
}
void fail (F *f, std::string_view op, std::string text) {
  fail (f, op, invalid (std::move (text)));
}
template <class T>
T *take (F *f, std::string_view op, Result<T &> result) {
  if (!result.has_value ()) {
    fail (f, op, result.error ());
    return nullptr;
  }
  return &result.value ();
}
template <class T>
void check (F *f, std::string_view op, Result<T> result) {
  if (!result.has_value ()) fail (f, op, result.error ());
}
template <class T>
T *take (BuildState *p, std::string_view op, Result<T &> result) {
  if (!result.has_value ()) {
    p->fail (op, result.error ());
    return nullptr;
  }
  return &result.value ();
}
// Most instructions and calls have at most four inputs. Keep the adapter's
// conversion scratch space on the stack; the core owns the final operands.
template <class T>
class InputBuffer {
 public:
  explicit InputBuffer (std::size_t size) : InputBuffer (size, T (0)) {}
  InputBuffer (std::size_t size, const T &initial)
    : local_{initial, initial, initial, initial}, large_ (size > local_.size ()) {
    if (large_) heap_.reserve (size);
  }
  template <class U>
  void push_back (U &&v) {
    if (large_)
      heap_.emplace_back (std::forward<U> (v));
    else
      local_[size_++] = T (std::forward<U> (v));
  }
  std::span<const T> view () const {
    return large_ ? std::span<const T> (heap_) : std::span<const T> (local_.data (), size_);
  }

 private:
  std::array<T, 4> local_;
  std::vector<T> heap_;
  std::size_t size_ = 0;
  bool large_;
};
bool owns (F *f, const ConstructionValue &v, std::string_view op) {
  if (!ready (f, op)) return false;
  if (A::owner (v) != f || !A::present (v)) {
    fail (f, op, "value belongs to another function or is invalid");
    return false;
  }
  return true;
}
template <class Handle>
bool owns (F *f, const Handle &v, std::string_view op) {
  if (!ready (f, op)) return false;
  if (A::owner (v) != f || !A::raw (v)) {
    fail (f, op, "handle belongs to another function or is invalid");
    return false;
  }
  if constexpr (requires { v.get (); }) {
    auto valid = v.get ();
    if (!valid.has_value ()) {
      fail (f, op, valid.error ());
      return false;
    }
  }
  return true;
}
bool owns (F *f, SymbolHandle s, std::string_view op) {
  if (!ready (f, op)) return false;
  if (A::owner (s) != f->program || !A::raw (s)) {
    fail (f, op, "symbol belongs to another program or is invalid");
    return false;
  }
  return true;
}
bool owns (F *f, VariableHandle v, std::string_view op) {
  if (!ready (f, op)) return false;
  if (A::owner (v) != f->program || !A::raw (v)) {
    fail (f, op, "variable belongs to another program or is invalid");
    return false;
  }
  return true;
}
std::optional<Operand> operand (F *f, const BuildOperand &input, std::string_view op) {
  if (!ready (f, op)) return {};
  return std::visit (
    [&] (const auto &v) -> std::optional<Operand> {
      using T = std::decay_t<decltype (v)>;
      if constexpr (std::is_same_v<T, Operand>)
        return v;
      else {
        if (!owns (f, v, op)) return {};
        if constexpr (std::is_same_v<T, ConstructionValue>)
          return A::raw (v);
        else if constexpr (std::is_same_v<T, MemoryHandle>)
          return Operand (*A::raw (v));
        else
          return Operand (*A::raw (v));
      }
    },
    A::raw (input));
}
const Register *reg (F *f, ConstructionValue v, std::string_view op) {
  if (!owns (f, v, op)) return nullptr;
  auto o = A::raw (v);
  if (o)
    if (auto r = detail::operand_access (*(o)).get<const Register *> ()) return *r;
  fail (f, op, "operation requires a register value");
  return nullptr;
}
std::optional<detail::ExpressionState> expression (F *f, const ConstructionValue &v,
                                                   std::string_view op) {
  if (!owns (f, v, op)) return {};
  if (auto e = A::scalar (v)) return *e;
  fail (f, op, "block parameter cannot be used as a scalar expression");
  return {};
}
ConstructionValue value (F *f, detail::ExpressionState e, std::string_view op) {
  if (!e.is_valid ()) {
    if (auto error = f->function->module ().error (); error.has_value ())
      fail (f, op, (error).value ());
    else
      fail (f, op, "invalid scalar expression");
    return A::value (f);
  }
  return A::value (f, std::move (e));
}
ConstructionValue result (F *f, Result<const Register &> r, std::string_view op) {
  return A::value (f, take (f, op, std::move (r)));
}
const Label *anchor (F *f, BlockHandle b, std::string_view op) {
  if (!owns (f, b, op)) return nullptr;
  if (auto i = f->anchors.find (A::raw (b)); i != f->anchors.end ()) return i->second;
  auto l = take (f, op, detail::access (*(f->function)).create_label (*A::raw (b)));
  if (l) f->anchors.emplace (A::raw (b), l);
  return l;
}
FunctionBuilder define (BuildState *p, Function *f, std::string_view op) {
  if (!f) return A::function (p);
  auto block = take (p, op, detail::access (*(f)).create_block ());
  if (!block) return A::function (p);
  auto state = std::make_unique<F> (*p, *f, *block);
  auto pointer = state.get ();
  p->functions.push_back (std::move (state));
  return A::function (p, pointer);
}
}  // namespace

ProgramBuilder::ProgramBuilder () : state_ (std::make_unique<BuildState> ()) {}
ProgramBuilder::~ProgramBuilder () = default;
ProgramBuilder::ProgramBuilder (ProgramBuilder &&) noexcept = default;
ProgramBuilder &ProgramBuilder::operator= (ProgramBuilder &&) noexcept = default;
OptionalRef<const Error> ProgramBuilder::error () const {
  static const Error moved{ErrorCode::InvalidArgument, "moved-from ProgramBuilder"};
  if (!state_) return moved;
  return state_->error ? OptionalRef<const Error> (*state_->error) : OptionalRef<const Error>{};
}
ModuleBuilder ProgramBuilder::module (std::string_view name) {
  if (!state_ || !state_->ready ("module")) return A::module (state_.get ());
  return A::module (state_.get (), take (state_.get (), "module",
                                         detail::access (state_->context ()).create_module (name)));
}
Result<Program> ProgramBuilder::finish () {
  if (!state_) return (error ()).value ();
  if (!state_->ready ("finish")) return *state_->error;
  auto valid = state_->context ().verify ();
  if (!(valid).has_value ()) {
    state_->fail ("finish", valid.error ());
    return *state_->error;
  }
  state_->closed = true;
  return std::move (state_->context ());
}
std::string_view SymbolHandle::name () const {
  return state_ && state_->ready ("symbol name") && symbol_ ? symbol_->name () : std::string_view{};
}
VariableHandle::operator SymbolHandle () const {
  if (!state_ || !state_->ready ("variable symbol") || !variable_) return A::symbol (state_);
  return A::symbol (state_, &variable_->symbol ());
}
FunctionBuilder::operator SymbolHandle () const { return symbol (); }
SymbolHandle FunctionBuilder::symbol () const {
  return A::symbol (state_, ready (function_, "function symbol") ? &function_->function->symbol ()
                                                                 : nullptr);
}
BuildOperand::BuildOperand (const FunctionBuilder &v) : value_ (v.symbol ()) {}

FunctionBuilder ModuleBuilder::function (std::string_view name, Signature type,
                                         std::span<const std::string_view> names) const {
  if (!state_ || !state_->ready ("function") || !module_) return A::function (state_);
  return define (state_,
                 take (state_, module_->name (),
                       detail::access (*(module_)).create_function (name, std::move (type), names)),
                 "function entry");
}
SymbolHandle ModuleBuilder::declare_function (std::string_view name, Signature type) const {
  if (!state_ || !state_->ready ("declare function") || !module_) return A::symbol (state_);
  return A::symbol (state_,
                    take (state_, module_->name (),
                          detail::access (*(module_)).declare_function (name, std::move (type))));
}
FunctionBuilder ModuleBuilder::define_function (SymbolHandle symbol) const {
  if (!state_ || !state_->ready ("define function") || !module_) return A::function (state_);
  if (A::owner (symbol) != state_ || !A::raw (symbol) || &A::raw (symbol)->module () != module_) {
    state_->fail ("define function", invalid ("declaration belongs to another module"));
    return A::function (state_);
  }
  // Symbol ownership and mutability were established by this declaration API.
  auto local = detail::access (*(module_)).reserve_symbol (A::raw (symbol)->name ());
  auto s = take (state_, "define function", std::move (local));
  return define (state_,
                 s ? take (state_, "define function",
                           detail::access (*(module_)).define_function (*s))
                   : nullptr,
                 "function entry");
}
SymbolHandle ModuleBuilder::import (std::string_view name, Signature type) const {
  if (!state_ || !state_->ready ("import") || !module_) return A::symbol (state_);
  return A::symbol (state_, take (state_, module_->name (),
                                  detail::access (*(module_)).import (name, std::move (type))));
}
SymbolHandle ModuleBuilder::prototype (std::string_view name, Signature type) const {
  if (!state_ || !state_->ready ("prototype") || !module_) return A::symbol (state_);
  return A::symbol (state_, take (state_, module_->name (),
                                  detail::access (*(module_)).prototype (name, std::move (type))));
}
VariableHandle ModuleBuilder::variable (std::string_view name, Type type) const {
  if (!state_ || !state_->ready ("variable") || !module_) return A::variable (state_);
  return A::variable (state_, take (state_, module_->name (),
                                    detail::access (*(module_)).create_variable (name, type)));
}
VariableHandle ModuleBuilder::variable (std::string_view name, Type type, Operand initial,
                                        bool writable) const {
  if (!state_ || !state_->ready ("variable") || !module_) return A::variable (state_);
  return A::variable (state_,
                      take (state_, module_->name (),
                            detail::access (*(module_))
                              .create_variable (name, type, std::move (initial), writable)));
}
VariableHandle ModuleBuilder::variable (std::string_view name, Type type, SymbolHandle initial,
                                        bool writable) const {
  if (!state_ || !state_->ready ("variable") || !module_) return A::variable (state_);
  if (A::owner (initial) != state_ || !A::raw (initial)) {
    state_->fail ("variable", invalid ("initializer symbol belongs to another program"));
    return A::variable (state_);
  }
  return variable (name, type, Operand (*A::raw (initial)), writable);
}
VariableHandle ModuleBuilder::import_variable (std::string_view name, Type type,
                                               bool writable) const {
  if (!state_ || !state_->ready ("import variable") || !module_) return A::variable (state_);
  return A::variable (state_,
                      take (state_, module_->name (),
                            detail::access (*(module_)).import_variable (name, type, writable)));
}
void ModuleBuilder::export_symbol (SymbolHandle s) const {
  if (!state_ || !state_->ready ("export") || !module_) return;
  if (A::owner (s) != state_ || !A::raw (s)) {
    state_->fail ("export", invalid ("foreign or invalid symbol"));
    return;
  }
  auto r = detail::access (*(module_)).export_symbol (*A::raw (s));
  if (!(r).has_value ()) state_->fail ("export", r.error ());
}

bool ConstructionValue::is_valid () const {
  return function_ && !function_->program->closed && !function_->program->error
         && value_.has_value ();
}
Type ConstructionValue::type () const { return type_; }
ConstructionValue ConstructionValue::convert (Type type) const {
  auto e = expression (function_, *this, "convert");
  return e ? ir::value (function_, e->convert (type), "convert") : A::value (function_);
}
ConstructionValue ConstructionValue::literal_like (Operand raw) const {
  if (!ready (function_, "literal")) return A::value (function_);
  if (type_.is_float ()) {
    auto cast = [&] (auto number) -> Operand {
      if (type_.kind () == Type::Kind::F32) return Operand (static_cast<float> (number));
      if (type_.kind () == Type::Kind::F64) return Operand (static_cast<double> (number));
      return Operand (static_cast<long double> (number));
    };
    if (auto n = detail::operand_access (raw).get<std::int64_t> ())
      raw = cast (*n);
    else if (auto n = detail::operand_access (raw).get<std::uint64_t> ())
      raw = cast (*n);
  }
  return ir::value (function_, function_->builder.literal (type_, std::move (raw)), "literal");
}
ConstructionValue ConstructionValue::apply (Operation op, const ConstructionValue &other) const {
  auto a = expression (function_, *this, "expression");
  auto b = expression (function_, other, "expression");
  if (!a || !b) return A::value (function_);
  auto calculate = [&] () -> detail::ExpressionState {
    switch (op) {
    case Operation::Add: return *a + *b;
    case Operation::Sub: return *a - *b;
    case Operation::Mul: return *a * *b;
    case Operation::Div: return *a / *b;
    case Operation::Mod: return *a % *b;
    case Operation::And: return *a & *b;
    case Operation::Or: return *a | *b;
    case Operation::Xor: return *a ^ *b;
    case Operation::LeftShift: return *a << *b;
    case Operation::RightShift: return *a >> *b;
    case Operation::Equal: return *a == *b;
    case Operation::NotEqual: return *a != *b;
    case Operation::Less: return *a < *b;
    case Operation::LessEqual: return *a <= *b;
    case Operation::Greater: return *a > *b;
    case Operation::GreaterEqual: return *a >= *b;
    }
    return *a;
  };
  return ir::value (function_, calculate (), "expression");
}
#define MIRNEXT_UNARY(symbol)                                                                \
  ConstructionValue ConstructionValue::operator symbol () const {                            \
    auto e = expression (function_, *this, "unary expression");                              \
    return e ? ir::value (function_, symbol * e, "unary expression") : A::value (function_); \
  }
MIRNEXT_UNARY (-)
MIRNEXT_UNARY (~)
MIRNEXT_UNARY (!)
#undef MIRNEXT_UNARY
ConstructionValue FunctionBuilder::arg (std::size_t n) const {
  if (!ready (function_, "argument")) return A::value (function_);
  if (n >= detail::ReferenceAccess::pointers (function_->function->arguments ()).size ()) {
    fail (function_, "argument", "argument index out of range");
    return A::value (function_);
  }
  return A::value (function_,
                   detail::ReferenceAccess::pointers (function_->function->arguments ())[n]);
}
ConstructionValue FunctionBuilder::literal (BuildOperand v) const {
  auto o = operand (function_, v, "literal");
  return o ? ir::value (function_, function_->builder.literal (*o), "literal")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::literal (Type t, BuildOperand v) const {
  auto o = operand (function_, v, "literal");
  return o ? ir::value (function_, function_->builder.literal (t, *o), "literal")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::local (Type t, std::string_view name,
                                          std::string_view hardware) const {
  if (!ready (function_, "local")) return A::value (function_);
  return A::value (function_, take (function_, "local",
                                    detail::access (*(function_->function))
                                      .create_register (t, name, hardware)));
}
void FunctionBuilder::assign (ConstructionValue destination, BuildOperand input) const {
  auto r = reg (function_, destination, "assign");
  auto v = operand (function_, input, "assign");
  if (!r || !v) return;
  if (auto scalar = std::get_if<ConstructionValue> (&A::raw (input))) {
    auto e = expression (function_, *scalar, "assign");
    if (e) check (function_, "assign", function_->builder.assign (*r, *e));
  } else
    check (function_, "assign", function_->builder.assign (*r, *v));
}
BlockHandle FunctionBuilder::entry () const {
  return A::block (function_, ready (function_, "entry") ? function_->entry : nullptr);
}
BlockHandle FunctionBuilder::block (std::string_view name) const {
  return A::block (function_, ready (function_, "block")
                                ? take (function_, "block", function_->builder.create_block (name))
                                : nullptr);
}
void FunctionBuilder::position_at (BlockHandle b) const {
  if (owns (function_, b, "position_at"))
    check (function_, "position_at", function_->builder.set_insert_point (*A::raw (b)));
}
LabelHandle FunctionBuilder::label (BlockHandle b, std::string_view name) const {
  return A::label (function_, owns (function_, b, "label")
                                ? take (function_, "label",
                                        detail::access (*(function_->function))
                                          .create_label (*A::raw (b), name))
                                : nullptr);
}
void FunctionBuilder::jump (BlockHandle b) const {
  if (auto l = anchor (function_, b, "jump"))
    check (function_, "jump", function_->builder.jump (*l));
}
void FunctionBuilder::jump (LabelHandle l) const {
  if (owns (function_, l, "jump")) check (function_, "jump", function_->builder.jump (*A::raw (l)));
}
void FunctionBuilder::branch_if (BuildOperand v, BlockHandle b, bool when_true) const {
  auto o = operand (function_, v, "branch_if");
  auto l = anchor (function_, b, "branch_if");
  if (o && l) check (function_, "branch_if", function_->builder.branch_if (*o, *l, when_true));
}
void FunctionBuilder::branch (Comparison c, ConstructionValue lhs, ConstructionValue rhs,
                              BlockHandle b) const {
  auto a = expression (function_, lhs, "branch"), d = expression (function_, rhs, "branch");
  auto l = anchor (function_, b, "branch");
  if (a && d && l) check (function_, "branch", function_->builder.branch (c, *a, *d, *l));
}
void FunctionBuilder::switch_to (BuildOperand v, std::span<const BlockHandle> blocks) const {
  auto o = operand (function_, v, "switch");
  if (!o) return;
  std::vector<const Label *> labels;
  for (auto b : blocks) {
    auto l = anchor (function_, b, "switch");
    if (!l) return;
    labels.push_back (l);
  }
  check (function_, "switch", function_->builder.switch_to (*o, labels));
}
InstructionHandle FunctionBuilder::emit (Opcode code, std::span<const BuildOperand> inputs) const {
  if (!ready (function_, "emit")) return A::instruction (function_);
  InputBuffer<Operand> values (inputs.size ());
  for (const auto &v : inputs) {
    auto o = operand (function_, v, "emit");
    if (!o) return A::instruction (function_);
    values.push_back (std::move (*o));
  }
  return A::instruction (function_,
                         take (function_, "emit", function_->builder.emit (code, values.view ())));
}
ConstructionValue FunctionBuilder::unary (Opcode code, BuildOperand a,
                                          std::string_view name) const {
  auto v = operand (function_, a, "unary");
  return v ? result (function_, function_->builder.unary (code, *v, name), "unary")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::binary (Opcode code, BuildOperand a, BuildOperand b,
                                           std::string_view name) const {
  auto v = operand (function_, a, "binary"), w = operand (function_, b, "binary");
  return v && w ? result (function_, function_->builder.binary (code, *v, *w, name), "binary")
                : A::value (function_);
}
namespace {
Call call (F *f, SymbolHandle s, std::optional<BuildOperand> pointer,
           std::span<const BuildArgument> inputs, bool inlined) {
  if (!owns (f, s, "call")) return A::call (f);
  InputBuffer<detail::CallArgumentState> args (inputs.size ());
  for (const auto &i : inputs) {
    if (auto block = std::get_if<BlockInput> (&A::raw (i))) {
      if (!owns (f, *block, "call block argument")) return A::call (f);
      args.push_back (*A::raw (*block));
    } else {
      const auto &input = std::get<BuildOperand> (A::raw (i));
      if (auto v = std::get_if<ConstructionValue> (&A::raw (input))) {
        auto e = expression (f, *v, "call argument");
        if (!e) return A::call (f);
        args.push_back (*e);
      } else {
        auto o = operand (f, input, "call argument");
        if (!o) return A::call (f);
        args.push_back (std::move (*o));
      }
    }
  }
  std::optional<Operand> address;
  if (pointer) {
    address = operand (f, *pointer, "call address");
    if (!address) return A::call (f);
  }
  auto r = address   ? f->builder.call_indirect (*A::raw (s), *address, args.view ())
           : inlined ? f->builder.inline_call (*A::raw (s), args.view ())
                     : f->builder.call (*A::raw (s), args.view ());
  if (!(r).has_value ()) {
    fail (f, "call", r.error ());
    return A::call (f);
  }
  return A::call (f, std::move ((r).value ()));
}
}  // namespace
Call FunctionBuilder::call (SymbolHandle s, std::span<const BuildArgument> a) const {
  return ir::call (function_, s, {}, a, false);
}
Call FunctionBuilder::inline_call (SymbolHandle s, std::span<const BuildArgument> a) const {
  return ir::call (function_, s, {}, a, true);
}
Call FunctionBuilder::call_indirect (SymbolHandle s, BuildOperand p,
                                     std::span<const BuildArgument> a) const {
  return ir::call (function_, s, std::move (p), a, false);
}
ConstructionValue Call::result (std::size_t n) const {
  if (!ready (function_, "call result")) return A::value (function_);
  if (!call_ || n >= detail::ReferenceAccess::pointers (call_->results ()).size ()) {
    fail (function_, "call result", "result index out of range");
    return A::value (function_);
  }
  return A::value (function_, detail::ReferenceAccess::pointers (call_->results ())[n]);
}
ConstructionValue Call::value () const {
  if (!ready (function_, "call value")) return A::value (function_);
  if (!call_ || detail::ReferenceAccess::pointers (call_->results ()).size () != 1) {
    fail (function_, "call value", "scalar use requires exactly one call result");
    return A::value (function_);
  }
  return result (0);
}
void FunctionBuilder::ret () const {
  if (ready (function_, "ret")) check (function_, "ret", function_->builder.ret ());
}
void FunctionBuilder::ret (BuildOperand v) const {
  const BuildOperand inputs[]{std::move (v)};
  ret (inputs);
}
void FunctionBuilder::ret (std::span<const BuildOperand> inputs) const {
  if (!ready (function_, "ret")) return;
  InputBuffer<Operand> values (inputs.size ());
  const auto &types = function_->function->signature ().results;
  for (std::size_t n = 0; n < inputs.size (); ++n) {
    if (auto v = std::get_if<ConstructionValue> (&A::raw (inputs[n]));
        v && n < types.size () && v->type () != types[n]) {
      fail (function_, "ret", "result type mismatch; use convert explicitly");
      return;
    }
    auto o = operand (function_, inputs[n], "ret");
    if (!o) return;
    values.push_back (std::move (*o));
  }
  check (function_, "ret", function_->builder.ret (values.view ()));
}
MemoryHandle FunctionBuilder::memory (Type t, ConstructionValue base, std::int64_t offset,
                                      std::optional<ConstructionValue> index,
                                      unsigned scale) const {
  auto b = reg (function_, base, "memory");
  const Register *i = nullptr;
  if (index) {
    i = reg (function_, *index, "memory index");
    if (!i) return A::memory (function_);
  }
  if (!b) return A::memory (function_);
  return A::memory (function_, Memory (t, ::mirnext::detail::borrow_optional (b),
                                       ::mirnext::detail::borrow_optional (i), offset, scale));
}
ConstructionValue FunctionBuilder::load (MemoryHandle m, std::string_view name) const {
  return owns (function_, m, "load")
           ? result (function_, function_->builder.load (*A::raw (m), name), "load")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::load (VariableHandle v, std::string_view name) const {
  return owns (function_, v, "load")
           ? result (function_, function_->builder.load (*A::raw (v), name), "load")
           : A::value (function_);
}
void FunctionBuilder::store (MemoryHandle m, BuildOperand v) const {
  if (!owns (function_, m, "store")) return;
  if (auto scalar = std::get_if<ConstructionValue> (&A::raw (v))) {
    auto e = expression (function_, *scalar, "store");
    if (e) check (function_, "store", function_->builder.store (*A::raw (m), *e));
  } else {
    auto o = operand (function_, v, "store");
    if (o) check (function_, "store", function_->builder.store (*A::raw (m), *o));
  }
}
void FunctionBuilder::store (VariableHandle var, BuildOperand v) const {
  if (!owns (function_, var, "store")) return;
  if (auto scalar = std::get_if<ConstructionValue> (&A::raw (v))) {
    auto e = expression (function_, *scalar, "store");
    if (e) check (function_, "store", function_->builder.store (*A::raw (var), *e));
  } else {
    auto o = operand (function_, v, "store");
    if (o) check (function_, "store", function_->builder.store (*A::raw (var), *o));
  }
}
ConstructionValue FunctionBuilder::address_of (ConstructionValue v, std::string_view name) const {
  auto r = reg (function_, v, "address_of");
  return r ? result (function_, function_->builder.address_of (*r, name), "address_of")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::address_of (SymbolHandle s, std::int64_t offset) const {
  return owns (function_, s, "address_of")
           ? result (function_, function_->builder.address_of (*A::raw (s), offset), "address_of")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::address_of (VariableHandle v) const {
  return owns (function_, v, "address_of")
           ? result (function_, function_->builder.address_of (*A::raw (v)), "address_of")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::address_of (LabelHandle l) const {
  return owns (function_, l, "address_of")
           ? result (function_, function_->builder.address_of (*A::raw (l)), "address_of")
           : A::value (function_);
}
BlockInput FunctionBuilder::block_arg (ConstructionValue address, Type t,
                                       std::int64_t offset) const {
  auto r = reg (function_, address, "block_arg");
  if (!r) return A::block_input (function_);
  if (!t.valid () || !t.is_block () || t.block_size () == 0) {
    fail (function_, "block_arg", "requires a nonempty block type");
    return A::block_input (function_);
  }
  return A::block_input (function_, detail::BlockArgumentState (*r, t, offset));
}
BlockInput FunctionBuilder::return_buffer (ConstructionValue a, std::size_t size) const {
  return block_arg (a, Type::return_block (size));
}
ConstructionValue FunctionBuilder::allocate_stack (BuildOperand v) const {
  auto o = operand (function_, v, "allocate_stack");
  return o ? result (function_, function_->builder.allocate_stack (*o), "allocate_stack")
           : A::value (function_);
}
ConstructionValue FunctionBuilder::stack_save () const {
  return ready (function_, "stack_save")
           ? result (function_, function_->builder.stack_save (), "stack_save")
           : A::value (function_);
}
void FunctionBuilder::stack_restore (ConstructionValue v) const {
  auto r = reg (function_, v, "stack_restore");
  if (r) check (function_, "stack_restore", function_->builder.stack_restore (*r));
}
ScopeHandle FunctionBuilder::begin_stack_scope (std::string_view name) const {
  return A::scope (function_, ready (function_, "begin_stack_scope")
                                ? take (function_, "begin_stack_scope",
                                        function_->builder.begin_stack_scope (name))
                                : nullptr);
}
void FunctionBuilder::end_stack_scope (ScopeHandle s) const {
  if (owns (function_, s, "end_stack_scope"))
    check (function_, "end_stack_scope", function_->builder.end_stack_scope (*A::raw (s)));
}
#define MIRNEXT_LIST_OPERATION(name)                               \
  void FunctionBuilder::name (BuildOperand v) const {              \
    auto o = operand (function_, v, #name);                        \
    if (o) check (function_, #name, function_->builder.name (*o)); \
  }
MIRNEXT_LIST_OPERATION (start_varargs)
MIRNEXT_LIST_OPERATION (end_varargs)
MIRNEXT_LIST_OPERATION (ret_to)
#undef MIRNEXT_LIST_OPERATION
#define MIRNEXT_NEXT_VARARG(name)                                                                 \
  ConstructionValue FunctionBuilder::name (Type t, BuildOperand v) const {                        \
    auto o = operand (function_, v, #name);                                                       \
    return o ? result (function_, function_->builder.name (t, *o), #name) : A::value (function_); \
  }
MIRNEXT_NEXT_VARARG (next_vararg)
MIRNEXT_NEXT_VARARG (next_vararg_address)
#undef MIRNEXT_NEXT_VARARG
void FunctionBuilder::next_vararg_block (ConstructionValue dest, BuildOperand list,
                                         Type type) const {
  auto r = reg (function_, dest, "next_vararg_block");
  auto o = operand (function_, list, "next_vararg_block");
  if (r && o)
    check (function_, "next_vararg_block", function_->builder.next_vararg_block (*r, *o, type));
}
void FunctionBuilder::jump_call (SymbolHandle s) const {
  if (owns (function_, s, "jump_call"))
    check (function_, "jump_call", function_->builder.jump_call (*A::raw (s)));
}
void FunctionBuilder::jump_call (SymbolHandle s, BuildOperand address) const {
  if (!owns (function_, s, "jump_call")) return;
  auto p = operand (function_, address, "jump_call");
  if (p) check (function_, "jump_call", function_->builder.jump_call (*A::raw (s), *p));
}
}  // namespace mirnext::ir

namespace mirnext::ir {
namespace {
Result<DataInitializer> initializer (detail::BuildState &state, const DataInput &input) {
  return std::visit (
    [&] (const auto &value) -> Result<DataInitializer> {
      using T = std::decay_t<decltype (value)>;
      if constexpr (std::is_same_v<T, InitializedData> || std::is_same_v<T, ZeroData>)
        return DataInitializer (value);
      else if constexpr (std::is_same_v<T, FunctionBuilder>) {
        auto f = A::owner (value);
        if (!f || f->program != &state)
          return invalid ("expression initializer belongs to another program");
        return DataInitializer (detail::NodeAccess::make_ExpressionData (f->function));
      } else if constexpr (requires { value.symbol; }) {
        if (A::owner (value.symbol) != &state || !A::raw (value.symbol))
          return invalid ("data symbol belongs to another program");
        return DataInitializer (
          detail::NodeAccess::make_SymbolReference (A::raw (value.symbol), value.displacement));
      } else {
        auto owner = A::owner (value.label);
        if (!owner || owner->program != &state || !A::raw (value.label))
          return invalid ("data label belongs to another program");
        const Label *base = nullptr;
        if (value.base) {
          auto b = A::owner (*value.base);
          if (!b || b->program != &state || !A::raw (*value.base))
            return invalid ("base label belongs to another program");
          base = A::raw (*value.base);
        }
        return DataInitializer (
          detail::NodeAccess::make_LabelReference (A::raw (value.label), base, value.displacement));
      }
    },
    A::raw (input));
}
Error missing_handle (std::string_view kind) {
  return {ErrorCode::InvalidArgument, "invalid " + std::string (kind) + " handle"};
}
}  // namespace
Result<const Symbol &> SymbolHandle::get () const {
  if (state_ && state_->error) return *state_->error;
  if (!state_ || !symbol_) return missing_handle ("symbol");
  return *symbol_;
}
Result<const GlobalData &> DataHandle::get () const {
  if (state_ && state_->error) return *state_->error;
  if (!state_ || !data_) return missing_handle ("data");
  return *data_;
}
DataHandle::operator SymbolHandle () const {
  return A::symbol (state_, data_ ? &data_->symbol () : nullptr);
}
Result<const Instruction &> InstructionHandle::get () const {
  if (function_ && function_->program->error) return *function_->program->error;
  if (!function_ || !instruction_) return missing_handle ("instruction");
  if (detail::access (*function_->function).erased (instruction_, revision_))
    return Error{ErrorCode::InvalidArgument, "instruction was erased"};
  return *instruction_;
}
SymbolHandle ModuleBuilder::forward (std::string_view name) const {
  if (!state_ || !state_->ready ("forward") || !module_) return A::symbol (state_);
  return A::symbol (state_, take (state_, "forward", detail::access (*(module_)).forward (name)));
}
SymbolHandle ModuleBuilder::import (std::string_view name) const {
  if (!state_ || !state_->ready ("import") || !module_) return A::symbol (state_);
  return A::symbol (state_, take (state_, "import", detail::access (*(module_)).import (name)));
}
SymbolHandle ModuleBuilder::declare_data (std::string_view name) const {
  if (!state_ || !state_->ready ("declare data") || !module_) return A::symbol (state_);
  return A::symbol (state_,
                    take (state_, "declare data", detail::access (*(module_)).declare_data (name)));
}
DataHandle ModuleBuilder::data (std::string_view name, DataInput input, bool writable) const {
  if (!state_ || !state_->ready ("data") || !module_) return A::data (state_);
  auto value = initializer (*state_, input);
  if (!(value).has_value ()) {
    state_->fail ("data", value.error ());
    return A::data (state_);
  }
  return A::data (state_, take (state_, "data",
                                detail::access (*(module_))
                                  .create_data (name, std::move ((value).value ()), writable)));
}
DataHandle ModuleBuilder::anonymous_data (DataInput input, bool writable) const {
  return data ({}, std::move (input), writable);
}
DataHandle ModuleBuilder::define_data (SymbolHandle symbol, DataInput input, bool writable) const {
  if (!state_ || !state_->ready ("define data") || !module_) return A::data (state_);
  if (A::owner (symbol) != state_ || !A::raw (symbol) || &A::raw (symbol)->module () != module_) {
    state_->fail ("define data", invalid ("data declaration belongs to another module"));
    return A::data (state_);
  }
  auto value = initializer (*state_, input);
  if (!(value).has_value ()) {
    state_->fail ("define data", value.error ());
    return A::data (state_);
  }
  auto declared = take (state_, "define data",
                        detail::access (*(module_)).reserve_symbol (A::raw (symbol)->name ()));
  if (!declared) return A::data (state_);
  return A::data (state_,
                  take (state_, "define data",
                        detail::access (*(module_))
                          .define_data (*declared, std::move ((value).value ()), writable)));
}
VariableHandle ModuleBuilder::bind_variable (SymbolHandle symbol, Type type, bool writable) const {
  if (!state_ || !state_->ready ("bind variable") || !module_) return A::variable (state_);
  if (A::owner (symbol) != state_ || !A::raw (symbol)) {
    state_->fail ("bind variable", invalid ("variable symbol belongs to another program"));
    return A::variable (state_);
  }
  return A::variable (state_, take (state_, "bind variable",
                                    detail::access (*(module_))
                                      .bind_variable (*A::raw (symbol), type, writable)));
}
VariableHandle ModuleBuilder::variable (std::string_view name, FunctionBuilder expression,
                                        bool writable) const {
  if (!state_ || !state_->ready ("expression variable") || !module_) return A::variable (state_);
  auto f = A::owner (expression);
  if (!f || f->program != state_) {
    state_->fail ("expression variable", invalid ("initializer belongs to another program"));
    return A::variable (state_);
  }
  return A::variable (state_, take (state_, "expression variable",
                                    detail::access (*(module_))
                                      .create_variable (name, *f->function, writable)));
}
void ModuleBuilder::rename_symbol (SymbolHandle symbol, std::string_view name) const {
  if (!state_ || !state_->ready ("rename symbol") || !module_) return;
  if (A::owner (symbol) != state_ || !A::raw (symbol)) {
    state_->fail ("rename symbol", invalid ("symbol belongs to another program"));
    return;
  }
  auto result = detail::access (*(module_)).rename_symbol (*A::raw (symbol), name);
  if (!(result).has_value ()) state_->fail ("rename symbol", result.error ());
}
}  // namespace mirnext::ir

namespace mirnext::ir {
namespace {
FunctionBuilder borrow_function (detail::BuildState *state, const Function &source) {
  if (!state || !state->ready ("edit function")) return A::function (state);
  if (&source.module ().program () != &state->context ()) {
    state->fail ("edit function", invalid ("function belongs to another program"));
    return A::function (state);
  }
  for (auto &f : state->functions)
    if (f->function == &source) return A::function (state, f.get ());
  auto &function = const_cast<Function &> (source);
  BasicBlock *entry = nullptr;
  if (function.blocks ().begin () != function.blocks ().end ())
    entry = &*function.blocks ().begin ();
  else
    entry = take (state, "edit entry", detail::access (function).create_block ());
  if (!entry) return A::function (state);
  auto f = std::make_unique<F> (*state, function, *entry);
  auto result = f.get ();
  state->functions.push_back (std::move (f));
  return A::function (state, result);
}
bool instruction_owner (F *f, const Instruction &instruction, std::string_view operation) {
  if (!ready (f, operation)) return false;
  if (&instruction.function () != f->function) {
    fail (f, operation, "instruction belongs to another function");
    return false;
  }
  return true;
}
}  // namespace
ProgramEditor::ProgramEditor (Program &program)
  : state_ (std::make_unique<detail::BuildState> (program)) {}
ProgramEditor::~ProgramEditor () = default;
ProgramEditor::ProgramEditor (ProgramEditor &&) noexcept = default;
ProgramEditor &ProgramEditor::operator= (ProgramEditor &&) noexcept = default;
OptionalRef<const Error> ProgramEditor::error () const {
  static const Error moved{ErrorCode::InvalidArgument, "moved-from ProgramEditor"};
  if (!state_) return moved;
  return state_->error ? OptionalRef<const Error> (*state_->error) : OptionalRef<const Error>{};
}
ModuleEditor ProgramEditor::module (std::string_view name) {
  if (!state_ || !state_->ready ("edit module")) return A::editor (A::module (state_.get ()));
  auto found = state_->context ().find_module (name).get ();
  auto module = found ? const_cast<Module *> (found)
                      : take (state_.get (), "edit module",
                              detail::access (state_->context ()).create_module (name));
  return A::editor (A::module (state_.get (), module));
}
FunctionEditor ProgramEditor::edit (const Function &function) {
  return A::editor (borrow_function (state_.get (), function));
}
Result<void> ProgramEditor::finish () {
  if (!state_) return (error ()).value ();
  if (!state_->ready ("finish edit")) {
    state_->close ();
    return *state_->error;
  }
  for (auto &function : state_->functions)
    if (function->pending_ssa) {
      auto installed = SSAUpdater::install (std::move (function->pending_ssa));
      if (!(installed).has_value ()) {
        state_->fail ("finish SSA edit", installed.error ());
        state_->close ();
        return *state_->error;
      }
      function->pending_ssa.reset ();
    }
  state_->close ();
  auto valid = state_->context ().status ();
  if (!(valid).has_value ()) {
    state_->fail ("finish edit", valid.error ());
    return *state_->error;
  }
  for (auto module : state_->edited_modules) {
    valid = module->verify ();
    if (!(valid).has_value ()) {
      state_->fail ("finish edit", valid.error ());
      return *state_->error;
    }
  }
  for (auto &function : state_->functions) {
    if (std::find (state_->edited_modules.begin (), state_->edited_modules.end (),
                   &function->function->module ())
        != state_->edited_modules.end ())
      continue;
    valid = (function->function->module ().error ()).has_value ()
              ? Result<void> ((function->function->module ().error ()).value ())
              : function->function->verify ();
    if (!(valid).has_value ()) {
      state_->fail ("finish edit", valid.error ());
      return *state_->error;
    }
  }
  return {};
}
FunctionEditor ModuleEditor::edit (std::string_view name) const {
  if (!state_ || !state_->ready ("edit function") || !module_)
    return A::editor (A::function (state_));
  auto symbol = module_->find_symbol (name).get ();
  if (!symbol || !symbol->function ().get ()) {
    state_->fail ("edit function",
                  {ErrorCode::UnknownName, "unknown function: " + std::string (name)});
    return A::editor (A::function (state_));
  }
  return A::editor (borrow_function (state_, *symbol->function ().get ()));
}
FunctionEditor ModuleEditor::edit (const Function &function) const {
  if (!state_ || !state_->ready ("edit function") || !module_)
    return A::editor (A::function (state_));
  if (&function.module () != module_) {
    state_->fail ("edit function", invalid ("function belongs to another module"));
    return A::editor (A::function (state_));
  }
  return A::editor (borrow_function (state_, function));
}
Result<const Function &> FunctionEditor::get () const {
  auto f = A::owner (*this);
  if (f && f->program->error) return *f->program->error;
  if (!f) return missing_handle ("function editor");
  return *f->function;
}
InstructionHandle FunctionEditor::instruction (const Instruction &instruction) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "instruction handle")) return A::instruction (f);
  return A::instruction (f, const_cast<Instruction *> (&instruction));
}
void FunctionEditor::replace (const Instruction &instruction, Opcode code,
                              std::span<const BuildOperand> input) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "replace")) return;
  std::vector<Operand> operands;
  operands.reserve (input.size ());
  for (const auto &v : input) {
    auto operand = ir::operand (f, v, "replace");
    if (!operand) return;
    operands.push_back (std::move (*operand));
  }
  auto result = InstructionRewriter (*f->function)
                  .replace (const_cast<Instruction &> (instruction), code, operands);
  if (!(result).has_value ()) fail (f, "replace", result.error ());
}
void FunctionEditor::replace_operand (const Instruction &instruction, std::size_t index,
                                      BuildOperand input) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "replace operand")) return;
  auto operand = ir::operand (f, input, "replace operand");
  if (!operand) return;
  auto result
    = InstructionRewriter (*f->function)
        .replace_operand (const_cast<Instruction &> (instruction), index, std::move (*operand));
  if (!(result).has_value ()) fail (f, "replace operand", result.error ());
}
void FunctionEditor::erase (const Instruction &instruction) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "erase")) return;
  auto result = InstructionRewriter (*f->function).erase (const_cast<Instruction &> (instruction));
  if (!(result).has_value ())
    fail (f, "erase", result.error ());
  else
    f->erased[&instruction] = f->function->revisions ().instructions;
}
InstructionHandle FunctionEditor::insert_before (const Instruction &instruction, Opcode code,
                                                 std::span<const BuildOperand> input) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "insert before")) return A::instruction (f);
  std::vector<Operand> operands;
  operands.reserve (input.size ());
  for (const auto &v : input) {
    auto operand = ir::operand (f, v, "insert before");
    if (!operand) return A::instruction (f);
    operands.push_back (std::move (*operand));
  }
  return A::instruction (f, take (f, "insert before",
                                  InstructionRewriter (*f->function)
                                    .insert_before (const_cast<Instruction &> (instruction), code,
                                                    operands)));
}
void FunctionEditor::move_before (const Instruction &instruction, BlockHandle block,
                                  OptionalRef<const Instruction> before) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "move before")) return;
  if (A::owner (block) != f || !A::raw (block)) {
    fail (f, "move before", "block belongs to another function");
    return;
  }
  auto result
    = InstructionRewriter (*f->function)
        .move_before (const_cast<Instruction &> (instruction), *A::raw (block), before.get ());
  if (!(result).has_value ()) fail (f, "move before", result.error ());
}
const Instruction *FunctionEditor::resolve (InstructionHandle instruction,
                                            std::string_view operation) const {
  auto f = A::owner (*this);
  if (!ready (f, operation)) return nullptr;
  auto value = instruction.get ();
  if (!(value).has_value ()) {
    fail (f, operation, value.error ());
    return nullptr;
  }
  if (&value.value ().function () != f->function) {
    fail (f, operation, "instruction belongs to another function");
    return nullptr;
  }
  return &(value).value ();
}
void FunctionEditor::replace (InstructionHandle instruction, Opcode opcode,
                              std::span<const BuildOperand> operands) const {
  if (auto value = resolve (instruction, "replace")) replace (*value, opcode, operands);
}
void FunctionEditor::replace_operand (InstructionHandle instruction, std::size_t index,
                                      BuildOperand operand) const {
  if (auto value = resolve (instruction, "replace operand"))
    replace_operand (*value, index, std::move (operand));
}
void FunctionEditor::erase (InstructionHandle instruction) const {
  if (auto value = resolve (instruction, "erase")) erase (*value);
}
InstructionHandle FunctionEditor::insert_before (InstructionHandle instruction, Opcode opcode,
                                                 std::span<const BuildOperand> operands) const {
  if (auto value = resolve (instruction, "insert before"))
    return insert_before (*value, opcode, operands);
  return A::instruction (A::owner (*this));
}
void FunctionEditor::move_before (InstructionHandle instruction, BlockHandle block,
                                  OptionalRef<const Instruction> before) const {
  if (auto value = resolve (instruction, "move before")) move_before (*value, block, before);
}
void FunctionEditor::replace_uses (ConstructionValue from, ConstructionValue to) const {
  auto f = A::owner (*this);
  if (!ready (f, "replace uses")) return;
  auto source = operand (f, from, "replace uses"), destination = operand (f, to, "replace uses");
  if (!source || !destination) return;
  auto a = detail::operand_access (*(source)).get<const Register *> (),
       b = detail::operand_access (*(destination)).get<const Register *> ();
  if (!a || !b) {
    fail (f, "replace uses", "replacement requires register values");
    return;
  }
  auto result = InstructionRewriter (*f->function).replace_uses (**a, **b);
  if (!(result).has_value ()) fail (f, "replace uses", result.error ());
}
}  // namespace mirnext::ir

namespace mirnext::ir {
Result<const GlobalVariable &> VariableHandle::get () const {
  auto state = state_;
  if (state && state->error) return *state->error;
  if (!state || !variable_) return missing_handle ("VariableHandle");
  return *variable_;
}
Result<const BasicBlock &> BlockHandle::get () const {
  auto state = function_ ? function_->program : nullptr;
  if (state && state->error) return *state->error;
  if (!state || !block_) return missing_handle ("BlockHandle");
  if (detail::access (*function_->function).erased (block_, revision_))
    return Error{ErrorCode::InvalidArgument, "block was erased"};
  return *block_;
}
Result<const Label &> LabelHandle::get () const {
  auto state = function_ ? function_->program : nullptr;
  if (state && state->error) return *state->error;
  if (!state || !label_) return missing_handle ("LabelHandle");
  if (detail::access (*function_->function).erased (label_, revision_))
    return Error{ErrorCode::InvalidArgument, "label was erased"};
  return *label_;
}
Result<const StackScope &> ScopeHandle::get () const {
  auto state = function_ ? function_->program : nullptr;
  if (state && state->error) return *state->error;
  if (!state || !scope_) return missing_handle ("ScopeHandle");
  return *scope_;
}
Result<const Function &> FunctionBuilder::get () const {
  if (state_ && state_->error) return *state_->error;
  if (!state_ || !function_) return missing_handle ("function");
  return *function_->function;
}
Result<Operand> ConstructionValue::get () const {
  if (function_ && function_->program->error) return *function_->program->error;
  if (!function_ || !value_) return missing_handle ("value");
  auto value = A::raw (*this);
  if (!value) return missing_handle ("value");
  return std::move (*value);
}
}  // namespace mirnext::ir

namespace mirnext::ir {
ConstructionValue FunctionEditor::value (const Register &reg) const {
  auto f = A::owner (*this);
  if (!ready (f, "register value")) return A::value (f);
  if (&reg.function () != f->function) {
    fail (f, "register value", "register belongs to another function");
    return A::value (f);
  }
  return A::value (f, &reg);
}
BlockHandle FunctionEditor::bind_block (const BasicBlock &block) const {
  auto f = A::owner (*this);
  if (!ready (f, "block handle")) return A::block (f);
  if (&block.function () != f->function) {
    fail (f, "block handle", "block belongs to another function");
    return A::block (f);
  }
  return A::block (f, const_cast<BasicBlock *> (&block));
}
BlockHandle FunctionEditor::split_before (const Instruction &instruction,
                                          std::string_view name) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "split")) return A::block (f);
  return A::block (f, take (f, "split",
                            InstructionRewriter (*f->function)
                              .split_before (const_cast<Instruction &> (instruction), name)));
}
void FunctionEditor::merge_fallthrough (BlockHandle block) const {
  auto f = A::owner (*this);
  if (!owns (f, block, "merge")) return;
  auto successor = InstructionRewriter (*f->function).next_block (*A::raw (block));
  auto result = InstructionRewriter (*f->function).merge_fallthrough (*A::raw (block));
  if (!(result).has_value ())
    fail (f, "merge", result.error ());
  else if (successor) {
    f->erased[successor] = f->function->revisions ().instructions;
    f->anchors.erase (successor);
    if (f->builder.insert_block () == successor) f->builder.clear_insert_point ();
  }
}
void FunctionEditor::move_block_before (BlockHandle block,
                                        std::optional<BlockHandle> before) const {
  auto f = A::owner (*this);
  if (!owns (f, block, "move block") || (before && !owns (f, *before, "move block"))) return;
  auto result = InstructionRewriter (*f->function)
                  .move_block_before (*A::raw (block), before ? A::raw (*before) : nullptr);
  if (!(result).has_value ()) fail (f, "move block", result.error ());
}
std::size_t FunctionEditor::separate_label_anchors (BlockHandle block) const {
  auto f = A::owner (*this);
  if (!owns (f, block, "separate labels")) return 0;
  auto result = InstructionRewriter (*f->function).separate_label_anchors (*A::raw (block));
  if (!(result).has_value ()) {
    fail (f, "separate labels", result.error ());
    return 0;
  }
  return (result).value ();
}
void FunctionEditor::erase_blocks (std::span<const BlockHandle> blocks) const {
  auto f = A::owner (*this);
  if (!ready (f, "erase blocks")) return;
  std::vector<BasicBlock *> raw;
  std::vector<const Instruction *> instructions;
  std::vector<const Label *> labels;
  raw.reserve (blocks.size ());
  for (auto block : blocks) {
    if (!owns (f, block, "erase blocks")) return;
    raw.push_back (A::raw (block));
    for (auto &i : A::raw (block)->instructions ()) instructions.push_back (&i);
    for (auto &label : A::raw (block)->labels ()) labels.push_back (&label);
  }
  auto result = InstructionRewriter (*f->function).erase_blocks (raw);
  if (!(result).has_value ()) {
    fail (f, "erase blocks", result.error ());
    return;
  }
  for (auto i : instructions) f->erased[i] = f->function->revisions ().instructions;
  for (auto label : labels) f->erased[label] = f->function->revisions ().instructions;
  for (auto block : raw) {
    f->erased[block] = f->function->revisions ().instructions;
    f->anchors.erase (block);
    if (f->builder.insert_block () == block) f->builder.clear_insert_point ();
    if (f->entry == block) f->entry = &*f->function->blocks ().begin ();
  }
}
std::size_t FunctionEditor::erase_unused_labels () const {
  auto f = A::owner (*this);
  if (!ready (f, "erase labels")) return 0;
  std::vector<const Label *> previous;
  for (auto &label : f->function->labels ()) previous.push_back (&label);
  auto result = InstructionRewriter (*f->function).erase_unused_labels ();
  if (!(result).has_value ()) {
    fail (f, "erase labels", result.error ());
    return 0;
  }
  auto retained = f->function->labels ().begin ();
  auto end = f->function->labels ().end ();
  for (auto label : previous) {
    if (retained != end && &*retained == label)
      ++retained;
    else
      f->erased[label] = f->function->revisions ().instructions;
  }
  std::erase_if (f->anchors, [&] (const auto &entry) { return f->erased.contains (entry.second); });
  return (result).value ();
}
void FunctionEditor::set_stack_scope (BlockHandle block, std::optional<ScopeHandle> scope) const {
  auto f = A::owner (*this);
  if (!owns (f, block, "set stack scope") || (scope && !owns (f, *scope, "set stack scope")))
    return;
  auto result = InstructionRewriter (*f->function)
                  .set_stack_scope (*A::raw (block), scope ? A::raw (*scope) : nullptr);
  if (!(result).has_value ()) fail (f, "set stack scope", result.error ());
}
void FunctionEditor::materialize_register_storage (
  std::span<const ConstructionValue> values) const {
  auto f = A::owner (*this);
  if (!ready (f, "materialize storage")) return;
  std::vector<const Register *> registers;
  for (const auto &v : values) {
    auto op = operand (f, v, "materialize storage");
    if (!op) return;
    auto reg = detail::operand_access (*(op)).get<const Register *> ();
    if (!reg) {
      fail (f, "materialize storage", "value is not a register");
      return;
    }
    registers.push_back (*reg);
  }
  auto result = InstructionRewriter (*f->function).materialize_register_storage (registers);
  if (!(result).has_value ()) fail (f, "materialize storage", result.error ());
}
void FunctionEditor::set_memory_location (const Instruction &instruction, std::size_t index,
                                          OptionalRef<const MemoryLocation> location) const {
  auto f = A::owner (*this);
  if (!instruction_owner (f, instruction, "memory location")) return;
  auto result
    = InstructionRewriter (*f->function)
        .set_memory_location (const_cast<Instruction &> (instruction), index, location.get ());
  if (!(result).has_value ()) fail (f, "memory location", result.error ());
}
MemoryHandle FunctionEditor::literal_memory (const GlobalData &data,
                                             ConstructionValue address) const {
  auto f = A::owner (*this);
  auto op = operand (f, address, "literal memory");
  if (!op) return A::memory (f);
  auto reg = detail::operand_access (*(op)).get<const Register *> ();
  if (!reg) {
    fail (f, "literal memory", "literal address is not a register");
    return A::memory (f);
  }
  auto result = InstructionRewriter (*f->function).literal_memory (data, **reg);
  if (!(result).has_value ()) {
    fail (f, "literal memory", result.error ());
    return A::memory (f);
  }
  return A::memory (f, std::move ((result).value ()));
}
SymbolHandle ModuleEditor::symbol (std::string_view name) const {
  if (!state_ || !state_->ready ("symbol") || !module_) return A::symbol (state_);
  auto found = module_->find_symbol (name).get ();
  if (!found) {
    state_->fail ("symbol", {ErrorCode::UnknownName, "unknown symbol: " + std::string (name)});
    return A::symbol (state_);
  }
  return A::symbol (state_, found);
}
DataHandle ModuleEditor::data (std::string_view name) const {
  auto found = symbol (name).get ();
  if (!(found).has_value () || !found.value ().data ().get ()) {
    if (state_ && !state_->error) state_->fail ("data", invalid ("symbol is not defined data"));
    return A::data (state_);
  }
  return A::data (state_, found.value ().data ().get ());
}
VariableHandle ModuleEditor::variable (std::string_view name) const {
  auto found = symbol (name).get ();
  if (!(found).has_value () || !found.value ().variable ().get ()) {
    if (state_ && !state_->error) state_->fail ("variable", invalid ("symbol is not a variable"));
    return A::variable (state_);
  }
  return A::variable (state_, found.value ().variable ().get ());
}
}  // namespace mirnext::ir

namespace mirnext::ir {
Result<const Module &> ModuleBuilder::get () const {
  if (state_ && state_->error) return *state_->error;
  if (!state_ || !module_) return missing_handle ("module editor");
  return *module_;
}
}  // namespace mirnext::ir

namespace mirnext::ir {
namespace {
SSAForm *edit_form (F *state) {
  if (!state) return nullptr;
  if (state->pending_ssa) return state->pending_ssa.get ();
  return const_cast<SSAForm *> (state->function->ssa_form ().get ());
}
}  // namespace
namespace detail {
Result<void> ssa_handle_status (FunctionBuildState *state, const SSAForm *form, const void *value,
                                SSAHandleKind kind, std::uint64_t revision, std::uint64_t phase) {
  if (!state || !value) return invalid ("invalid SSA handle");
  if (state->program->error) return *state->program->error;
  if ((kind == SSAHandleKind::Instruction || kind == SSAHandleKind::Block)
      && detail::access (*state->function).erased (value, revision))
    return invalid ("SSA instruction or block was erased");
  if (!form || edit_form (state) != form || form->phase_identity () != phase)
    return invalid ("SSA handle belongs to an inactive phase");
  switch (kind) {
  case SSAHandleKind::Value:
    if (!static_cast<const SSAValue *> (value)->alive ()) return invalid ("SSA value was erased");
    break;
  case SSAHandleKind::Use:
    if (!static_cast<const SSAUse *> (value)->alive ()) return invalid ("SSA use was erased");
    break;
  case SSAHandleKind::Phi:
    if (!static_cast<const SSAPhi *> (value)->alive ()) return invalid ("SSA phi was erased");
    break;
  default: break;
  }
  return {};
}
}  // namespace detail
SSARewriter FunctionEditor::ssa () const {
  auto f = A::owner (*this);
  if (ready (f, "SSA editor") && !edit_form (f)) fail (f, "SSA editor", "function is not in SSA");
  return A::ssa (f);
}
SSARewriter FunctionEditor::create_ssa () const {
  auto f = A::owner (*this);
  if (!ready (f, "create SSA")) return A::ssa (f);
  if (edit_form (f)) {
    fail (f, "create SSA", "function already has SSA metadata");
    return A::ssa (f);
  }
  auto created = SSAUpdater::create (*f->function);
  if (!(created).has_value ())
    fail (f, "create SSA", created.error ());
  else
    f->pending_ssa = std::make_unique<SSAForm> (std::move ((created).value ()));
  return A::ssa (f);
}
Result<const SSAForm &> SSARewriter::get () const {
  if (!state_) return missing_handle ("SSA editor");
  if (state_->program->error) return *state_->program->error;
  auto form = edit_form (state_);
  if (!form) return missing_handle ("SSA phase");
  return *form;
}
template <class T>
SSAHandle<T> SSARewriter::handle (const T *value) const {
  if (state_ && value) detail::access (*state_->function).track_handles ();
  return {state_, edit_form (state_), value,
          state_ ? state_->function->revisions ().instructions : 0};
}
template <class T>
T *SSARewriter::resolve (SSANode<T> node, std::string_view operation) const {
  if (!ready (state_, operation)) return nullptr;
  if (node.state_ && node.state_ != state_) {
    fail (state_, operation, "SSA handle belongs to another function");
    return nullptr;
  }
  auto valid
    = detail::ssa_handle_status (state_, node.form_ ? node.form_ : edit_form (state_), node.value_,
                                 detail::ssa_kind<T> (), node.revision_,
                                 node.form_           ? node.phase_
                                 : edit_form (state_) ? edit_form (state_)->phase_identity ()
                                                      : 0);
  if (!valid.has_value ()) {
    fail (state_, operation, valid.error ());
    return nullptr;
  }
  const Function *owner;
  if constexpr (requires { node.value_->form (); })
    owner = &node.value_->form ().function ();
  else
    owner = &node.value_->function ();
  if (owner != state_->function) {
    fail (state_, operation, "SSA node belongs to another function");
    return nullptr;
  }
  return const_cast<T *> (node.value_);
}
SSAHandle<SSAValue> SSARewriter::definition (SSANode<Instruction> instruction,
                                             std::size_t operand) const {
  if (!ready (state_, "definition")) return handle<SSAValue> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "definition", "function is not in SSA");
    return handle<SSAValue> ();
  }
  auto instruction_node = resolve (instruction, "definition");
  if (!instruction_node) return handle<SSAValue> ();
  auto result = SSAUpdater (*form).definition (*instruction_node, operand);
  if (!(result).has_value ()) {
    fail (state_, "definition", result.error ());
    return handle<SSAValue> ();
  }
  return handle (&(result).value ());
}
SSAHandle<SSAValue> SSARewriter::initial (SSANode<Register> reg, SSAValue::Kind kind) const {
  if (!ready (state_, "initial")) return handle<SSAValue> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "initial", "function is not in SSA");
    return handle<SSAValue> ();
  }
  auto reg_node = resolve (reg, "initial");
  if (!reg_node) return handle<SSAValue> ();
  auto result = SSAUpdater (*form).initial (*reg_node, kind);
  if (!(result).has_value ()) {
    fail (state_, "initial", result.error ());
    return handle<SSAValue> ();
  }
  return handle (&(result).value ());
}
SSAHandle<SSAPhi> SSARewriter::phi (
  SSANode<BasicBlock> block, SSANode<Register> reg,
  std::span<const std::optional<SSANode<BasicBlock>>> predecessors) const {
  if (!ready (state_, "phi")) return handle<SSAPhi> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "phi", "function is not in SSA");
    return handle<SSAPhi> ();
  }
  auto block_node = resolve (block, "phi");
  if (!block_node) return handle<SSAPhi> ();
  auto reg_node = resolve (reg, "phi");
  if (!reg_node) return handle<SSAPhi> ();
  std::vector<const BasicBlock *> predecessors_nodes;
  predecessors_nodes.reserve (predecessors.size ());
  for (auto input : predecessors) {
    if (!input) {
      predecessors_nodes.push_back (nullptr);
      continue;
    }
    auto node = resolve (*input, "phi");
    if (!node) return handle<SSAPhi> ();
    predecessors_nodes.push_back (node);
  }
  auto result = SSAUpdater (*form).phi (*block_node, *reg_node, predecessors_nodes);
  if (!(result).has_value ()) {
    fail (state_, "phi", result.error ());
    return handle<SSAPhi> ();
  }
  return handle (&(result).value ());
}
SSAHandle<SSAUse> SSARewriter::input (SSANode<Instruction> instruction, std::size_t operand,
                                      SSAOperandPart part, SSANode<SSAValue> value) const {
  if (!ready (state_, "input")) return handle<SSAUse> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "input", "function is not in SSA");
    return handle<SSAUse> ();
  }
  auto instruction_node = resolve (instruction, "input");
  if (!instruction_node) return handle<SSAUse> ();
  auto value_node = resolve (value, "input");
  if (!value_node) return handle<SSAUse> ();
  auto result = SSAUpdater (*form).input (*instruction_node, operand, part, *value_node);
  if (!(result).has_value ()) {
    fail (state_, "input", result.error ());
    return handle<SSAUse> ();
  }
  return handle (&(result).value ());
}
SSAHandle<SSAUse> SSARewriter::hardware_input (SSANode<Instruction> instruction,
                                               SSANode<Register> hardware,
                                               SSANode<SSAValue> value) const {
  if (!ready (state_, "hardware_input")) return handle<SSAUse> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "hardware_input", "function is not in SSA");
    return handle<SSAUse> ();
  }
  auto instruction_node = resolve (instruction, "hardware_input");
  if (!instruction_node) return handle<SSAUse> ();
  auto hardware_node = resolve (hardware, "hardware_input");
  if (!hardware_node) return handle<SSAUse> ();
  auto value_node = resolve (value, "hardware_input");
  if (!value_node) return handle<SSAUse> ();
  auto result = SSAUpdater (*form).hardware_input (*instruction_node, *hardware_node, *value_node);
  if (!(result).has_value ()) {
    fail (state_, "hardware_input", result.error ());
    return handle<SSAUse> ();
  }
  return handle (&(result).value ());
}
SSAHandle<SSAUse> SSARewriter::phi_input (SSANode<SSAPhi> phi, SSANode<SSAValue> value) const {
  if (!ready (state_, "phi_input")) return handle<SSAUse> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "phi_input", "function is not in SSA");
    return handle<SSAUse> ();
  }
  auto phi_node = resolve (phi, "phi_input");
  if (!phi_node) return handle<SSAUse> ();
  auto value_node = resolve (value, "phi_input");
  if (!value_node) return handle<SSAUse> ();
  auto result = SSAUpdater (*form).phi_input (*phi_node, *value_node);
  if (!(result).has_value ()) {
    fail (state_, "phi_input", result.error ());
    return handle<SSAUse> ();
  }
  return handle (&(result).value ());
}
void SSARewriter::retarget (SSANode<SSAUse> use, SSANode<SSAValue> value) const {
  if (!ready (state_, "retarget")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "retarget", "function is not in SSA");
    return;
  }
  auto use_node = resolve (use, "retarget");
  if (!use_node) return;
  auto value_node = resolve (value, "retarget");
  if (!value_node) return;
  auto result = SSAUpdater (*form).retarget (*use_node, *value_node);
  if (!(result).has_value ()) {
    fail (state_, "retarget", result.error ());
    return;
  }
}
void SSARewriter::replace_all_uses (SSANode<SSAValue> from, SSANode<SSAValue> to) const {
  if (!ready (state_, "replace_all_uses")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "replace_all_uses", "function is not in SSA");
    return;
  }
  auto from_node = resolve (from, "replace_all_uses");
  if (!from_node) return;
  auto to_node = resolve (to, "replace_all_uses");
  if (!to_node) return;
  auto result = SSAUpdater (*form).replace_all_uses (*from_node, *to_node);
  if (!(result).has_value ()) {
    fail (state_, "replace_all_uses", result.error ());
    return;
  }
}
void SSARewriter::replace_uses (std::span<const SSANode<SSAUse>> uses,
                                SSANode<SSAValue> value) const {
  if (!ready (state_, "replace_uses")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "replace_uses", "function is not in SSA");
    return;
  }
  std::vector<SSAUse *> uses_nodes;
  uses_nodes.reserve (uses.size ());
  for (auto input : uses) {
    auto node = resolve (input, "replace_uses");
    if (!node) return;
    uses_nodes.push_back (node);
  }
  auto value_node = resolve (value, "replace_uses");
  if (!value_node) return;
  auto result = SSAUpdater (*form).replace_uses (uses_nodes, *value_node);
  if (!(result).has_value ()) {
    fail (state_, "replace_uses", result.error ());
    return;
  }
}
void SSARewriter::rename (SSANode<SSAValue> value, SSANode<Register> reg,
                          bool preserve_addresses) const {
  if (!ready (state_, "rename")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "rename", "function is not in SSA");
    return;
  }
  auto value_node = resolve (value, "rename");
  if (!value_node) return;
  auto reg_node = resolve (reg, "rename");
  if (!reg_node) return;
  auto result = SSAUpdater (*form).rename (*value_node, *reg_node, preserve_addresses);
  if (!(result).has_value ()) {
    fail (state_, "rename", result.error ());
    return;
  }
}
SSAHandle<Register> SSARewriter::fresh_register (SSANode<Register> reg) const {
  if (!ready (state_, "fresh_register")) return handle<Register> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "fresh_register", "function is not in SSA");
    return handle<Register> ();
  }
  auto reg_node = resolve (reg, "fresh_register");
  if (!reg_node) return handle<Register> ();
  auto result = SSAUpdater (*form).fresh_register (*reg_node);
  if (!(result).has_value ()) {
    fail (state_, "fresh_register", result.error ());
    return handle<Register> ();
  }
  return handle (&(result).value ());
}
SSAHandle<Register> SSARewriter::fresh_register (Type type) const {
  if (!ready (state_, "fresh_register")) return handle<Register> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "fresh_register", "function is not in SSA");
    return handle<Register> ();
  }
  auto result = SSAUpdater (*form).fresh_register (type);
  if (!(result).has_value ()) {
    fail (state_, "fresh_register", result.error ());
    return handle<Register> ();
  }
  return handle (&(result).value ());
}
void SSARewriter::replace (SSANode<Instruction> instruction, Opcode opcode,
                           std::span<const Operand> operands,
                           std::span<const SSANode<SSAValue>> inputs) const {
  if (!ready (state_, "replace")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "replace", "function is not in SSA");
    return;
  }
  auto instruction_node = resolve (instruction, "replace");
  if (!instruction_node) return;
  std::vector<SSAValue *> inputs_nodes;
  inputs_nodes.reserve (inputs.size ());
  for (auto input : inputs) {
    auto node = resolve (input, "replace");
    if (!node) return;
    inputs_nodes.push_back (node);
  }
  auto result = SSAUpdater (*form).replace (*instruction_node, opcode, operands, inputs_nodes);
  if (!(result).has_value ()) {
    fail (state_, "replace", result.error ());
    return;
  }
}
void SSARewriter::replace_memory (SSANode<Instruction> instruction, std::size_t operand,
                                  Memory memory, std::optional<SSANode<SSAValue>> base,
                                  std::optional<SSANode<SSAValue>> index) const {
  if (!ready (state_, "replace_memory")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "replace_memory", "function is not in SSA");
    return;
  }
  auto instruction_node = resolve (instruction, "replace_memory");
  if (!instruction_node) return;
  SSAValue *base_node = nullptr;
  if (base) {
    base_node = resolve (*base, "replace_memory");
    if (!base_node) return;
  }
  SSAValue *index_node = nullptr;
  if (index) {
    index_node = resolve (*index, "replace_memory");
    if (!index_node) return;
  }
  auto result
    = SSAUpdater (*form).replace_memory (*instruction_node, operand, memory, base_node, index_node);
  if (!(result).has_value ()) {
    fail (state_, "replace_memory", result.error ());
    return;
  }
}
void SSARewriter::set_memory_location (SSANode<Instruction> instruction, std::size_t operand,
                                       OptionalRef<const MemoryLocation> location) const {
  if (!ready (state_, "set_memory_location")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "set_memory_location", "function is not in SSA");
    return;
  }
  auto instruction_node = resolve (instruction, "set_memory_location");
  if (!instruction_node) return;
  auto result
    = SSAUpdater (*form).set_memory_location (*instruction_node, operand, location.get ());
  if (!(result).has_value ()) {
    fail (state_, "set_memory_location", result.error ());
    return;
  }
}
SSAHandle<SSAValue> SSARewriter::coalesce_copy (SSANode<Instruction> instruction) const {
  if (!ready (state_, "coalesce_copy")) return handle<SSAValue> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "coalesce_copy", "function is not in SSA");
    return handle<SSAValue> ();
  }
  auto instruction_node = resolve (instruction, "coalesce_copy");
  if (!instruction_node) return handle<SSAValue> ();
  auto result = SSAUpdater (*form).coalesce_copy (*instruction_node);
  if (!(result).has_value ()) {
    fail (state_, "coalesce_copy", result.error ());
    return handle<SSAValue> ();
  }
  return handle (&(result).value ());
}
void SSARewriter::erase_phi_input (SSANode<SSAPhi> phi, std::size_t input) const {
  if (!ready (state_, "erase_phi_input")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "erase_phi_input", "function is not in SSA");
    return;
  }
  auto phi_node = resolve (phi, "erase_phi_input");
  if (!phi_node) return;
  auto result = SSAUpdater (*form).erase_phi_input (*phi_node, input);
  if (!(result).has_value ()) {
    fail (state_, "erase_phi_input", result.error ());
    return;
  }
}
void SSARewriter::erase_phi (SSANode<SSAPhi> phi, SSANode<SSAValue> replacement) const {
  if (!ready (state_, "erase_phi")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "erase_phi", "function is not in SSA");
    return;
  }
  auto phi_node = resolve (phi, "erase_phi");
  if (!phi_node) return;
  auto replacement_node = resolve (replacement, "erase_phi");
  if (!replacement_node) return;
  auto result = SSAUpdater (*form).erase_phi (*phi_node, *replacement_node);
  if (!(result).has_value ()) {
    fail (state_, "erase_phi", result.error ());
    return;
  }
}
void SSARewriter::erase_phi (SSANode<SSAPhi> phi) const {
  if (!ready (state_, "erase_phi")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "erase_phi", "function is not in SSA");
    return;
  }
  auto phi_node = resolve (phi, "erase_phi");
  if (!phi_node) return;
  auto result = SSAUpdater (*form).erase_phi (*phi_node);
  if (!(result).has_value ()) {
    fail (state_, "erase_phi", result.error ());
    return;
  }
}
void SSARewriter::erase_instruction (SSANode<Instruction> instruction) const {
  if (!ready (state_, "erase_instruction")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "erase_instruction", "function is not in SSA");
    return;
  }
  auto instruction_node = resolve (instruction, "erase_instruction");
  if (!instruction_node) return;
  auto result = SSAUpdater (*form).erase_instruction (*instruction_node);
  if (!(result).has_value ()) {
    fail (state_, "erase_instruction", result.error ());
    return;
  }
  state_->erased[instruction_node] = state_->function->revisions ().instructions;
}
void SSARewriter::move_before (SSANode<Instruction> instruction, SSANode<BasicBlock> block,
                               OptionalRef<const Instruction> before) const {
  if (!ready (state_, "move_before")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "move_before", "function is not in SSA");
    return;
  }
  auto instruction_node = resolve (instruction, "move_before");
  if (!instruction_node) return;
  auto block_node = resolve (block, "move_before");
  if (!block_node) return;
  auto result = SSAUpdater (*form).move_before (*instruction_node, *block_node, before.get ());
  if (!(result).has_value ()) {
    fail (state_, "move_before", result.error ());
    return;
  }
}
SSAHandle<BasicBlock> SSARewriter::split_fallthrough (SSANode<BasicBlock> source,
                                                      SSANode<BasicBlock> destination) const {
  if (!ready (state_, "split_fallthrough")) return handle<BasicBlock> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "split_fallthrough", "function is not in SSA");
    return handle<BasicBlock> ();
  }
  auto source_node = resolve (source, "split_fallthrough");
  if (!source_node) return handle<BasicBlock> ();
  auto destination_node = resolve (destination, "split_fallthrough");
  if (!destination_node) return handle<BasicBlock> ();
  auto result = SSAUpdater (*form).split_fallthrough (*source_node, *destination_node);
  if (!(result).has_value ()) {
    fail (state_, "split_fallthrough", result.error ());
    return handle<BasicBlock> ();
  }
  return handle (&(result).value ());
}
SSAHandle<Instruction> SSARewriter::insert (SSANode<BasicBlock> block,
                                            OptionalRef<const Instruction> before, Opcode opcode,
                                            std::span<const Operand> operands,
                                            std::span<const SSANode<SSAValue>> inputs,
                                            OptionalRef<const SSAPhi> after_phi) const {
  if (!ready (state_, "insert")) return handle<Instruction> ();
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "insert", "function is not in SSA");
    return handle<Instruction> ();
  }
  auto block_node = resolve (block, "insert");
  if (!block_node) return handle<Instruction> ();
  std::vector<SSAValue *> inputs_nodes;
  inputs_nodes.reserve (inputs.size ());
  for (auto input : inputs) {
    auto node = resolve (input, "insert");
    if (!node) return handle<Instruction> ();
    inputs_nodes.push_back (node);
  }
  auto result = SSAUpdater (*form).insert (*block_node, before.get (), opcode, operands,
                                           inputs_nodes, after_phi.get ());
  if (!(result).has_value ()) {
    fail (state_, "insert", result.error ());
    return handle<Instruction> ();
  }
  return handle (&(result).value ());
}
void SSARewriter::mark_renamed () const {
  if (!ready (state_, "mark_renamed")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "mark_renamed", "function is not in SSA");
    return;
  }
  auto result = SSAUpdater (*form).mark_renamed ();
  if (!(result).has_value ()) {
    fail (state_, "mark_renamed", result.error ());
    return;
  }
}
void SSARewriter::mark_conventional () const {
  if (!ready (state_, "mark_conventional")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "mark_conventional", "function is not in SSA");
    return;
  }
  auto result = SSAUpdater (*form).mark_conventional ();
  if (!(result).has_value ()) {
    fail (state_, "mark_conventional", result.error ());
    return;
  }
}
void SSARewriter::set_phi_position (SSANode<SSAPhi> phi,
                                    OptionalRef<const Instruction> before) const {
  if (!ready (state_, "set_phi_position")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "set_phi_position", "function is not in SSA");
    return;
  }
  auto phi_node = resolve (phi, "set_phi_position");
  if (!phi_node) return;
  auto result = SSAUpdater (*form).set_phi_position (*phi_node, before.get ());
  if (!(result).has_value ()) {
    fail (state_, "set_phi_position", result.error ());
    return;
  }
}
void SSARewriter::complete () const {
  if (!ready (state_, "complete")) return;
  auto form = edit_form (state_);
  if (!form) {
    fail (state_, "complete", "function is not in SSA");
    return;
  }
  auto result = SSAUpdater (*form).complete ();
  if (!(result).has_value ()) {
    fail (state_, "complete", result.error ());
    return;
  }
}
}  // namespace mirnext::ir

// The explicit public interface delegates to the same checked construction and
// rewriting routines as the implementation fixtures above.
#include "mirnext/IR/Builder.hpp"
namespace mirnext::ir {
struct MIRBuilder::Impl {
  ProgramEditor batch;
  std::optional<FunctionEditor> selected;
  explicit Impl (Program &program) : batch (program) {}
};
namespace detail {
struct NativeAccess {
  static BuildState *state (MIRBuilder &b) {
    return b.impl_ ? b.impl_->batch.state_.get () : nullptr;
  }
  static F *function (MIRBuilder &b) {
    return b.impl_ && b.impl_->selected ? A::owner (*b.impl_->selected) : nullptr;
  }
  static FunctionEditor *edit (MIRBuilder &b) {
    return b.impl_ && b.impl_->selected ? &b.impl_->selected.value () : nullptr;
  }
  static bool ready (MIRBuilder &b, std::string_view op) {
    auto s = state (b);
    return s && s->ready (op);
  }
  static bool has_function (MIRBuilder &b, std::string_view op) {
    if (!ready (b, op)) return false;
    if (function (b)) return true;
    state (b)->fail (op, invalid ("no function selected"));
    return false;
  }
  static bool module (MIRBuilder &b, const Module *m, std::string_view op) {
    if (!ready (b, op)) return false;
    if (m && &m->program () == &state (b)->context ()) return true;
    state (b)->fail (op, invalid ("module is null or belongs to another program"));
    return false;
  }
  static bool symbol (MIRBuilder &b, const Symbol *s, std::string_view op) {
    if (s) return module (b, &s->module (), op);
    if (ready (b, op)) state (b)->fail (op, invalid ("null symbol"));
    return false;
  }
  static ModuleBuilder module_builder (MIRBuilder &b, const Module &m) {
    return A::module (state (b), const_cast<Module *> (&m));
  }
  static ConstructionValue input (MIRBuilder &b, const Value &value, std::string_view op) {
    auto f = function (b);
    if (!has_function (b, op)) return A::value (f);
    if (!value.operand_ || (value.owner_ && value.owner_ != f->function)) {
      fail (f, op, "invalid value or value from another function");
      return A::value (f);
    }
    if (auto reg = value.operand_->get<Register> ().get ()) return A::value (f, reg);
    // Only checked builder results can create Value. Rebind a previously checked
    // immediate without repeating literal validation and its temporary storage.
    return A::value (f, detail::ExpressionState (f->builder, value.type_, value.operand_));
  }
  static Value output (MIRBuilder &b, ConstructionValue value) {
    auto raw = value.get ();
    if (!raw.has_value ()) return {};
    return Value (value.type (), std::move (raw).value (), function (b)->function);
  }
  static CallResult output (MIRBuilder &b, Call value) {
    CallResult result;
    if (!A::raw (value)) return result;
    result.instruction_ = &A::raw (value)->instruction ();
    result.count_ = A::raw (value)->results ().size ();
    return result;
  }
  static SymbolHandle symbol_handle (MIRBuilder &b, const Symbol *s) {
    return A::symbol (state (b), s);
  }
  static VariableHandle variable_handle (MIRBuilder &b, const GlobalVariable *v) {
    return A::variable (state (b), v);
  }
  static BlockHandle block (MIRBuilder &b, const BasicBlock *v) {
    return A::block (function (b), const_cast<BasicBlock *> (v));
  }
  static LabelHandle label (MIRBuilder &b, const Label *v) { return A::label (function (b), v); }
  static InstructionHandle instruction (MIRBuilder &b, const Instruction *v) {
    return A::instruction (function (b), const_cast<Instruction *> (v));
  }
  static InputBuffer<BuildArgument> arguments (MIRBuilder &b,
                                               std::span<const CallArgument> values) {
    InputBuffer<BuildArgument> result (values.size ());
    for (auto &value : values) {
      if (auto scalar = std::get_if<Value> (&value.value_))
        result.push_back (input (b, *scalar, "call argument"));
      else {
        auto &memory = std::get<Memory> (value.value_);
        result.push_back (
          A::block_input (function (b),
                          detail::BlockArgumentState (*memory.base ().get (), memory.type,
                                                      memory.displacement)));
      }
    }
    return result;
  }
  static const Instruction *instruction (InstructionHandle value) {
    auto raw = value.get ();
    return raw.has_value () ? &raw.value () : nullptr;
  }
};
}  // namespace detail
using N = detail::NativeAccess;
MIRBuilder::MIRBuilder (Program &program) : impl_ (std::make_unique<Impl> (program)) {}
MIRBuilder::MIRBuilder (Function &function) : MIRBuilder (function.module ().program ()) {
  set_function (&function);
}
MIRBuilder::MIRBuilder (BasicBlock &block)
  : MIRBuilder (const_cast<Function &> (block.function ())) {
  set_insert_point (&block);
}
MIRBuilder::~MIRBuilder () = default;
MIRBuilder::MIRBuilder (MIRBuilder &&) noexcept = default;
MIRBuilder &MIRBuilder::operator= (MIRBuilder &&) noexcept = default;
Result<void> MIRBuilder::finish () {
  if (!impl_) return Error{ErrorCode::InvalidArgument, "moved-from MIRBuilder"};
  return impl_->batch.finish ();
}
Result<void> MIRBuilder::status () const {
  auto e = error ();
  if (e.has_value ()) return e.value ();
  if (impl_->batch.state_->closed)
    return Error{ErrorCode::InvalidArgument, "construction already finished"};
  return {};
}
OptionalRef<const Error> MIRBuilder::error () const {
  static const Error moved{ErrorCode::InvalidArgument, "moved-from MIRBuilder"};
  if (!impl_) return moved;
  auto error = impl_->batch.error ();
  if (error.has_value ()) return error;
  auto storage = impl_->batch.state_->storage;
  return storage && storage->edit_error ? OptionalRef<const Error> (storage->edit_error.value ())
                                        : OptionalRef<const Error>{};
}
Program &MIRBuilder::program () const { return impl_->batch.state_->context (); }
Function *MIRBuilder::function () const {
  auto state = impl_ && impl_->selected ? A::owner (impl_->selected.value ()) : nullptr;
  return state ? state->function : nullptr;
}
BasicBlock *MIRBuilder::insert_block () const {
  auto f = impl_ && impl_->selected ? A::owner (impl_->selected.value ()) : nullptr;
  return f ? const_cast<BasicBlock *> (f->builder.insert_block ()) : nullptr;
}
void MIRBuilder::set_function (const Function *function) {
  if (!N::ready (*this, "select function")) return;
  if (!function) {
    N::state (*this)->fail ("select function", invalid ("null function"));
    return;
  }
  impl_->selected.emplace (impl_->batch.edit (*function));
}
void MIRBuilder::set_insert_point (const BasicBlock *block) {
  if (!N::ready (*this, "set insertion point")) return;
  if (!block) {
    N::state (*this)->fail ("set insertion point", invalid ("null block"));
    return;
  }
  if (function () != &block->function ()) set_function (&block->function ());
  if (N::has_function (*this, "set insertion point"))
    N::edit (*this)->position_at (N::block (*this, block));
}
void MIRBuilder::clear_insert_point () {
  if (N::has_function (*this, "clear insertion point"))
    N::function (*this)->builder.clear_insert_point ();
}
Module *MIRBuilder::create_module (std::string_view name) {
  if (!N::ready (*this, "create module")) return nullptr;
  return take (N::state (*this), "create module", detail::access (program ()).create_module (name));
}
Function *MIRBuilder::create_function (Module *module, std::string_view name, Signature type,
                                       std::span<const std::string_view> names) {
  if (!N::module (*this, module, "create function")) return nullptr;
  auto f = N::module_builder (*this, *module).function (name, std::move (type), names);
  impl_->selected.emplace (A::editor (f));
  auto object = f.get ();
  return object.has_value () ? const_cast<Function *> (&object.value ()) : nullptr;
}
Function *MIRBuilder::define_function (const Symbol *symbol) {
  if (!N::symbol (*this, symbol, "define function")) return nullptr;
  auto f = N::module_builder (*this, symbol->module ())
             .define_function (N::symbol_handle (*this, symbol));
  impl_->selected.emplace (A::editor (f));
  auto object = f.get ();
  return object.has_value () ? const_cast<Function *> (&object.value ()) : nullptr;
}
#define MIRNEXT_NATIVE_MODULE_SYMBOL(method, old)                                            \
  const Symbol *MIRBuilder::method (Module *m, std::string_view name, Signature signature) { \
    if (!N::module (*this, m, #method)) return nullptr;                                      \
    return A::raw (N::module_builder (*this, *m).old (name, std::move (signature)));         \
  }
MIRNEXT_NATIVE_MODULE_SYMBOL (declare_function, declare_function)
MIRNEXT_NATIVE_MODULE_SYMBOL (create_prototype, prototype)
#undef MIRNEXT_NATIVE_MODULE_SYMBOL
const Symbol *MIRBuilder::create_import (Module *m, std::string_view name,
                                         std::optional<Signature> signature) {
  if (!N::module (*this, m, "create import")) return nullptr;
  auto builder = N::module_builder (*this, *m);
  return A::raw (signature ? builder.import (name, std::move (signature.value ()))
                           : builder.import (name));
}
#define MIRNEXT_NATIVE_MODULE_DECL(method, old)                         \
  const Symbol *MIRBuilder::method (Module *m, std::string_view name) { \
    if (!N::module (*this, m, #method)) return nullptr;                 \
    return A::raw (N::module_builder (*this, *m).old (name));           \
  }
MIRNEXT_NATIVE_MODULE_DECL (create_forward, forward)
MIRNEXT_NATIVE_MODULE_DECL (declare_data, declare_data)
#undef MIRNEXT_NATIVE_MODULE_DECL
const GlobalData *MIRBuilder::create_data (Module *m, std::string_view name,
                                           DataInitializer initial, bool writable) {
  if (!N::module (*this, m, "create data")) return nullptr;
  N::module_builder (*this, *m);
  return take (N::state (*this), "create data",
               detail::access (*m).create_data (name, std::move (initial), writable));
}
const GlobalData *MIRBuilder::create_anonymous_data (Module *m, DataInitializer initial,
                                                     bool writable) {
  return create_data (m, {}, std::move (initial), writable);
}
const GlobalData *MIRBuilder::define_data (const Symbol *s, DataInitializer initial,
                                           bool writable) {
  if (!N::symbol (*this, s, "define data")) return nullptr;
  N::module_builder (*this, s->module ());
  return take (N::state (*this), "define data",
               detail::access (const_cast<Module &> (s->module ()))
                 .define_data (const_cast<Symbol &> (*s), std::move (initial), writable));
}
const GlobalVariable *MIRBuilder::create_variable (Module *m, std::string_view name, Type type,
                                                   std::optional<Operand> initial, bool writable) {
  if (!N::module (*this, m, "create variable")) return nullptr;
  N::module_builder (*this, *m);
  return take (N::state (*this), "create variable",
               detail::access (*m).create_variable (name, type, std::move (initial), writable));
}
const GlobalVariable *MIRBuilder::create_expression_variable (Module *m, std::string_view name,
                                                              const Function *initializer,
                                                              bool writable) {
  if (!N::module (*this, m, "create variable")) return nullptr;
  if (!initializer || &initializer->module ().program () != &program ()) {
    N::state (*this)->fail ("create variable", invalid ("invalid expression initializer"));
    return nullptr;
  }
  N::module_builder (*this, *m);
  return take (N::state (*this), "create variable",
               detail::access (*m).create_variable (name, *initializer, writable));
}
const GlobalVariable *MIRBuilder::bind_variable (const Symbol *s, Type type, bool writable) {
  if (!N::symbol (*this, s, "bind variable")) return nullptr;
  return A::raw (N::module_builder (*this, s->module ())
                   .bind_variable (N::symbol_handle (*this, s), type, writable));
}
const GlobalVariable *MIRBuilder::create_import_variable (Module *m, std::string_view name,
                                                          Type type, bool writable) {
  if (!N::module (*this, m, "import variable")) return nullptr;
  return A::raw (N::module_builder (*this, *m).import_variable (name, type, writable));
}
void MIRBuilder::export_symbol (const Symbol *s) {
  if (N::symbol (*this, s, "export symbol"))
    N::module_builder (*this, s->module ()).export_symbol (N::symbol_handle (*this, s));
}
void MIRBuilder::rename_symbol (const Symbol *s, std::string_view name) {
  if (N::symbol (*this, s, "rename symbol"))
    N::module_builder (*this, s->module ()).rename_symbol (N::symbol_handle (*this, s), name);
}
Result<const Operand &> Value::operand () const {
  if (!operand_) return Error{ErrorCode::InvalidOperand, "invalid IR value"};
  return operand_.value ();
}
Value CallResult::result (std::size_t index) const {
  if (!instruction_ || index >= count_) {
    detail::BuildState::value_error (instruction_, "call result", "result index out of range");
    return {};
  }
  auto operands = instruction_->operands ();
  auto symbol = operands.empty () ? nullptr : operands.front ().get<SymbolReference> ().get ();
  auto signature = symbol ? symbol->symbol ().signature ().get () : nullptr;
  if ((instruction_->opcode () != Opcode::Call && instruction_->opcode () != Opcode::Inline)
      || !signature || signature->results.size () != count_ || operands.size () < 2 + count_) {
    detail::BuildState::value_error (instruction_, "call result", "call result shape changed");
    return {};
  }
  auto reg = operands[2 + index].get<Register> ().get ();
  if (!reg) {
    detail::BuildState::value_error (instruction_, "call result", "result is not a register");
    return {};
  }
  return Value (reg->type (), Operand (*reg), &reg->function ());
}
Value CallResult::value () const {
  if (count_ == 1) return result (0);
  detail::BuildState::value_error (instruction_, "call value",
                                   "scalar use requires exactly one call result");
  return {};
}
Value MIRBuilder::argument (std::size_t index) const {
  auto &b = const_cast<MIRBuilder &> (*this);
  return N::has_function (b, "argument") ? N::output (b, N::edit (b)->arg (index)) : Value{};
}
Value MIRBuilder::value (const Register &reg) const {
  auto &b = const_cast<MIRBuilder &> (*this);
  return N::has_function (b, "value") ? N::output (b, N::edit (b)->value (reg)) : Value{};
}
Value MIRBuilder::constant (Type type, Operand operand) const {
  auto &b = const_cast<MIRBuilder &> (*this);
  return N::has_function (b, "constant")
           ? N::output (b, N::edit (b)->literal (type, BuildOperand (std::move (operand))))
           : Value{};
}
Value MIRBuilder::create_register (Type type, std::string_view name, std::string_view hardware) {
  return N::has_function (*this, "create register")
           ? N::output (*this, N::edit (*this)->local (type, name, hardware))
           : Value{};
}
BasicBlock *MIRBuilder::create_block (std::string_view name) {
  if (!N::has_function (*this, "create block")) return nullptr;
  return A::raw (N::edit (*this)->block (name));
}
const Label *MIRBuilder::create_label (const BasicBlock *block, std::string_view name) {
  return N::has_function (*this, "create label")
           ? A::raw (N::edit (*this)->label (N::block (*this, block), name))
           : nullptr;
}
const Instruction *MIRBuilder::create_instruction (Opcode op, std::span<const Operand> operands) {
  if (!N::has_function (*this, "create instruction")) return nullptr;
  InputBuffer<BuildOperand> inputs (operands.size ());
  for (auto &operand : operands) inputs.push_back (operand);
  return N::instruction (N::edit (*this)->emit (op, inputs.view ()));
}
Value MIRBuilder::create_unary (Opcode op, Value input, std::string_view name) {
  if (!N::has_function (*this, "create unary")) return {};
  return N::output (*this, N::edit (*this)->unary (op, N::input (*this, input, "unary"), name));
}
Value MIRBuilder::constant_vector (Type type, std::span<const std::byte> bytes) const {
  auto &builder = const_cast<MIRBuilder &> (*this);
  if (!N::has_function (builder, "vector constant")) return {};
  if (!type.valid () || !type.is_vector () || bytes.size () != 16) {
    N::state (builder)->fail ("vector constant",
                              invalid ("v128 constant requires a valid vector type and 16 bytes"));
    return {};
  }
  VectorConstant value{type};
  std::copy (bytes.begin (), bytes.end (), value.bytes.begin ());
  return constant (type, Operand (value));
}
Value MIRBuilder::create_vector_operation (Opcode opcode, Type type,
                                           std::span<const Value> values) {
  if (!N::has_function (*this, "vector operation")) return {};
  InputBuffer<Operand> operands (values.size ());
  for (auto &value : values) {
    auto checked = N::input (*this, value, "vector operand").get ();
    if (!checked.has_value ()) return {};
    operands.push_back (std::move (checked).value ());
  }
  auto result = N::function (*this)->builder.value_operation (opcode, operands.view (), type, {});
  if (!result.has_value ()) {
    N::state (*this)->fail ("vector operation", result.error ());
    return {};
  }
  return Value (type, Operand (result.value ()), N::function (*this)->function);
}
Value MIRBuilder::create_vector_splat (Type type, Value scalar) {
  const Value values[]{scalar};
  return create_vector_operation (Opcode::VSplat, type, values);
}
Value MIRBuilder::create_extract_lane (Value vector, unsigned lane) {
  const Value values[]{vector, constant (std::uint64_t (lane))};
  return create_vector_operation (Opcode::VExtract, vector.type ().vector_lane_type (), values);
}
Value MIRBuilder::create_insert_lane (Value vector, Value scalar, unsigned lane) {
  const Value values[]{vector, scalar, constant (std::uint64_t (lane))};
  return create_vector_operation (Opcode::VInsert, vector.type (), values);
}
Value MIRBuilder::create_shuffle (Value a, Value b, std::span<const unsigned> lanes) {
  if (!N::has_function (*this, "shuffle")) return {};
  auto type = a.type ();
  if (!type.valid () || !type.is_vector () || lanes.size () != type.vector_lanes ()) {
    N::state (*this)->fail ("shuffle",
                            invalid ("shuffle mask must contain one index per result lane"));
    return {};
  }
  std::array<std::byte, 16> mask{};
  for (std::size_t n = 0; n < lanes.size (); ++n) {
    if (lanes[n] >= 2 * type.vector_lanes ()) {
      N::state (*this)->fail ("shuffle", invalid ("shuffle lane index out of range"));
      return {};
    }
    mask[n] = std::byte (lanes[n]);
  }
  const Value values[]{a, b, constant_vector (Type::vector128 (Type::VectorElement::U8, 16), mask)};
  return create_vector_operation (Opcode::VShuffle, type, values);
}
Value MIRBuilder::create_binary (Opcode op, Value a, Value b, std::string_view name) {
  if (!N::has_function (*this, "create binary")) return {};
  auto lhs = N::input (*this, a, "binary lhs");
  auto rhs = N::input (*this, b, "binary rhs");
  return N::output (*this, N::edit (*this)->binary (op, lhs, rhs, name));
}
#define MIRNEXT_NATIVE_BINARY(name, operation, vector)                                  \
  Value MIRBuilder::create_##name (Value a, Value b) {                                  \
    if (a.type ().is_vector ()) {                                                       \
      const Value inputs[]{a, b};                                                       \
      return create_vector_operation (Opcode::vector, a.type (), inputs);               \
    }                                                                                   \
    auto lhs = N::input (*this, a, #name);                                              \
    auto rhs = N::input (*this, b, #name);                                              \
    return N::output (*this, lhs.apply (ConstructionValue::Operation::operation, rhs)); \
  }
MIRNEXT_NATIVE_BINARY (add, Add, VAdd)
MIRNEXT_NATIVE_BINARY (sub, Sub, VSub)
MIRNEXT_NATIVE_BINARY (mul, Mul, VMul)
MIRNEXT_NATIVE_BINARY (div, Div, VDiv)
MIRNEXT_NATIVE_BINARY (mod, Mod, Mod)
MIRNEXT_NATIVE_BINARY (and, And, VAnd)
MIRNEXT_NATIVE_BINARY (or, Or, VOr)
MIRNEXT_NATIVE_BINARY (xor, Xor, VXor)
MIRNEXT_NATIVE_BINARY (shl, LeftShift, VShl)
MIRNEXT_NATIVE_BINARY (shr, RightShift, VShr)
#undef MIRNEXT_NATIVE_BINARY
#define MIRNEXT_NATIVE_COMPARE(name, operation, code)                                       \
  Value MIRBuilder::create_##name (Value a, Value b) {                                      \
    if (a.type ().is_vector ()) {                                                           \
      const Value inputs[]{a, b};                                                           \
      return create_vector_operation (Opcode::code, a.type ().vector_mask_type (), inputs); \
    }                                                                                       \
    auto lhs = N::input (*this, a, #name);                                                  \
    auto rhs = N::input (*this, b, #name);                                                  \
    return N::output (*this, lhs.apply (ConstructionValue::Operation::operation, rhs));     \
  }
MIRNEXT_NATIVE_COMPARE (eq, Equal, VEq)
MIRNEXT_NATIVE_COMPARE (ne, NotEqual, VNe)
MIRNEXT_NATIVE_COMPARE (lt, Less, VLt)
MIRNEXT_NATIVE_COMPARE (le, LessEqual, VLe)
MIRNEXT_NATIVE_COMPARE (gt, Greater, VGt)
MIRNEXT_NATIVE_COMPARE (ge, GreaterEqual, VGe)
#undef MIRNEXT_NATIVE_COMPARE
Value MIRBuilder::create_neg (Value value) {
  if (value.type ().is_vector ())
    return create_vector_operation (Opcode::VNeg, value.type (), std::span (&value, 1));
  return N::output (*this, -N::input (*this, value, "negate"));
}
Value MIRBuilder::create_bit_not (Value value) {
  return N::output (*this, ~N::input (*this, value, "complement"));
}
Value MIRBuilder::create_logical_not (Value value) {
  return N::output (*this, !N::input (*this, value, "logical not"));
}
Value MIRBuilder::create_convert (Value value, Type type) {
  if (value.type ().is_vector () || type.is_vector ())
    return create_vector_operation (Opcode::VConvert, type, std::span (&value, 1));
  return N::output (*this, N::input (*this, value, "convert").convert (type));
}
#define MIRNEXT_VECTOR_UNARY(name, code)                                                 \
  Value MIRBuilder::create_##name (Value value) {                                        \
    return create_vector_operation (Opcode::code, value.type (), std::span (&value, 1)); \
  }
MIRNEXT_VECTOR_UNARY (abs, VAbs)
MIRNEXT_VECTOR_UNARY (sqrt, VSqrt)
MIRNEXT_VECTOR_UNARY (popcount, VPopcnt)
MIRNEXT_VECTOR_UNARY (ceil, VCeil)
MIRNEXT_VECTOR_UNARY (floor, VFloor)
MIRNEXT_VECTOR_UNARY (trunc, VTrunc)
MIRNEXT_VECTOR_UNARY (nearest, VNearest)
#undef MIRNEXT_VECTOR_UNARY
#define MIRNEXT_VECTOR_BINARY(name, code)                             \
  Value MIRBuilder::create_##name (Value a, Value b) {                \
    const Value inputs[]{a, b};                                       \
    return create_vector_operation (Opcode::code, a.type (), inputs); \
  }
MIRNEXT_VECTOR_BINARY (min, VMin)
MIRNEXT_VECTOR_BINARY (max, VMax)
MIRNEXT_VECTOR_BINARY (pmin, VPMin)
MIRNEXT_VECTOR_BINARY (pmax, VPMax)
MIRNEXT_VECTOR_BINARY (add_sat, VAddSat)
MIRNEXT_VECTOR_BINARY (sub_sat, VSubSat)
MIRNEXT_VECTOR_BINARY (avgr, VAvgr)
MIRNEXT_VECTOR_BINARY (andnot, VAndNot)
MIRNEXT_VECTOR_BINARY (swizzle, VSwizzle)
MIRNEXT_VECTOR_BINARY (q15mulr_sat, VQ15MulrSat)
#undef MIRNEXT_VECTOR_BINARY
#define MIRNEXT_VECTOR_CONVERT(name, code)                                      \
  Value MIRBuilder::create_##name (Value value, Type type) {                    \
    return create_vector_operation (Opcode::code, type, std::span (&value, 1)); \
  }
MIRNEXT_VECTOR_CONVERT (bitcast, VBitcast)
MIRNEXT_VECTOR_CONVERT (convert_low, VConvertLow)
MIRNEXT_VECTOR_CONVERT (convert_zero, VConvertZero)
MIRNEXT_VECTOR_CONVERT (trunc_sat, VTruncSat)
MIRNEXT_VECTOR_CONVERT (trunc_sat_zero, VTruncSatZero)
#undef MIRNEXT_VECTOR_CONVERT
Value MIRBuilder::create_any_true (Value value) {
  return create_vector_operation (Opcode::VAnyTrue, Type (Type::Kind::Bool), std::span (&value, 1));
}
Value MIRBuilder::create_all_true (Value value) {
  return create_vector_operation (Opcode::VAllTrue, Type (Type::Kind::Bool), std::span (&value, 1));
}
Value MIRBuilder::create_bitmask (Value value) {
  return create_vector_operation (Opcode::VBitmask, Type (Type::Kind::U32), std::span (&value, 1));
}
Value MIRBuilder::create_bitselect (Value mask, Value a, Value b) {
  const Value inputs[]{mask, a, b};
  return create_vector_operation (Opcode::VBitselect, a.type (), inputs);
}
Value MIRBuilder::create_extend (Value value, VectorHalf half) {
  if (!N::has_function (*this, "extend")) return {};
  if (half != VectorHalf::Low && half != VectorHalf::High) {
    N::state (*this)->fail ("extend", invalid ("invalid vector half"));
    return {};
  }
  return create_vector_operation (half == VectorHalf::Low ? Opcode::VExtendLow
                                                          : Opcode::VExtendHigh,
                                  detail::vector_widened (value.type ()), std::span (&value, 1));
}
Value MIRBuilder::create_extmul (Value a, Value b, VectorHalf half) {
  if (!N::has_function (*this, "extmul")) return {};
  if (half != VectorHalf::Low && half != VectorHalf::High) {
    N::state (*this)->fail ("extmul", invalid ("invalid vector half"));
    return {};
  }
  const Value inputs[]{a, b};
  return create_vector_operation (half == VectorHalf::Low ? Opcode::VExtmulLow
                                                          : Opcode::VExtmulHigh,
                                  detail::vector_widened (a.type ()), inputs);
}
Value MIRBuilder::create_extadd_pairwise (Value value) {
  return create_vector_operation (Opcode::VExtaddPairwise, detail::vector_widened (value.type ()),
                                  std::span (&value, 1));
}
Value MIRBuilder::create_narrow_sat (Value a, Value b, Type type) {
  const Value inputs[]{a, b};
  return create_vector_operation (Opcode::VNarrowSat, type, inputs);
}
Value MIRBuilder::create_dot (Value a, Value b) {
  const Value inputs[]{a, b};
  return create_vector_operation (Opcode::VDot, detail::vector_integer (32, true), inputs);
}
Value MIRBuilder::create_load_extend (Type type, Value address, std::int64_t displacement) {
  if (!N::has_function (*this, "load extend")) return {};
  if (!type.valid () || !type.is_vector () || !type.vector_lane_type ().is_integer ()
      || type.vector_lanes () == 16) {
    N::state (*this)->fail ("load extend",
                            invalid ("extended load requires integer lanes of 16, 32 or 64 bits"));
    return {};
  }
  auto loaded = create_load (memory (Type (Type::Kind::U64), address, displacement));
  auto bits = create_vector_splat (detail::vector_integer (64, false), loaded);
  auto source = detail::vector_integer (64 / type.vector_lanes (), detail::vector_signed (type));
  return create_extend (create_bitcast (bits, source), VectorHalf::Low);
}
Value MIRBuilder::create_load_splat (Type type, Memory source) {
  if (!N::has_function (*this, "load splat")) return {};
  if (!type.valid () || !type.is_vector () || source.type != type.vector_lane_type ()) {
    N::state (*this)->fail ("load splat", invalid ("splat memory must have the vector lane type"));
    return {};
  }
  return create_vector_splat (type, create_load (source));
}
Value MIRBuilder::create_load_zero (Type type, Memory source) {
  if (!N::has_function (*this, "load zero")) return {};
  const auto kind = source.type.kind ();
  bool word = kind == Type::Kind::I32 || kind == Type::Kind::U32 || kind == Type::Kind::F32;
  bool wide = kind == Type::Kind::I64 || kind == Type::Kind::U64 || kind == Type::Kind::F64;
  if (!type.valid () || !type.is_vector () || (!word && !wide)) {
    N::state (*this)->fail ("load zero",
                            invalid ("zero load requires a vector result and 32/64-bit memory"));
    return {};
  }
  auto loaded = create_load (source);
  auto vector
    = Type::vector128 (word ? (source.type.is_float ()                  ? Type::VectorElement::F32
                               : source.type.kind () == Type::Kind::I32 ? Type::VectorElement::I32
                                                                        : Type::VectorElement::U32)
                            : (source.type.is_float ()                  ? Type::VectorElement::F64
                               : source.type.kind () == Type::Kind::I64 ? Type::VectorElement::I64
                                                                        : Type::VectorElement::U64),
                       word ? 4 : 2);
  std::array<std::byte, 16> zero{};
  return create_bitcast (create_insert_lane (constant_vector (vector, zero), loaded, 0), type);
}
Value MIRBuilder::create_load_lane (Value vector, Memory source, unsigned lane) {
  if (!N::has_function (*this, "load lane") || !N::input (*this, vector, "load lane").is_valid ())
    return {};
  if (!vector.type ().valid () || !vector.type ().is_vector ()
      || source.type != vector.type ().vector_lane_type ()
      || lane >= vector.type ().vector_lanes ()) {
    N::state (*this)->fail ("load lane", invalid ("lane load type or lane mismatch"));
    return {};
  }
  return create_insert_lane (vector, create_load (source), lane);
}
void MIRBuilder::create_store_lane (Memory destination, Value vector, unsigned lane) {
  if (!N::has_function (*this, "store lane") || !N::input (*this, vector, "store lane").is_valid ())
    return;
  if (!vector.type ().valid () || !vector.type ().is_vector ()
      || destination.type != vector.type ().vector_lane_type ()
      || lane >= vector.type ().vector_lanes ()) {
    N::state (*this)->fail ("store lane", invalid ("lane store type or lane mismatch"));
    return;
  }
  // Validate the store address and mutability before emitting the extract.
  auto opcode = destination.type.kind () == Type::Kind::F32   ? Opcode::FMov
                : destination.type.kind () == Type::Kind::F64 ? Opcode::DMov
                                                              : Opcode::Mov;
  Operand zero = destination.type.kind () == Type::Kind::F32   ? Operand (0.0f)
                 : destination.type.kind () == Type::Kind::F64 ? Operand (0.0)
                                                               : Operand (std::int64_t (0));
  const std::array<Operand, 2> operands{Operand (destination), zero};
  auto checked = validate_instruction (*N::function (*this)->function, opcode, operands);
  if (!checked.has_value ()) {
    N::state (*this)->fail ("store lane", checked.error ());
    return;
  }
  create_store (destination, create_extract_lane (vector, lane));
}
void MIRBuilder::create_assign (Value to, Value from) {
  if (!N::has_function (*this, "assign")) return;
  auto destination = N::input (*this, to, "assign destination");
  auto input = N::input (*this, from, "assign source");
  N::edit (*this)->assign (destination, input);
}
void MIRBuilder::create_return () {
  if (N::has_function (*this, "return")) N::edit (*this)->ret ();
}
void MIRBuilder::create_return (Value value) {
  if (N::has_function (*this, "return")) N::edit (*this)->ret (N::input (*this, value, "return"));
}
void MIRBuilder::create_return (std::span<const Value> values) {
  if (!N::has_function (*this, "return")) return;
  InputBuffer<BuildOperand> inputs (values.size ());
  for (auto &value : values) inputs.push_back (N::input (*this, value, "return"));
  N::edit (*this)->ret (inputs.view ());
}
void MIRBuilder::create_jump (const BasicBlock *block) {
  if (N::has_function (*this, "jump")) N::edit (*this)->jump (N::block (*this, block));
}
void MIRBuilder::create_jump (const Label *label) {
  if (N::has_function (*this, "jump")) N::edit (*this)->jump (N::label (*this, label));
}
void MIRBuilder::create_cond_branch (Value value, const BasicBlock *block, bool when_true) {
  if (N::has_function (*this, "branch"))
    N::edit (*this)->branch_if (N::input (*this, value, "branch"), N::block (*this, block),
                                when_true);
}
void MIRBuilder::create_compare_branch (Comparison comparison, Value a, Value b,
                                        const BasicBlock *block) {
  if (!N::has_function (*this, "compare branch")) return;
  auto lhs = N::input (*this, a, "compare lhs");
  auto rhs = N::input (*this, b, "compare rhs");
  N::edit (*this)->branch (comparison, lhs, rhs, N::block (*this, block));
}
void MIRBuilder::create_switch (Value value, std::span<const BasicBlock *const> targets) {
  if (!N::has_function (*this, "switch")) return;
  InputBuffer<BlockHandle> blocks (targets.size (), N::block (*this, nullptr));
  for (auto target : targets) blocks.push_back (N::block (*this, target));
  N::edit (*this)->switch_to (N::input (*this, value, "switch"), blocks.view ());
}
#define MIRNEXT_NATIVE_CALL(method, old)                                                         \
  CallResult MIRBuilder::method (const Symbol *symbol, std::span<const CallArgument> args) {     \
    if (!N::has_function (*this, #method) || !N::symbol (*this, symbol, #method)) return {};     \
    auto inputs = N::arguments (*this, args);                                                    \
    return N::output (*this,                                                                     \
                      N::edit (*this)->old (N::symbol_handle (*this, symbol), inputs.view ()));  \
  }                                                                                              \
  CallResult MIRBuilder::method (const Function *function, std::span<const CallArgument> args) { \
    return method (function ? &function->symbol () : nullptr, args);                             \
  }
MIRNEXT_NATIVE_CALL (create_call, call)
MIRNEXT_NATIVE_CALL (create_inline_call, inline_call)
#undef MIRNEXT_NATIVE_CALL
CallResult MIRBuilder::create_indirect_call (const Symbol *signature, Value address,
                                             std::span<const CallArgument> args) {
  if (!N::has_function (*this, "indirect call") || !N::symbol (*this, signature, "indirect call"))
    return {};
  auto inputs = N::arguments (*this, args);
  return N::output (*this,
                    N::edit (*this)->call_indirect (N::symbol_handle (*this, signature),
                                                    N::input (*this, address, "indirect call"),
                                                    inputs.view ()));
}
void MIRBuilder::create_jump_call (const Symbol *symbol) {
  if (N::has_function (*this, "jump call") && N::symbol (*this, symbol, "jump call"))
    N::edit (*this)->jump_call (N::symbol_handle (*this, symbol));
}
void MIRBuilder::create_jump_call (const Symbol *symbol, Value address) {
  if (N::has_function (*this, "jump call") && N::symbol (*this, symbol, "jump call"))
    N::edit (*this)->jump_call (N::symbol_handle (*this, symbol),
                                N::input (*this, address, "jump call"));
}
void MIRBuilder::create_return_to (Value value) {
  if (N::has_function (*this, "return to"))
    N::edit (*this)->ret_to (N::input (*this, value, "return to"));
}
Memory MIRBuilder::memory (Type type, Value base, std::int64_t displacement,
                           std::optional<Value> index, unsigned scale) {
  if (!N::has_function (*this, "memory")) return detail::NodeAccess::make_Memory (type);
  auto address = N::input (*this, base, "memory base");
  auto idx = index
               ? std::optional<ConstructionValue> (N::input (*this, index.value (), "memory index"))
               : std::nullopt;
  auto value = N::edit (*this)->memory (type, address, displacement, idx, scale);
  return A::raw (value).value_or (detail::NodeAccess::make_Memory (type));
}
Value MIRBuilder::create_load (Memory memory, std::string_view name) {
  return N::has_function (*this, "load")
           ? N::output (*this,
                        N::edit (*this)->load (A::memory (N::function (*this), std::move (memory)),
                                               name))
           : Value{};
}
Value MIRBuilder::create_load (const GlobalVariable *variable, std::string_view name) {
  return N::has_function (*this, "load")
           ? N::output (*this, N::edit (*this)->load (N::variable_handle (*this, variable), name))
           : Value{};
}
void MIRBuilder::create_store (Memory memory, Value value) {
  if (N::has_function (*this, "store"))
    N::edit (*this)->store (A::memory (N::function (*this), std::move (memory)),
                            N::input (*this, value, "store"));
}
void MIRBuilder::create_store (const GlobalVariable *variable, Value value) {
  if (N::has_function (*this, "store"))
    N::edit (*this)->store (N::variable_handle (*this, variable), N::input (*this, value, "store"));
}
Value MIRBuilder::create_address_of (Value value, std::string_view name) {
  return N::has_function (*this, "address of")
           ? N::output (*this,
                        N::edit (*this)->address_of (N::input (*this, value, "address of"), name))
           : Value{};
}
Value MIRBuilder::create_address_of (const Symbol *symbol, std::int64_t displacement) {
  return N::has_function (*this, "address of") && N::symbol (*this, symbol, "address of")
           ? N::output (*this, N::edit (*this)->address_of (N::symbol_handle (*this, symbol),
                                                            displacement))
           : Value{};
}
Value MIRBuilder::create_address_of (const Function *function, std::int64_t displacement) {
  return create_address_of (function ? &function->symbol () : nullptr, displacement);
}
Value MIRBuilder::create_address_of (const GlobalVariable *variable) {
  return N::has_function (*this, "address of")
           ? N::output (*this, N::edit (*this)->address_of (N::variable_handle (*this, variable)))
           : Value{};
}
Value MIRBuilder::create_address_of (const Label *label) {
  return N::has_function (*this, "address of")
           ? N::output (*this, N::edit (*this)->address_of (N::label (*this, label)))
           : Value{};
}
CallArgument MIRBuilder::block_argument (Value address, Type type, std::int64_t displacement) {
  if (!N::has_function (*this, "block argument")) return CallArgument (Value{});
  auto arg
    = N::edit (*this)->block_arg (N::input (*this, address, "block argument"), type, displacement);
  return A::raw (arg) ? CallArgument (A::raw (arg)->memory ()) : CallArgument (Value{});
}
CallArgument MIRBuilder::return_buffer (Value address, std::size_t size) {
  return block_argument (address, Type::return_block (size));
}
Value MIRBuilder::create_stack_alloc (Value size) {
  return N::has_function (*this, "stack alloc")
           ? N::output (*this,
                        N::edit (*this)->allocate_stack (N::input (*this, size, "stack alloc")))
           : Value{};
}
Value MIRBuilder::create_stack_save () {
  return N::has_function (*this, "stack save") ? N::output (*this, N::edit (*this)->stack_save ())
                                               : Value{};
}
void MIRBuilder::create_stack_restore (Value value) {
  if (N::has_function (*this, "stack restore"))
    N::edit (*this)->stack_restore (N::input (*this, value, "stack restore"));
}
const StackScope *MIRBuilder::begin_stack_scope (std::string_view name) {
  return N::has_function (*this, "stack scope") ? A::raw (N::edit (*this)->begin_stack_scope (name))
                                                : nullptr;
}
void MIRBuilder::end_stack_scope (const StackScope *scope) {
  if (N::has_function (*this, "end stack scope"))
    N::edit (*this)->end_stack_scope (A::scope (N::function (*this), scope));
}
void MIRBuilder::create_va_start (Value value) {
  if (N::has_function (*this, "va start"))
    N::edit (*this)->start_varargs (N::input (*this, value, "va start"));
}
void MIRBuilder::create_va_end (Value value) {
  if (N::has_function (*this, "va end"))
    N::edit (*this)->end_varargs (N::input (*this, value, "va end"));
}
Value MIRBuilder::create_va_arg (Type type, Value value) {
  return N::has_function (*this, "va arg")
           ? N::output (*this,
                        N::edit (*this)->next_vararg (type, N::input (*this, value, "va arg")))
           : Value{};
}
Value MIRBuilder::create_va_address (Type type, Value value) {
  return N::has_function (*this, "va address")
           ? N::output (*this, N::edit (*this)->next_vararg_address (type, N::input (*this, value,
                                                                                     "va address")))
           : Value{};
}
void MIRBuilder::create_va_block (Value destination, Value list, Type type) {
  if (!N::has_function (*this, "va block")) return;
  auto d = N::input (*this, destination, "va block destination");
  auto l = N::input (*this, list, "va block list");
  N::edit (*this)->next_vararg_block (d, l, type);
}
void MIRBuilder::replace_instruction (const Instruction *i, Opcode op,
                                      std::span<const Operand> values) {
  if (!N::has_function (*this, "replace instruction")) return;
  InputBuffer<BuildOperand> operands (values.size ());
  for (auto &v : values) operands.push_back (v);
  N::edit (*this)->replace (N::instruction (*this, i), op, operands.view ());
}
void MIRBuilder::replace_operand (const Instruction *i, std::size_t operand, Operand value) {
  if (N::has_function (*this, "replace operand"))
    N::edit (*this)->replace_operand (N::instruction (*this, i), operand, std::move (value));
}
void MIRBuilder::erase_instruction (const Instruction *i) {
  if (N::has_function (*this, "erase instruction"))
    N::edit (*this)->erase (N::instruction (*this, i));
}
const Instruction *MIRBuilder::insert_before (const Instruction *i, Opcode op,
                                              std::span<const Operand> values) {
  if (!N::has_function (*this, "insert instruction")) return nullptr;
  InputBuffer<BuildOperand> operands (values.size ());
  for (auto &v : values) operands.push_back (v);
  return N::instruction (
    N::edit (*this)->insert_before (N::instruction (*this, i), op, operands.view ()));
}
void MIRBuilder::move_before (const Instruction *i, const BasicBlock *b,
                              const Instruction *before) {
  if (N::has_function (*this, "move instruction"))
    N::edit (*this)->move_before (N::instruction (*this, i), N::block (*this, b),
                                  ::mirnext::detail::borrow_optional (before));
}
void MIRBuilder::replace_uses (Value from, Value to) {
  if (!N::has_function (*this, "replace uses")) return;
  auto f = N::input (*this, from, "replace uses");
  auto t = N::input (*this, to, "replace uses");
  N::edit (*this)->replace_uses (f, t);
}
BasicBlock *MIRBuilder::split_before (const Instruction *i, std::string_view name) {
  if (!N::has_function (*this, "split block")) return nullptr;
  if (!i) {
    fail (N::function (*this), "split block", "null instruction");
    return nullptr;
  }
  return A::raw (N::edit (*this)->split_before (*i, name));
}
void MIRBuilder::merge_fallthrough (const BasicBlock *b) {
  if (N::has_function (*this, "merge block"))
    N::edit (*this)->merge_fallthrough (N::block (*this, b));
}
void MIRBuilder::move_block_before (const BasicBlock *b, const BasicBlock *before) {
  if (N::has_function (*this, "move block"))
    N::edit (*this)->move_block_before (N::block (*this, b), before ? std::optional<BlockHandle> (
                                                                        N::block (*this, before))
                                                                    : std::nullopt);
}
std::size_t MIRBuilder::separate_label_anchors (const BasicBlock *b) {
  return N::has_function (*this, "separate labels")
           ? N::edit (*this)->separate_label_anchors (N::block (*this, b))
           : 0;
}
void MIRBuilder::erase_blocks (std::span<const BasicBlock *const> values) {
  if (!N::has_function (*this, "erase blocks")) return;
  std::vector<BlockHandle> blocks;
  for (auto b : values) blocks.push_back (N::block (*this, b));
  N::edit (*this)->erase_blocks (blocks);
}
std::size_t MIRBuilder::erase_unused_labels () {
  return N::has_function (*this, "erase labels") ? N::edit (*this)->erase_unused_labels () : 0;
}
void MIRBuilder::set_stack_scope (const BasicBlock *b, const StackScope *scope) {
  if (N::has_function (*this, "stack scope"))
    N::edit (*this)->set_stack_scope (N::block (*this, b),
                                      scope ? std::optional<ScopeHandle> (
                                                A::scope (N::function (*this), scope))
                                            : std::nullopt);
}
void MIRBuilder::materialize_register_storage (std::span<const Value> values) {
  if (!N::has_function (*this, "register storage")) return;
  std::vector<ConstructionValue> regs;
  for (auto &v : values) regs.push_back (N::input (*this, v, "register storage"));
  N::edit (*this)->materialize_register_storage (regs);
}
void MIRBuilder::set_memory_location (const Instruction *i, std::size_t operand,
                                      OptionalRef<const MemoryLocation> location) {
  if (!N::has_function (*this, "memory location")) return;
  if (!i) {
    fail (N::function (*this), "memory location", "null instruction");
    return;
  }
  N::edit (*this)->set_memory_location (*i, operand, location);
}
Memory MIRBuilder::literal_memory (const GlobalData *data, Value address) {
  if (!N::has_function (*this, "literal memory") || !data)
    return detail::NodeAccess::make_Memory (Type::i64 ());
  auto m = N::edit (*this)->literal_memory (*data, N::input (*this, address, "literal memory"));
  return A::raw (m).value_or (detail::NodeAccess::make_Memory (Type::i64 ()));
}
SSARewriter MIRBuilder::ssa () {
  return N::has_function (*this, "ssa") ? N::edit (*this)->ssa () : SSARewriter (nullptr);
}
SSARewriter MIRBuilder::create_ssa () {
  return N::has_function (*this, "create ssa") ? N::edit (*this)->create_ssa ()
                                               : SSARewriter (nullptr);
}
}  // namespace mirnext::ir
