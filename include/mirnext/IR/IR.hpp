/* Licensed under the MIT License. Canonical register IR with stable object ownership. */
#pragma once
#include "mirnext/Support/Result.hpp"
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <list>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

namespace mirnext::analysis::detail {
template <class, class>
class AnalysisCache;
}
namespace mirnext::ir {
namespace detail {
class NodeAccess;
}
class Program;
namespace detail {
struct ProgramStorage;
struct BuildState;
class ProgramAccess;
class ModuleAccess;
class FunctionAccess;
struct ReferenceAccess;
class OperandAccess;
}  // namespace detail
class Module;
class Function;
class BasicBlock;
class Instruction;
class Label;
class Symbol;
class InstructionBuilder;
class MIRBuilder;
class InstructionRewriter;
class IRCloner;
class SSAForm;
class SSAUpdater;
class StackScope;
class GlobalVariable;
class GlobalData;
enum class Comparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
struct MaterializationStats {
  std::size_t saves = 0, restores = 0, cleanup_blocks = 0, indirect_dispatches = 0;
};
struct StringHash {
  using is_transparent = void;
  std::size_t operator() (std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }
};
template <class T>
using StringMap = std::unordered_map<std::string_view, T, StringHash, std::equal_to<>>;

class Type {
 public:
  enum class Kind {
    Bool,
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F32,
    F64,
    LongDouble,
    Pointer,
    Block,
    ReturnBlock,
    Undef,
    Vector
  };
  enum class VectorElement : std::uint8_t { I8, U8, I16, U16, I32, U32, I64, U64, F32, F64 };
  enum class BlockClass { Default, Class1, Class2, Class3, Class4 };
  constexpr explicit Type (Kind kind) : kind_ (kind) {}
  static constexpr Type i64 () { return Type (Kind::I64); }
  static constexpr Type pointer () { return Type (Kind::Pointer); }
  static constexpr Type vector128 (VectorElement element, unsigned lanes) {
    return Type (Kind::Vector, element, 128, lanes);
  }
  static constexpr Type block (std::size_t size, BlockClass classification = BlockClass::Default) {
    return Type (Kind::Block, size, classification);
  }
  static constexpr Type return_block (std::size_t size) {
    return Type (Kind::ReturnBlock, size, BlockClass::Default);
  }
  constexpr Kind kind () const { return kind_; }
  constexpr std::size_t block_size () const { return size_; }
  constexpr BlockClass block_class () const { return class_; }
  constexpr bool is_block () const { return kind_ == Kind::Block || kind_ == Kind::ReturnBlock; }
  constexpr bool is_float () const { return kind_ >= Kind::F32 && kind_ <= Kind::LongDouble; }
  constexpr bool is_integer () const { return kind_ <= Kind::U64 || kind_ == Kind::Pointer; }
  constexpr bool is_scalar () const { return is_integer () || is_float (); }
  constexpr bool is_vector () const { return kind_ == Kind::Vector; }
  constexpr VectorElement vector_element () const { return vector_element_; }
  constexpr unsigned vector_bits () const { return vector_bits_; }
  constexpr unsigned vector_lanes () const { return vector_lanes_; }
  constexpr Type vector_lane_type () const {
    return is_vector () ? Type (static_cast<Kind> (static_cast<unsigned> (vector_element_) + 1))
                        : Type (Kind::Undef);
  }
  constexpr Type vector_mask_type () const {
    return !is_vector ()         ? Type (Kind::Undef)
           : vector_lanes_ == 16 ? vector128 (VectorElement::U8, 16)
           : vector_lanes_ == 8  ? vector128 (VectorElement::U16, 8)
           : vector_lanes_ == 4  ? vector128 (VectorElement::U32, 4)
                                 : vector128 (VectorElement::U64, 2);
  }
  bool valid () const noexcept;
  friend auto operator<=> (const Type &, const Type &) = default;

 private:
  constexpr Type (Kind kind, std::size_t size, BlockClass classification)
    : kind_ (kind), class_ (classification), size_ (size) {}
  constexpr Type (Kind kind, VectorElement element, unsigned bits, unsigned lanes)
    : kind_ (kind), vector_element_ (element), vector_bits_ (bits), vector_lanes_ (lanes) {}
  Kind kind_;
  BlockClass class_ = BlockClass::Default;
  std::size_t size_ = 0;
  VectorElement vector_element_ = VectorElement::I8;
  std::uint16_t vector_bits_ = 0;
  unsigned vector_lanes_ = 0;
};

struct Signature {
  std::vector<Type> results, parameters;
  bool variadic = false;
  Result<void> verify () const;
  friend auto operator<=> (const Signature &, const Signature &) = default;
};

enum class Opcode {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) name,
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
};
enum class OperandCount { Fixed, Call, Return, Switch };
enum class ControlFlow { Next, Branch, Exit };
struct InstructionDescriptor {
  Opcode opcode;
  std::string_view name, roles;
  OperandCount count;
  ControlFlow flow;
};
std::span<const InstructionDescriptor> instruction_descriptors () noexcept;
OptionalRef<const InstructionDescriptor> instruction_descriptor (Opcode) noexcept;

// Views expose node iterators, never their owning container. In particular a
// mutable transform_view would leak its unique_ptr container through base().
template <class Iterator, bool Const>
class NodeIterator {
 public:
  using value_type = std::remove_cvref_t<decltype (**std::declval<Iterator> ())>;
  using difference_type = typename std::iterator_traits<Iterator>::difference_type;
  using iterator_category = std::bidirectional_iterator_tag;
  using reference = std::conditional_t<Const, const value_type &, value_type &>;
  using pointer = std::conditional_t<Const, const value_type *, value_type *>;
  NodeIterator () = default;
  explicit NodeIterator (Iterator iterator) : iterator_ (iterator) {}
  reference operator* () const { return **iterator_; }
  pointer operator->() const { return std::addressof (**iterator_); }
  NodeIterator &operator++ () {
    ++iterator_;
    return *this;
  }
  NodeIterator operator++ (int) {
    auto old = *this;
    ++*this;
    return old;
  }
  NodeIterator &operator-- () {
    --iterator_;
    return *this;
  }
  NodeIterator operator-- (int) {
    auto old = *this;
    --*this;
    return old;
  }
  friend bool operator== (const NodeIterator &, const NodeIterator &) = default;

 private:
  Iterator iterator_;
};
template <class Iterator>
class NodeRange {
 public:
  NodeRange (Iterator first, Iterator last)
    : NodeRange (first, last, first == last ? 0 : std::distance (first, last)) {}
  NodeRange (Iterator first, Iterator last, std::size_t size)
    : first_ (first), last_ (last), size_ (size) {}
  Iterator begin () const { return first_; }
  Iterator end () const { return last_; }
  bool empty () const { return first_ == last_; }
  std::size_t size () const { return size_; }
  decltype (auto) front () const { return *first_; }
  decltype (auto) back () const { return *std::prev (last_); }

 private:
  Iterator first_, last_;
  std::size_t size_;
};
template <class Container>
auto nodes (const Container &values) {
  using Iterator = NodeIterator<typename Container::const_iterator, true>;
  return NodeRange (Iterator (values.begin ()), Iterator (values.end ()), values.size ());
}
template <class Container>
auto mutable_nodes (Container &values) {
  using Iterator = NodeIterator<typename Container::iterator, false>;
  return NodeRange (Iterator (values.begin ()), Iterator (values.end ()), values.size ());
}

template <class T>
class ReferenceRange {
 public:
  using iterator = NodeIterator<typename std::span<T *const>::iterator, std::is_const_v<T>>;
  explicit ReferenceRange (std::span<T *const> values) : values_ (values) {}
  ReferenceRange () = default;
  iterator begin () const { return iterator (values_.begin ()); }
  iterator end () const { return iterator (values_.end ()); }
  auto rbegin () const { return std::make_reverse_iterator (end ()); }
  auto rend () const { return std::make_reverse_iterator (begin ()); }
  T &operator[] (std::size_t index) const { return *values_[index]; }
  T &front () const { return *values_.front (); }
  T &back () const { return *values_.back (); }
  std::size_t size () const { return values_.size (); }
  bool empty () const { return values_.empty (); }

 private:
  friend struct detail::ReferenceAccess;
  std::span<T *const> values_;
};

// Some graph edges, such as an SSA function-entry predecessor, have no IR node.
template <class T>
class OptionalReferenceRange {
 public:
  class iterator {
    using Base = typename std::span<T *const>::iterator;

   public:
    using value_type = OptionalRef<T>;
    using reference = value_type;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::bidirectional_iterator_tag;
    iterator () = default;
    explicit iterator (Base value) : value_ (value) {}
    value_type operator* () const { return ::mirnext::detail::borrow_optional (*value_); }
    iterator &operator++ () {
      ++value_;
      return *this;
    }
    iterator operator++ (int) {
      auto old = *this;
      ++*this;
      return old;
    }
    iterator &operator-- () {
      --value_;
      return *this;
    }
    iterator operator-- (int) {
      auto old = *this;
      --*this;
      return old;
    }
    friend bool operator== (const iterator &, const iterator &) = default;

   private:
    Base value_;
  };
  OptionalReferenceRange () = default;
  explicit OptionalReferenceRange (std::span<T *const> values) : values_ (values) {}
  iterator begin () const { return iterator (values_.begin ()); }
  iterator end () const { return iterator (values_.end ()); }
  auto rbegin () const { return std::make_reverse_iterator (end ()); }
  auto rend () const { return std::make_reverse_iterator (begin ()); }
  OptionalRef<T> operator[] (std::size_t index) const {
    return ::mirnext::detail::borrow_optional (values_[index]);
  }
  OptionalRef<T> front () const { return (*this)[0]; }
  OptionalRef<T> back () const { return (*this)[size () - 1]; }
  std::size_t size () const { return values_.size (); }
  bool empty () const { return values_.empty (); }

 private:
  friend struct detail::ReferenceAccess;
  std::span<T *const> values_;
};

class Register {
 public:
  Register (const Register &) = delete;
  Register &operator= (const Register &) = delete;
  const Function &function () const { return *function_; }
  Type type () const { return type_; }
  std::string_view name () const { return name_; }
  std::string_view hardware_binding () const { return hardware_; }
  bool is_argument () const { return argument_; }
  bool is_memory_backed () const { return memory_backed_; }

 private:
  friend class Function;
  friend class InstructionBuilder;
  friend class InstructionRewriter;
  friend class IRCloner;
  Register (Function &, Type, std::string_view, std::string_view, bool);
  Function *function_;
  Type type_;
  std::string_view name_, hardware_;
  bool argument_;
  bool memory_backed_ = false;
};

class Label {
 public:
  Label (const Label &) = delete;
  Label &operator= (const Label &) = delete;
  const Function &function () const { return *function_; }
  const BasicBlock &block () const { return *block_; }
  std::string_view name () const { return name_; }

 private:
  friend class Function;
  friend class InstructionRewriter;
  Label (Function &, BasicBlock &, std::string_view);
  Function *function_;
  BasicBlock *block_;
  std::string_view name_;
};

// An optimizer-owned memory equivalence class, independent of SSA values and
// final register allocation. It is analysis provenance, never file-format data.
class MemoryLocation {
 public:
  MemoryLocation (const MemoryLocation &) = delete;
  MemoryLocation &operator= (const MemoryLocation &) = delete;
  const Function &function () const { return *function_; }
  std::size_t creation_order () const { return order_; }

 private:
  friend class InstructionRewriter;
  MemoryLocation (Function &function, std::size_t order) : function_ (&function), order_ (order) {}
  Function *function_;
  std::size_t order_;
};
struct Memory {
  Memory (Type type, OptionalRef<const Register> base = {}, OptionalRef<const Register> index = {},
          std::int64_t displacement = 0, unsigned scale = 1, std::string alias = {},
          std::string nonalias = {})
    : type (type),
      base_ (base.get ()),
      index_ (index.get ()),
      displacement (displacement),
      scale (scale),
      alias (std::move (alias)),
      nonalias (std::move (nonalias)) {}
  Type type;
  OptionalRef<const Register> base () const { return ::mirnext::detail::borrow_optional (base_); }
  OptionalRef<const Register> index () const { return ::mirnext::detail::borrow_optional (index_); }
  std::int64_t displacement = 0;
  unsigned scale = 1;
  std::string alias, nonalias;
  OptionalRef<const GlobalData> literal_source () const {
    return ::mirnext::detail::borrow_optional (literal_source_);
  }
  OptionalRef<const MemoryLocation> location () const {
    return ::mirnext::detail::borrow_optional (location_);
  }
  friend bool operator== (const Memory &a, const Memory &b) {
    return a.type == b.type && a.base_ == b.base_ && a.index_ == b.index_
           && a.displacement == b.displacement && a.scale == b.scale && a.alias == b.alias
           && a.nonalias == b.nonalias && a.literal_source_ == b.literal_source_;
  }

 private:
  friend class detail::NodeAccess;
  const Register *base_ = nullptr, *index_ = nullptr;
  friend class InstructionRewriter;
  friend class IRCloner;
  const GlobalData *literal_source_ = nullptr;
  const MemoryLocation *location_ = nullptr;
};
struct SymbolReference {
  SymbolReference (const Symbol &symbol, std::int64_t displacement = 0)
    : SymbolReference (Key{}, &symbol, std::move (displacement)) {}
  const Symbol &symbol () const { return *symbol_; }
  std::int64_t displacement = 0;
  friend bool operator== (const SymbolReference &, const SymbolReference &) = default;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  SymbolReference (Key, const Symbol *symbol, std::int64_t displacement)
    : displacement (displacement), symbol_ (symbol) {}
  const Symbol *symbol_;
};
struct VectorConstant {
  Type type;
  std::array<std::byte, 16> bytes{};
  friend bool operator== (const VectorConstant &, const VectorConstant &) = default;
};
}  // namespace mirnext::ir
namespace std {
template <>
struct hash<mirnext::ir::VectorConstant> {
  std::size_t operator() (const mirnext::ir::VectorConstant &value) const noexcept {
    std::size_t result = static_cast<unsigned> (value.type.kind ());
    result ^= value.type.vector_lanes () + (result << 6) + (result >> 2);
    result ^= static_cast<unsigned> (value.type.vector_element ()) + (result << 6) + (result >> 2);
    for (auto byte : value.bytes)
      result ^= std::to_integer<unsigned char> (byte) + (result << 6) + (result >> 2);
    return result;
  }
};
}  // namespace std
namespace mirnext::ir {
class Operand {
  using Storage
    = std::variant<std::int64_t, std::uint64_t, float, double, long double, std::string,
                   VectorConstant, const Register *, Memory, const Label *, SymbolReference>;

 public:
  Operand (std::int64_t value) : value_ (value) {}
  Operand (std::uint64_t value) : value_ (value) {}
  template <class Integer>
    requires (std::is_integral_v<Integer> && sizeof (Integer) <= sizeof (std::uint64_t))
  Operand (Integer value)
    : value_ (
        static_cast<std::conditional_t<std::is_signed_v<Integer> || std::is_same_v<Integer, bool>,
                                       std::int64_t, std::uint64_t>> (value)) {}
  Operand (float value) : value_ (value) {}
  Operand (double value) : value_ (value) {}
  Operand (long double value) : value_ (value) {}
  Operand (std::string value) : value_ (std::move (value)) {}
  Operand (VectorConstant value) : value_ (std::move (value)) {}
  Operand (const Register &value) : value_ (&value) {}
  Operand (Memory value) : value_ (std::move (value)) {}
  Operand (const Label &value) : value_ (&value) {}
  Operand (const Symbol &value) : value_ (SymbolReference{value}) {}
  Operand (SymbolReference value) : value_ (value) {}
  template <class T>
    requires (!std::is_pointer_v<T>)
  OptionalRef<const T> get () const {
    if constexpr (std::is_same_v<T, Register> || std::is_same_v<T, Label>) {
      auto value = std::get_if<const T *> (&value_);
      return value ? ::mirnext::detail::borrow_optional (*value) : OptionalRef<const T>{};
    } else
      return ::mirnext::detail::borrow_optional (std::get_if<T> (&value_));
  }
  friend bool operator== (const Operand &, const Operand &) = default;

 private:
  friend class detail::OperandAccess;
  friend class InstructionRewriter;
  Storage value_;
};

struct Revisions {
  std::uint64_t instructions = 0, control_flow = 0, registers = 0, module_data = 0;
  std::uint64_t symbols = 0;
  friend bool operator== (const Revisions &, const Revisions &) = default;
};

class Instruction {
 public:
  class ConstructionKey {
    friend class InstructionBuilder;
    friend class InstructionRewriter;
    ConstructionKey () = default;
  };
  Instruction (ConstructionKey, BasicBlock &block, Opcode opcode, std::vector<Operand> operands)
    : block_ (&block), opcode_ (opcode), operands_ (std::move (operands)) {}
  Instruction (const Instruction &) = delete;
  Instruction &operator= (const Instruction &) = delete;
  Opcode opcode () const { return opcode_; }
  std::span<const Operand> operands () const { return operands_; }
  const BasicBlock &block () const { return *block_; }
  const Function &function () const;

 private:
  friend class InstructionBuilder;
  friend class InstructionRewriter;
  BasicBlock *block_;
  Opcode opcode_;
  std::vector<Operand> operands_;
  // List iterators survive splice, so rewrites never need to scan for this node.
  std::list<Instruction>::iterator position_;
};

class BasicBlock {
 public:
  BasicBlock (const BasicBlock &) = delete;
  BasicBlock &operator= (const BasicBlock &) = delete;
  const Function &function () const { return *function_; }
  std::string_view name () const { return name_; }
  auto instructions () const {
    return NodeRange (instructions_.cbegin (), instructions_.cend (), instructions_.size ());
  }
  auto instructions () {
    return NodeRange (instructions_.begin (), instructions_.end (), instructions_.size ());
  }
  ReferenceRange<const Label> labels () const { return ReferenceRange<const Label> (labels_); }
  std::size_t size () const { return instructions_.size (); }
  OptionalRef<const Instruction> terminator () const;
  OptionalRef<const StackScope> stack_scope () const {
    return ::mirnext::detail::borrow_optional (scope_);
  }

 private:
  friend class Function;
  friend class InstructionBuilder;
  friend class InstructionRewriter;
  BasicBlock (Function &, std::string_view);
  Function *function_;
  std::string_view name_;
  // One allocation per stable instruction; splice moves it without copying operands.
  std::list<Instruction> instructions_;
  std::vector<const Label *> labels_;
  const StackScope *scope_ = nullptr;
  std::list<std::unique_ptr<BasicBlock>>::iterator position_;
};

class StackScope {
 public:
  StackScope (const StackScope &) = delete;
  StackScope &operator= (const StackScope &) = delete;
  const Function &function () const { return *function_; }
  OptionalRef<const StackScope> parent () const {
    return ::mirnext::detail::borrow_optional (parent_);
  }
  std::string_view name () const { return name_; }
  bool materialized () const { return materialized_; }
  std::size_t depth () const { return depth_; }

 private:
  friend class Function;
  friend class detail::FunctionAccess;
  friend class IRCloner;
  StackScope (Function &function, const StackScope *parent, std::string_view name)
    : function_ (&function),
      parent_ (parent),
      name_ (name),
      depth_ (parent ? parent->depth () + 1 : 1) {}
  Function *function_;
  const StackScope *parent_;
  std::string_view name_;
  bool materialized_ = false;
  std::size_t depth_;
};

class Function {
 public:
  ~Function ();
  Function (const Function &) = delete;
  Function &operator= (const Function &) = delete;
  Module &module () { return *module_; }
  const Module &module () const { return *module_; }
  const Symbol &symbol () const { return *symbol_; }
  const Signature &signature () const;
  auto blocks () const { return nodes (blocks_); }
  auto blocks () { return mutable_nodes (blocks_); }
  auto registers () const { return nodes (registers_); }
  bool has_hardware_bindings () const noexcept { return !hardware_registers_.empty (); }
  auto labels () const { return nodes (labels_); }
  auto memory_locations () const { return nodes (memory_locations_); }
  auto stack_scopes () const { return nodes (scopes_); }
  ReferenceRange<const Register> arguments () const {
    return ReferenceRange<const Register> (arguments_);
  }
  // Borrowed data-use lists share the module's immutable initializer objects.
  ReferenceRange<const GlobalData> label_data_uses () const;
  ReferenceRange<const GlobalData> expression_data_uses () const;

  bool needs_materialization () const;

  OptionalRef<const BasicBlock> find_block (std::string_view) const;
  OptionalRef<const Register> find_register (std::string_view) const;
  OptionalRef<const Label> find_label (std::string_view) const;
  Revisions revisions () const;
  std::size_t instruction_count () const;
  Result<void> verify () const;
  OptionalRef<const SSAForm> ssa_form () const {
    return ::mirnext::detail::borrow_optional (ssa_.get ());
  }
  Result<void> require_register_ir () const;

 private:
  friend class detail::FunctionAccess;
  Result<BasicBlock &> create_block_after (BasicBlock &, std::string_view name = {},
                                           const StackScope * = nullptr);
  Result<Label &> create_label (BasicBlock &, std::string_view name = {});
  Result<BasicBlock &> create_block (std::string_view name = {}, const StackScope * = nullptr);
  Result<Register &> create_register (Type, std::string_view name = {},
                                      std::string_view hardware = {});
  Result<StackScope &> create_stack_scope (const StackScope *parent = nullptr,
                                           std::string_view name = {});
  Result<BasicBlock &> create_block_before (BasicBlock &, std::string_view name = {},
                                            const StackScope * = nullptr);
  friend class Module;
  friend class InstructionBuilder;
  friend class InstructionRewriter;
  friend class SSAUpdater;
  friend class IRCloner;
  Function (Module &, Symbol &);
  Result<Register &> add_register (Type, std::string_view, std::string_view, bool);
  void changed (bool cfg);
  void registers_changed ();
  Module *module_;
  Symbol *symbol_;
  std::uint64_t next_ssa_identity_ = 0;
  bool tracks_handles_ = false;
  std::unordered_map<const void *, std::uint64_t> erased_nodes_;
  std::list<std::unique_ptr<BasicBlock>> blocks_;
  std::vector<std::unique_ptr<StackScope>> scopes_;
  std::vector<std::unique_ptr<Register>> registers_;
  std::vector<std::unique_ptr<Label>> labels_;
  std::vector<std::unique_ptr<MemoryLocation>> memory_locations_;
  std::vector<const Register *> arguments_;
  StringMap<Register *> register_names_, hardware_registers_;
  StringMap<BasicBlock *> block_names_;
  StringMap<Label *> label_names_;
  Revisions revisions_;
  std::unique_ptr<SSAForm> ssa_;
};

struct InitializedData {
  Type type;
  std::vector<Operand> values;
};
struct ZeroData {
  std::size_t size;
};
struct LabelReference {
  LabelReference (const Label &label, OptionalRef<const Label> base = {},
                  std::int64_t displacement = 0)
    : LabelReference (Key{}, &label, base.get (), std::move (displacement)) {}
  const Label &label () const { return *label_; }
  OptionalRef<const Label> base () const { return ::mirnext::detail::borrow_optional (base_); }
  std::int64_t displacement = 0;

 private:
  friend class detail::NodeAccess;
  struct Key {};
  LabelReference (Key, const Label *label, const Label *base, std::int64_t displacement)
    : displacement (displacement), label_ (label), base_ (base) {}
  const Label *label_;
  const Label *base_;
};
struct ExpressionData {
  ExpressionData (const Function &function) : ExpressionData (Key{}, &function) {}
  const Function &function () const { return *function_; }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  ExpressionData (Key, const Function *function) : function_ (function) {}
  const Function *function_;
};
using DataInitializer
  = std::variant<InitializedData, ZeroData, SymbolReference, LabelReference, ExpressionData>;
class GlobalData {
 public:
  GlobalData (const GlobalData &) = delete;
  GlobalData &operator= (const GlobalData &) = delete;
  const Symbol &symbol () const { return *symbol_; }
  const DataInitializer &initializer () const { return initializer_; }
  bool writable () const { return writable_; }
  bool is_literal_pool () const { return literal_pool_; }

 private:
  friend class Module;
  friend class IRCloner;
  GlobalData (Symbol &symbol, DataInitializer initializer, bool writable)
    : symbol_ (&symbol), initializer_ (std::move (initializer)), writable_ (writable) {}
  Symbol *symbol_;
  DataInitializer initializer_;
  bool writable_;
  bool literal_pool_ = false;
};

class GlobalVariable {
 public:
  GlobalVariable (const GlobalVariable &) = delete;
  GlobalVariable &operator= (const GlobalVariable &) = delete;
  const Symbol &symbol () const { return *symbol_; }
  Type type () const { return type_; }
  bool writable () const { return writable_; }

 private:
  friend class Module;
  GlobalVariable (const Symbol &symbol, Type type, bool writable)
    : symbol_ (&symbol), type_ (type), writable_ (writable) {}
  const Symbol *symbol_;
  Type type_;
  bool writable_;
};

class Symbol {
 public:
  Symbol (const Symbol &) = delete;
  Symbol &operator= (const Symbol &) = delete;
  enum class Kind { Forward, Function, Data, Prototype, Import };
  const Module &module () const { return *module_; }
  std::string_view name () const { return name_; }
  Kind kind () const { return kind_; }
  OptionalRef<const Signature> signature () const {
    return ::mirnext::detail::borrow_optional (signature_);
  }
  OptionalRef<const Function> function () const {
    return ::mirnext::detail::borrow_optional (function_);
  }
  OptionalRef<const GlobalData> data () const { return ::mirnext::detail::borrow_optional (data_); }
  OptionalRef<const GlobalVariable> variable () const {
    return ::mirnext::detail::borrow_optional (variable_.get ());
  }
  ReferenceRange<const GlobalData> reference_data_uses () const;
  bool exported () const { return exported_; }

 private:
  friend class Module;
  friend class IRCloner;
  Symbol (Module &, std::string_view, Kind);
  Module *module_;
  std::string_view name_;
  Kind kind_;
  const Signature *signature_ = nullptr;
  Function *function_ = nullptr;
  GlobalData *data_ = nullptr;
  std::unique_ptr<GlobalVariable> variable_;
  bool exported_ = false;
  bool forward_declared_ = false;
  std::optional<std::size_t> data_slot_;
};
struct ForwardDeclaration {
  ForwardDeclaration (const Symbol &symbol) : ForwardDeclaration (Key{}, &symbol) {}
  const Symbol &symbol () const { return *symbol_; }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  ForwardDeclaration (Key, const Symbol *symbol) : symbol_ (symbol) {}
  const Symbol *symbol_;
};
struct ExportDeclaration {
  ExportDeclaration (const Symbol &symbol) : ExportDeclaration (Key{}, &symbol) {}
  const Symbol &symbol () const { return *symbol_; }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  ExportDeclaration (Key, const Symbol *symbol) : symbol_ (symbol) {}
  const Symbol *symbol_;
};
struct DataDeclaration {
  DataDeclaration (const Symbol &symbol) : DataDeclaration (Key{}, &symbol) {}
  const Symbol &symbol () const { return *symbol_; }

 private:
  friend class detail::NodeAccess;
  struct Key {};
  DataDeclaration (Key, const Symbol *symbol) : symbol_ (symbol) {}
  const Symbol *symbol_;
};
class ModuleItem {
  using Storage = std::variant<const Function *, const GlobalData *, const Symbol *,
                               ForwardDeclaration, DataDeclaration, ExportDeclaration>;

 public:
  ModuleItem (const Function &value) : value_ (&value) {}
  ModuleItem (const GlobalData &value) : value_ (&value) {}
  ModuleItem (const Symbol &value) : value_ (&value) {}
  ModuleItem (ForwardDeclaration value) : value_ (value) {}
  ModuleItem (DataDeclaration value) : value_ (value) {}
  ModuleItem (ExportDeclaration value) : value_ (value) {}
  template <class T>
    requires (!std::is_pointer_v<T>)
  OptionalRef<const T> get () const {
    if constexpr (std::is_same_v<T, Function> || std::is_same_v<T, GlobalData>
                  || std::is_same_v<T, Symbol>) {
      auto value = std::get_if<const T *> (&value_);
      return value ? OptionalRef<const T> (**value) : OptionalRef<const T>{};
    } else
      return ::mirnext::detail::borrow_optional (std::get_if<T> (&value_));
  }

 private:
  friend class detail::NodeAccess;
  Storage value_;
};

class Module {
 public:
  Module (const Module &) = delete;
  Module &operator= (const Module &) = delete;
  Program &program ();
  const Program &program () const;
  std::string_view name () const { return name_; }
  auto functions () const { return nodes (functions_); }
  auto functions () { return mutable_nodes (functions_); }
  auto data () const { return nodes (data_); }
  auto symbols () const { return nodes (symbols_); }
  std::span<const ModuleItem> items () const { return items_; }
  OptionalRef<const Symbol> find_symbol (std::string_view) const;
  OptionalRef<const GlobalVariable> find_variable (std::string_view name) const {
    auto symbol = find_symbol (name).get ();
    return ::mirnext::detail::borrow_optional (symbol ? symbol->variable ().get () : nullptr);
  }

  OptionalRef<const Error> error () const {
    return error_ ? OptionalRef<const Error> (*error_) : OptionalRef<const Error>{};
  }
  std::uint64_t data_revision () const { return data_revision_; }
  Revisions revisions () const {
    auto result = revisions_;
    result.module_data = data_revision_;
    return result;
  }
  Result<void> verify () const;

 private:
  friend class detail::ModuleAccess;
  Result<GlobalData &> define_data (Symbol &, DataInitializer, bool writable = true);
  Result<GlobalData &> create_data (std::string_view, DataInitializer, bool writable = true);
  Result<Symbol &> declare_data (std::string_view);
  Result<Symbol &> prototype (std::string_view, Signature);
  Result<const GlobalVariable &> import_variable (std::string_view, Type, bool writable = true);
  Result<const GlobalVariable &> create_variable (std::string_view, const Function &initializer,
                                                  bool writable = true);
  Result<const GlobalVariable &> create_variable (std::string_view, Type,
                                                  std::optional<Operand> initial = {},
                                                  bool writable = true);
  Result<Symbol &> forward (std::string_view);
  Result<Function &> define_function (Symbol &,
                                      std::span<const std::string_view> argument_names = {});
  Result<Symbol &> reserve_symbol (std::string_view);
  Result<void> rename_symbol (const Symbol &, std::string_view);
  Result<const GlobalVariable &> bind_variable (const Symbol &, Type, bool writable = true);
  Result<void> export_symbol (const Symbol &);
  Result<GlobalData &> create_literal (Operand);
  Result<Function &> create_function (std::string_view, Signature,
                                      std::span<const std::string_view> argument_names = {});
  Result<Symbol &> declare_function (std::string_view, Signature);
  Result<Symbol &> import (std::string_view, std::optional<Signature> = {});
  friend class Program;
  friend class InstructionBuilder;
  friend class IRCloner;
  friend class Function;
  friend class Symbol;
  Module (Program &, std::string_view);
  Result<Symbol &> symbol (std::string_view, Symbol::Kind, bool allow_forward);
  Error fail (Error);
  detail::ProgramStorage *storage_;
  std::string_view name_;
  std::vector<std::unique_ptr<Symbol>> symbols_;
  std::vector<std::unique_ptr<Function>> functions_;
  std::vector<std::unique_ptr<GlobalData>> data_;
  std::vector<ModuleItem> items_;
  struct FunctionDataUses {
    std::vector<const GlobalData *> labels, expressions;
  };
  std::unordered_map<const Function *, FunctionDataUses> function_data_uses_;
  std::unordered_map<const Symbol *, std::vector<const GlobalData *>> symbol_data_uses_;
  StringMap<Symbol *> names_;
  std::optional<Error> error_;
  std::uint64_t data_revision_ = 0;
  Revisions revisions_;
  std::uint64_t next_literal_ = 0;
};

class Program {
 public:
  Program ();
  Program (Program &&) noexcept;
  Program &operator= (Program &&) noexcept;
  ~Program ();
  Program (const Program &) = delete;
  Program &operator= (const Program &) = delete;

  NodeRange<NodeIterator<std::vector<std::unique_ptr<Module>>::const_iterator, true>> modules ()
    const;
  NodeRange<NodeIterator<std::vector<std::unique_ptr<Module>>::iterator, false>> modules ();
  OptionalRef<const Module> find_module (std::string_view) const;
  const Type &intern_type (Type);
  Result<const Signature &> intern_signature (Signature);
  std::string_view intern_string (std::string_view);
  Result<void> status () const;
  Result<void> verify () const;

 private:
  template <class, class>
  friend class mirnext::analysis::detail::AnalysisCache;
  Result<void> analysis_status (bool allow_pending) const;
  friend class detail::ProgramAccess;
  Result<Module &> create_module (std::string_view);
  friend class Module;
  friend struct detail::BuildState;
  std::unique_ptr<detail::ProgramStorage> storage_;
};

bool output_operand (const Function &, const Instruction &, std::size_t);
Result<Program> clone (const Program &);
Result<Program> clone (const Module &);

std::string dump (const Program &);
std::string dump (const Function &);
bool needs_materialization (const Program &);
Result<Program> materialize (const Program &);
}  // namespace mirnext::ir
