/* Licensed under the MIT License. Canonical register IR, independent of the C engine. */
#pragma once
#include "mirnext/Support/Result.hpp"
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

namespace mirnext::ir {
class Context;
class Module;
class Function;
class BasicBlock;
class Instruction;
class Label;
class Symbol;
class IRBuilder;
class IRRewriter;
class IRCloner;
class SSAForm;
class SSARewriter;
class StackScope;
class GlobalVariable;
class GlobalData;
class Expression;
enum class Comparison { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
struct MaterializationStats {
  std::size_t saves = 0, restores = 0, cleanup_blocks = 0, indirect_dispatches = 0;
};
Result<MaterializationStats> materialize_stack_scopes(Function &);
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view value) const noexcept { return std::hash<std::string_view>{}(value); }
};
template<class T> using StringMap = std::unordered_map<std::string_view, T, StringHash, std::equal_to<>>;

class Type {
public:
  enum class Kind { Bool, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64, LongDouble,
                    Pointer, Block, ReturnBlock, Undef };
  enum class BlockClass { Default, Class1, Class2, Class3, Class4 };
  constexpr explicit Type(Kind kind) : kind_(kind) {}
  static constexpr Type i64() { return Type(Kind::I64); }
  static constexpr Type pointer() { return Type(Kind::Pointer); }
  static constexpr Type block(std::size_t size, BlockClass classification = BlockClass::Default) {
    return Type(Kind::Block, size, classification);
  }
  static constexpr Type return_block(std::size_t size) { return Type(Kind::ReturnBlock, size, BlockClass::Default); }
  constexpr Kind kind() const { return kind_; }
  constexpr std::size_t block_size() const { return size_; }
  constexpr BlockClass block_class() const { return class_; }
  constexpr bool is_block() const { return kind_ == Kind::Block || kind_ == Kind::ReturnBlock; }
  constexpr bool is_float() const { return kind_ >= Kind::F32 && kind_ <= Kind::LongDouble; }
  constexpr bool is_integer() const { return kind_ <= Kind::U64 || kind_ == Kind::Pointer; }
  constexpr bool is_scalar() const { return is_integer() || is_float(); }
  bool valid() const noexcept;
  friend auto operator<=>(const Type &, const Type &) = default;
private:
  constexpr Type(Kind kind, std::size_t size, BlockClass classification)
    : kind_(kind), size_(size), class_(classification) {}
  Kind kind_;
  std::size_t size_ = 0;
  BlockClass class_ = BlockClass::Default;
};

struct Signature {
  std::vector<Type> results, parameters;
  bool variadic = false;
  Result<void> verify() const;
  friend auto operator<=>(const Signature &, const Signature &) = default;
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
std::span<const InstructionDescriptor> instruction_descriptors() noexcept;
const InstructionDescriptor *instruction_descriptor(Opcode) noexcept;

// Views expose node iterators, never their owning container. In particular a
// mutable transform_view would leak its unique_ptr container through base().
template<class Iterator, bool Const> class NodeIterator {
public:
  using value_type = typename std::iterator_traits<Iterator>::value_type::element_type;
  using difference_type = typename std::iterator_traits<Iterator>::difference_type;
  using iterator_category = std::bidirectional_iterator_tag;
  using reference = std::conditional_t<Const, const value_type &, value_type &>;
  using pointer = std::conditional_t<Const, const value_type *, value_type *>;
  NodeIterator() = default;
  explicit NodeIterator(Iterator iterator) : iterator_(iterator) {}
  reference operator*() const { return **iterator_; }
  pointer operator->() const { return iterator_->get(); }
  NodeIterator &operator++() { ++iterator_; return *this; }
  NodeIterator operator++(int) { auto old = *this; ++*this; return old; }
  NodeIterator &operator--() { --iterator_; return *this; }
  NodeIterator operator--(int) { auto old = *this; --*this; return old; }
  friend bool operator==(const NodeIterator &, const NodeIterator &) = default;
private:
  Iterator iterator_;
};
template<class Iterator> class NodeRange {
public:
  NodeRange(Iterator first, Iterator last) : first_(first), last_(last) {}
  Iterator begin() const { return first_; }
  Iterator end() const { return last_; }
private:
  Iterator first_, last_;
};
template<class Container> auto nodes(const Container &values) {
  using Iterator = NodeIterator<typename Container::const_iterator, true>;
  return NodeRange(Iterator(values.begin()), Iterator(values.end()));
}
template<class Container> auto mutable_nodes(Container &values) {
  using Iterator = NodeIterator<typename Container::iterator, false>;
  return NodeRange(Iterator(values.begin()), Iterator(values.end()));
}

class Register {
public:
  Register(const Register &) = delete;
  Register &operator=(const Register &) = delete;
  const Function &function() const { return *function_; }
  Type type() const { return type_; }
  std::string_view name() const { return name_; }
  std::string_view hardware_binding() const { return hardware_; }
  bool is_argument() const { return argument_; }
  bool is_memory_backed() const { return memory_backed_; }
private:
  friend class Function;
  friend class IRBuilder;
  friend class IRRewriter;
  friend class IRCloner;
  Register(Function &, Type, std::string_view, std::string_view, bool);
  Function *function_;
  Type type_;
  std::string_view name_, hardware_;
  bool argument_;
  bool memory_backed_ = false;
};

class Label {
public:
  Label(const Label &) = delete;
  Label &operator=(const Label &) = delete;
  const Function &function() const { return *function_; }
  const BasicBlock &block() const { return *block_; }
  std::string_view name() const { return name_; }
private:
  friend class Function;
  friend class IRRewriter;
  Label(Function &, BasicBlock &, std::string_view);
  Function *function_;
  BasicBlock *block_;
  std::string_view name_;
};

// An optimizer-owned memory equivalence class, independent of SSA values and
// final register allocation. It is analysis provenance, never file-format data.
class MemoryLocation {
public:
  MemoryLocation(const MemoryLocation &) = delete;
  MemoryLocation &operator=(const MemoryLocation &) = delete;
  const Function &function() const { return *function_; }
  std::size_t creation_order() const { return order_; }
private:
  friend class IRRewriter;
  MemoryLocation(Function &function,std::size_t order) : function_(&function),order_(order) {}
  Function *function_;
  std::size_t order_;
};
struct Memory {
  Memory(Type type, const Register *base = nullptr, const Register *index = nullptr, std::int64_t displacement = 0,
         unsigned scale = 1, std::string alias = {}, std::string nonalias = {})
    : type(type), base(base), index(index), displacement(displacement), scale(scale), alias(std::move(alias)), nonalias(std::move(nonalias)) {}
  Type type;
  const Register *base = nullptr, *index = nullptr;
  std::int64_t displacement = 0;
  unsigned scale = 1;
  std::string alias, nonalias;
  const GlobalData *literal_source() const { return literal_source_; }
  const MemoryLocation *location() const { return location_; }
  friend bool operator==(const Memory &a,const Memory &b) {
    return a.type == b.type && a.base == b.base && a.index == b.index && a.displacement == b.displacement
      && a.scale == b.scale && a.alias == b.alias && a.nonalias == b.nonalias && a.literal_source_ == b.literal_source_;
  }
private:
  friend class IRRewriter;
  friend class IRCloner;
  const GlobalData *literal_source_ = nullptr;
  const MemoryLocation *location_ = nullptr;
};
struct SymbolReference {
  const Symbol *symbol;
  std::int64_t displacement = 0;
  friend bool operator==(const SymbolReference &, const SymbolReference &) = default;
};
class Operand {
public:
  using Storage = std::variant<std::int64_t, std::uint64_t, float, double, long double,
                               std::string, const Register *, Memory, const Label *, SymbolReference>;
  Operand(std::int64_t value) : value_(value) {}
  Operand(std::uint64_t value) : value_(value) {}
  template<class Integer> requires (std::is_integral_v<Integer> && sizeof(Integer) <= sizeof(std::uint64_t))
  Operand(Integer value) : value_(static_cast<std::conditional_t<std::is_signed_v<Integer> || std::is_same_v<Integer, bool>,
                                                               std::int64_t, std::uint64_t>>(value)) {}
  Operand(float value) : value_(value) {}
  Operand(double value) : value_(value) {}
  Operand(long double value) : value_(value) {}
  Operand(std::string value) : value_(std::move(value)) {}
  Operand(const Register &value) : value_(&value) {}
  Operand(Memory value) : value_(std::move(value)) {}
  Operand(const Label &value) : value_(&value) {}
  Operand(const Symbol &value) : value_(SymbolReference{&value}) {}
  Operand(SymbolReference value) : value_(value) {}
  template<class T> const T *get() const { return std::get_if<T>(&value_); }
  const Storage &storage() const { return value_; }
  friend bool operator==(const Operand &, const Operand &) = default;
private:
  friend class IRRewriter;
  Storage value_;
};

class Expression {
public:
  Type type() const { return type_; }
  bool is_valid() const;
  const Function &function() const;
  Result<Operand> value() const;
  Expression convert(Type) const;
  Expression operator-() const;
  Expression operator~() const;
  Expression operator!() const;
#define MIRNEXT_EXPRESSION_OPERATOR(symbol, operation) \
  friend Expression operator symbol(const Expression &lhs, const Expression &rhs) { return lhs.apply(Operation::operation, rhs); } \
  template<class T> requires (std::is_arithmetic_v<T> && std::is_constructible_v<Operand, T>) \
  friend Expression operator symbol(const Expression &lhs, T rhs) { return lhs.apply(Operation::operation, lhs.literal_like(Operand(rhs))); } \
  template<class T> requires (std::is_arithmetic_v<T> && std::is_constructible_v<Operand, T>) \
  friend Expression operator symbol(T lhs, const Expression &rhs) { return rhs.literal_like(Operand(lhs)).apply(Operation::operation, rhs); }
  MIRNEXT_EXPRESSION_OPERATOR(+, Add)
  MIRNEXT_EXPRESSION_OPERATOR(-, Sub)
  MIRNEXT_EXPRESSION_OPERATOR(*, Mul)
  MIRNEXT_EXPRESSION_OPERATOR(/, Div)
  MIRNEXT_EXPRESSION_OPERATOR(%, Mod)
  MIRNEXT_EXPRESSION_OPERATOR(&, And)
  MIRNEXT_EXPRESSION_OPERATOR(|, Or)
  MIRNEXT_EXPRESSION_OPERATOR(^, Xor)
  MIRNEXT_EXPRESSION_OPERATOR(<<, LeftShift)
  MIRNEXT_EXPRESSION_OPERATOR(>>, RightShift)
  MIRNEXT_EXPRESSION_OPERATOR(==, Equal)
  MIRNEXT_EXPRESSION_OPERATOR(!=, NotEqual)
  MIRNEXT_EXPRESSION_OPERATOR(<, Less)
  MIRNEXT_EXPRESSION_OPERATOR(<=, LessEqual)
  MIRNEXT_EXPRESSION_OPERATOR(>, Greater)
  MIRNEXT_EXPRESSION_OPERATOR(>=, GreaterEqual)
#undef MIRNEXT_EXPRESSION_OPERATOR
private:
  friend class IRBuilder;
  enum class Operation { Add, Sub, Mul, Div, Mod, And, Or, Xor, LeftShift, RightShift,
                         Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };
  Expression(IRBuilder &, Type, std::optional<Operand>);
  Expression apply(Operation, const Expression &) const;
  Expression literal_like(Operand) const;
  IRBuilder *builder_;
  Type type_;
  using Scalar = std::variant<std::int64_t, std::uint64_t, float, double, long double, const Register *>;
  std::optional<Scalar> value_;
};

class BlockArgument {
public:
  BlockArgument(const Register &address, std::size_t size,
                Type::BlockClass classification = Type::BlockClass::Default, std::int64_t displacement = 0)
    : memory_{Type::block(size, classification), &address, nullptr, displacement} {}
  BlockArgument(const Register &address, Type type, std::int64_t displacement = 0)
    : memory_{type, &address, nullptr, displacement} {}
  static BlockArgument return_buffer(const Register &address, std::size_t size) {
    return BlockArgument(address, Type::return_block(size));
  }
  const Memory &memory() const { return memory_; }
private:
  Memory memory_;
};

class CallArgument {
public:
  CallArgument(BlockArgument value) : value_(std::move(value)) {}
  CallArgument(Expression value) : value_(std::move(value)) {}
  template<class T> requires std::is_constructible_v<Operand, T>
  CallArgument(T &&value) : value_(Operand(std::forward<T>(value))) {}
  const auto &storage() const { return value_; }
private:
  std::variant<Operand, BlockArgument, Expression> value_;
};

class CallResult {
public:
  const Instruction &instruction() const { return *instruction_; }
  std::span<const Register *const> results() const { return results_; }
  Result<const Register *> value() const {
    if (results_.size() != 1) return Error{ErrorCode::InvalidOperand, "call does not have exactly one result"};
    return results_.front();
  }
private:
  friend class IRBuilder;
  CallResult(const Instruction &instruction, std::vector<const Register *> results)
    : instruction_(&instruction), results_(std::move(results)) {}
  const Instruction *instruction_;
  std::vector<const Register *> results_;
};

struct Revisions {
  std::uint64_t instructions = 0, control_flow = 0, registers = 0, module_data = 0;
  std::uint64_t symbols = 0;
  friend bool operator==(const Revisions &, const Revisions &) = default;
};

class Instruction {
public:
  class ConstructionKey {
    friend class IRBuilder;
    friend class IRRewriter;
    ConstructionKey() = default;
  };
  Instruction(ConstructionKey, BasicBlock &block, Opcode opcode, std::vector<Operand> operands)
    : block_(&block), opcode_(opcode), operands_(std::move(operands)) {}
  Instruction(const Instruction &) = delete;
  Instruction &operator=(const Instruction &) = delete;
  Opcode opcode() const { return opcode_; }
  std::span<const Operand> operands() const { return operands_; }
  const BasicBlock &block() const { return *block_; }
  const Function &function() const;
private:
  friend class IRBuilder;
  friend class IRRewriter;
  BasicBlock *block_;
  Opcode opcode_;
  std::vector<Operand> operands_;
  // List iterators survive splice, so rewrites never need to scan for this node.
  std::list<Instruction>::iterator position_;
};

class BasicBlock {
public:
  BasicBlock(const BasicBlock &) = delete;
  BasicBlock &operator=(const BasicBlock &) = delete;
  const Function &function() const { return *function_; }
  std::string_view name() const { return name_; }
  auto instructions() const { return NodeRange(instructions_.cbegin(), instructions_.cend()); }
  auto instructions() { return NodeRange(instructions_.begin(), instructions_.end()); }
  std::span<const Label *const> labels() const { return labels_; }
  std::size_t size() const { return instructions_.size(); }
  const Instruction *terminator() const;
  const StackScope *stack_scope() const { return scope_; }
private:
  friend class Function;
  friend class IRBuilder;
  friend class IRRewriter;
  BasicBlock(Function &, std::string_view);
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
  StackScope(const StackScope &) = delete;
  StackScope &operator=(const StackScope &) = delete;
  const Function &function() const { return *function_; }
  const StackScope *parent() const { return parent_; }
  std::string_view name() const { return name_; }
  bool materialized() const { return materialized_; }
  std::size_t depth() const { return depth_; }
private:
  friend class Function;
  friend Result<MaterializationStats> materialize_stack_scopes(Function &);
  friend class IRCloner;
  StackScope(Function &function, const StackScope *parent, std::string_view name)
    : function_(&function), parent_(parent), name_(name), depth_(parent ? parent->depth() + 1 : 1) {}
  Function *function_;
  const StackScope *parent_;
  std::string_view name_;
  bool materialized_ = false;
  std::size_t depth_;
};

class Function {
public:
  ~Function();
  Function(const Function &) = delete;
  Function &operator=(const Function &) = delete;
  Module &module() { return *module_; }
  const Module &module() const { return *module_; }
  const Symbol &symbol() const { return *symbol_; }
  const Signature &signature() const;
  auto blocks() const { return nodes(blocks_); }
  auto blocks() { return mutable_nodes(blocks_); }
  auto registers() const { return nodes(registers_); }
  bool has_hardware_bindings() const noexcept { return !hardware_registers_.empty(); }
  auto labels() const { return nodes(labels_); }
  auto memory_locations() const { return nodes(memory_locations_); }
  auto stack_scopes() const { return nodes(scopes_); }
  std::span<const Register *const> arguments() const { return arguments_; }
  // Borrowed data-use lists share the module's immutable initializer objects.
  std::span<const GlobalData *const> label_data_uses() const;
  std::span<const GlobalData *const> expression_data_uses() const;
  Result<BasicBlock *> create_block(std::string_view name = {}, const StackScope * = nullptr);
  Result<BasicBlock *> create_block_after(BasicBlock &, std::string_view name = {}, const StackScope * = nullptr);
  Result<BasicBlock *> create_block_before(BasicBlock &, std::string_view name = {}, const StackScope * = nullptr);
  Result<StackScope *> create_stack_scope(const StackScope *parent = nullptr, std::string_view name = {});
  bool needs_materialization() const;
  Result<Label *> create_label(BasicBlock &, std::string_view name = {});
  Result<Register *> create_register(Type, std::string_view name = {}, std::string_view hardware = {});
  const BasicBlock *find_block(std::string_view) const;
  const Register *find_register(std::string_view) const;
  const Label *find_label(std::string_view) const;
  Revisions revisions() const;
  std::size_t instruction_count() const;
  Result<void> verify() const;
  const SSAForm *ssa_form() const { return ssa_.get(); }
  Result<void> require_register_ir() const;
private:
  friend class Module;
  friend class IRBuilder;
  friend class IRRewriter;
  friend class SSARewriter;
  friend Result<MaterializationStats> materialize_stack_scopes(Function &);
  friend class IRCloner;
  Function(Module &, Symbol &);
  Result<Register *> add_register(Type, std::string_view, std::string_view, bool);
  void changed(bool cfg);
  void registers_changed();
  Module *module_;
  Symbol *symbol_;
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

struct InitializedData { Type type; std::vector<Operand> values; };
struct ZeroData { std::size_t size; };
struct LabelReference { const Label *label; const Label *base = nullptr; std::int64_t displacement = 0; };
struct ExpressionData { const Function *function; };
using DataInitializer = std::variant<InitializedData, ZeroData, SymbolReference, LabelReference, ExpressionData>;
class GlobalData {
public:
  GlobalData(const GlobalData &) = delete;
  GlobalData &operator=(const GlobalData &) = delete;
  const Symbol &symbol() const { return *symbol_; }
  const DataInitializer &initializer() const { return initializer_; }
  bool writable() const { return writable_; }
  bool is_literal_pool() const { return literal_pool_; }
private:
  friend class Module;
  friend class IRCloner;
  GlobalData(Symbol &symbol, DataInitializer initializer, bool writable)
    : symbol_(&symbol), initializer_(std::move(initializer)), writable_(writable) {}
  Symbol *symbol_;
  DataInitializer initializer_;
  bool writable_;
  bool literal_pool_ = false;
};

class GlobalVariable {
public:
  GlobalVariable(const GlobalVariable &) = delete;
  GlobalVariable &operator=(const GlobalVariable &) = delete;
  const Symbol &symbol() const { return *symbol_; }
  Type type() const { return type_; }
  bool writable() const { return writable_; }
private:
  friend class Module;
  GlobalVariable(const Symbol &symbol, Type type, bool writable) : symbol_(&symbol), type_(type), writable_(writable) {}
  const Symbol *symbol_;
  Type type_;
  bool writable_;
};

class Symbol {
public:
  Symbol(const Symbol &) = delete;
  Symbol &operator=(const Symbol &) = delete;
  enum class Kind { Forward, Function, Data, Prototype, Import };
  const Module &module() const { return *module_; }
  std::string_view name() const { return name_; }
  Kind kind() const { return kind_; }
  const Signature *signature() const { return signature_; }
  const Function *function() const { return function_; }
  const GlobalData *data() const { return data_; }
  const GlobalVariable *variable() const { return variable_.get(); }
  std::span<const GlobalData *const> reference_data_uses() const;
  bool exported() const { return exported_; }
private:
  friend class Module;
  friend class IRCloner;
  Symbol(Module &, std::string_view, Kind);
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
struct ForwardDeclaration { const Symbol *symbol; };
struct ExportDeclaration { const Symbol *symbol; };
struct DataDeclaration { const Symbol *symbol; };
using ModuleItem = std::variant<const Function *, const GlobalData *, const Symbol *, ForwardDeclaration, ExportDeclaration, DataDeclaration>;

class Module {
public:
  Module(const Module &) = delete;
  Module &operator=(const Module &) = delete;
  Context &context() { return *context_; }
  const Context &context() const { return *context_; }
  std::string_view name() const { return name_; }
  auto functions() const { return nodes(functions_); }
  auto functions() { return mutable_nodes(functions_); }
  auto data() const { return nodes(data_); }
  auto symbols() const { return nodes(symbols_); }
  std::span<const ModuleItem> items() const { return items_; }
  const Symbol *find_symbol(std::string_view) const;
  const GlobalVariable *find_variable(std::string_view name) const {
    auto symbol = find_symbol(name); return symbol ? symbol->variable() : nullptr;
  }
  Result<Symbol *> reserve_symbol(std::string_view);
  Result<Symbol *> forward(std::string_view);
  Result<Symbol *> declare_function(std::string_view, Signature);
  Result<Function *> define_function(Symbol &, std::span<const std::string_view> argument_names = {});
  Result<Function *> create_function(std::string_view, Signature, std::span<const std::string_view> argument_names = {});
  Result<Symbol *> prototype(std::string_view, Signature);
  Result<Symbol *> import(std::string_view, std::optional<Signature> = {});
  Result<Symbol *> declare_data(std::string_view);
  Result<GlobalData *> define_data(Symbol &, DataInitializer, bool writable = true);
  Result<GlobalData *> create_data(std::string_view, DataInitializer, bool writable = true);
  Result<GlobalData *> create_literal(Operand);
  Result<const GlobalVariable *> create_variable(std::string_view, Type, std::optional<Operand> initial = {}, bool writable = true);
  Result<const GlobalVariable *> create_variable(std::string_view, const Function &initializer, bool writable = true);
  Result<const GlobalVariable *> import_variable(std::string_view, Type, bool writable = true);
  Result<const GlobalVariable *> bind_variable(const Symbol &, Type, bool writable = true);
  Result<void> export_symbol(const Symbol &);
  Result<void> rename_symbol(const Symbol &, std::string_view);
  const std::optional<Error> &error() const { return error_; }
  std::uint64_t data_revision() const { return data_revision_; }
  Revisions revisions() const { auto result = revisions_; result.module_data = data_revision_; return result; }
  Result<void> verify() const;
private:
  friend class Context;
  friend class IRBuilder;
  friend class IRCloner;
  friend class Function;
  friend class Symbol;
  Module(Context &, std::string_view);
  Result<Symbol *> symbol(std::string_view, Symbol::Kind, bool allow_forward);
  Error fail(Error);
  Context *context_;
  std::string_view name_;
  std::vector<std::unique_ptr<Symbol>> symbols_;
  std::vector<std::unique_ptr<Function>> functions_;
  std::vector<std::unique_ptr<GlobalData>> data_;
  std::vector<ModuleItem> items_;
  struct FunctionDataUses { std::vector<const GlobalData *> labels, expressions; };
  std::unordered_map<const Function *, FunctionDataUses> function_data_uses_;
  std::unordered_map<const Symbol *, std::vector<const GlobalData *>> symbol_data_uses_;
  StringMap<Symbol *> names_;
  std::optional<Error> error_;
  std::uint64_t data_revision_ = 0;
  Revisions revisions_;
  std::uint64_t next_literal_ = 0;
};

class Context {
public:
  Context() = default;
  Context(const Context &) = delete;
  Context &operator=(const Context &) = delete;
  Result<Module *> create_module(std::string_view);
  auto modules() const { return nodes(modules_); }
  auto modules() { return mutable_nodes(modules_); }
  const Module *find_module(std::string_view) const;
  const Type &intern_type(Type);
  Result<const Signature *> intern_signature(Signature);
  std::string_view intern_string(std::string_view);
  Result<void> verify() const;
private:
  // Interned identifiers and types outlive every module that references them.
  std::set<std::string, std::less<>> strings_;
  std::set<Type> types_;
  std::set<Signature> signatures_;
  std::vector<std::unique_ptr<Module>> modules_;
};

Result<void> validate_instruction(const Function &, Opcode, std::span<const Operand>);
Result<void> validate_initializer(const Module &, const DataInitializer &);
Result<void> validate_variable_binding(const Symbol &, Type, bool writable);
bool output_operand(const Function &, const Instruction &, std::size_t);
Result<std::unique_ptr<Context>> clone(const Context &);
Result<std::unique_ptr<Context>> clone(const Module &);

class IRBuilder {
public:
  explicit IRBuilder(Function &function) : function_(&function) {}
  explicit IRBuilder(BasicBlock &block) : function_(block.function_), block_(&block), scope_(block.scope_) {}
  Result<void> set_insert_point(BasicBlock &);
  void clear_insert_point() { block_ = nullptr; }
  const BasicBlock *insert_block() const { return block_; }
  const StackScope *stack_scope() const { return scope_; }
  Result<BasicBlock *> create_block(std::string_view name = {});
  Result<StackScope *> begin_stack_scope(std::string_view name = {});
  Result<void> end_stack_scope(const StackScope &);
  Result<Instruction *> emit(Opcode, std::span<const Operand>);
  Result<Instruction *> emit(Opcode opcode, std::initializer_list<Operand> operands) {
    return emit(opcode, std::span<const Operand>(operands.begin(), operands.size()));
  }
  Result<const Register *> binary(Opcode, Operand lhs, Operand rhs, std::string_view name = {});
  Expression expression(const Register &);
  Expression expression(Memory);
  Expression expression(const GlobalVariable &);
  Expression literal(Operand);
  template<class Boolean> requires std::is_same_v<std::remove_cvref_t<Boolean>, bool>
  Expression literal(Boolean value) { return literal(Type(Type::Kind::Bool), Operand(value)); }
  Expression literal(Type, Operand);
  Result<Instruction *> assign(const Register &, Operand);
  Result<Instruction *> assign(const Register &, const Expression &);
  Result<const Register *> unary(Opcode, Operand, std::string_view name = {});
  Result<const Register *> allocate_stack(Operand size, std::string_view name = {});
  Result<const Register *> stack_save(std::string_view name = {});
  Result<Instruction *> stack_restore(const Register &);
  Result<const Register *> address_of(const Register &, std::string_view name = {});
  Result<const Register *> address_of(const Symbol &, std::int64_t displacement = 0, std::string_view name = {});
  Result<const Register *> address_of(const Label &, std::string_view name = {});
  Result<const Register *> address_of(const GlobalVariable &, std::string_view name = {});
  Result<const Register *> load(Memory, std::string_view name = {});
  Result<const Register *> load(const GlobalVariable &, std::string_view name = {});
  Result<Instruction *> store(Memory, Operand);
  Result<Instruction *> store(Memory, const Expression &);
  Result<Instruction *> store(const GlobalVariable &, Operand);
  Result<Instruction *> store(const GlobalVariable &, const Expression &);
  Result<Instruction *> branch_if(Operand, const Label &, bool when_true = true);
  Result<Instruction *> branch_if(const Expression &, const Label &, bool when_true = true);
  Result<Instruction *> branch(Comparison, const Expression &, const Expression &, const Label &);
  Result<Instruction *> switch_to(Operand, std::span<const Label *const>);
  Result<CallResult> call(const Symbol &callee, std::span<const CallArgument> arguments = {});
  Result<CallResult> call(const Function &callee, std::span<const CallArgument> arguments = {}) {
    return call(callee.symbol(), arguments);
  }
  Result<CallResult> call(const Function &callee, std::initializer_list<CallArgument> arguments) {
    return call(callee.symbol(), std::span<const CallArgument>(arguments.begin(), arguments.size()));
  }
  Result<CallResult> call_indirect(const Symbol &signature, Operand callee, std::span<const CallArgument> arguments = {});
  Result<CallResult> call(const Symbol &callee, std::initializer_list<CallArgument> arguments) {
    return call(callee, std::span<const CallArgument>(arguments.begin(), arguments.size()));
  }
  Result<CallResult> call_indirect(const Symbol &signature, Operand callee, std::initializer_list<CallArgument> arguments) {
    return call_indirect(signature, std::move(callee), std::span<const CallArgument>(arguments.begin(), arguments.size()));
  }
  Result<CallResult> inline_call(const Symbol &callee, std::span<const CallArgument> arguments = {});
  Result<CallResult> inline_call(const Function &callee, std::span<const CallArgument> arguments = {}) {
    return inline_call(callee.symbol(), arguments);
  }
  Result<CallResult> inline_call(const Function &callee, std::initializer_list<CallArgument> arguments) {
    return inline_call(callee.symbol(), std::span<const CallArgument>(arguments.begin(), arguments.size()));
  }
  Result<CallResult> inline_call(const Symbol &callee, std::initializer_list<CallArgument> arguments) {
    return inline_call(callee, std::span<const CallArgument>(arguments.begin(), arguments.size()));
  }
  Result<Instruction *> jump_call(const Symbol &callee);
  Result<Instruction *> jump_call(const Function &callee) { return jump_call(callee.symbol()); }
  Result<Instruction *> jump_call(const Symbol &signature, Operand callee);
  Result<Instruction *> ret_to(Operand continuation);
  Result<Instruction *> start_varargs(Operand list);
  Result<Instruction *> end_varargs(Operand list);
  Result<const Register *> next_vararg_address(Type, Operand list, std::string_view name = {});
  Result<const Register *> next_vararg(Type, Operand list, std::string_view name = {});
  Result<Instruction *> next_vararg_block(const Register &destination, Operand list, Type block_type);
  Result<Instruction *> jump(const Label &);
  Result<Instruction *> ret(std::span<const Operand> = {});
  Result<Instruction *> ret(Operand operand) { return emit(Opcode::Ret, {std::move(operand)}); }
  Result<Instruction *> ret(const Expression &);
private:
  friend class Expression;
  Error fail(Error);
  Function *function_;
  BasicBlock *block_ = nullptr;
  const StackScope *scope_ = nullptr;
  Result<const Register *> value_operation(Opcode, std::span<const Operand>, Type, std::string_view);
  Result<CallResult> build_call(Opcode, const Symbol &, Operand, std::span<const CallArgument>);
  Instruction *append_validated(Opcode, std::vector<Operand>);
  Result<Instruction *> access_variable(const GlobalVariable &, std::optional<Operand> stored, std::string_view);
  Result<Operand> expression_operand(const Expression &, std::optional<Type> expected = {});
  Expression convert_expression(const Expression &, Type);
};

class IRRewriter {
public:
  explicit IRRewriter(Function &function) : function_(function) {}
  const MemoryLocation &create_memory_location();
  // Annotation edits do not change operands, SSA uses or executable semantics.
  Result<void> set_memory_location(Instruction &,std::size_t,const MemoryLocation *);
  Result<void> replace_operand(Instruction &, std::size_t, Operand);
  Result<void> replace(Instruction &, Opcode, std::span<const Operand>);
  Result<void> erase(Instruction &);
  Result<void> move_before(Instruction &, BasicBlock &, const Instruction *before = nullptr);
  Result<void> move_block_before(BasicBlock &, BasicBlock *before = nullptr);
  Result<void> replace_uses(const Register &, const Register &);
  // A null anchor appends. Terminators can only be appended to an open block.
  Result<Instruction *> insert(BasicBlock &, const Instruction *before, Opcode, std::span<const Operand>);
  Result<Instruction *> insert(BasicBlock &, const Instruction *before, Opcode, std::vector<Operand> &&);
  Result<Instruction *> insert(BasicBlock &block, const Instruction *before, Opcode opcode, std::initializer_list<Operand> operands) {
    return insert(block, before, opcode, std::span<const Operand>(operands.begin(), operands.size()));
  }
  Result<Instruction *> insert_before(Instruction &, Opcode, std::span<const Operand>);
  Result<Instruction *> insert_before(Instruction &i, Opcode opcode, std::initializer_list<Operand> operands) {
    return insert_before(i, opcode, std::span<const Operand>(operands.begin(), operands.size()));
  }
  Result<Instruction *> insert_after(Instruction &, Opcode, std::span<const Operand>);
  Result<Instruction *> insert_after(Instruction &i, Opcode opcode, std::initializer_list<Operand> operands) {
    return insert_after(i, opcode, std::span<const Operand>(operands.begin(), operands.size()));
  }
  // Split transfers the suffix without moving labels or copying instructions.
  Result<BasicBlock *> split_before(Instruction &, std::string_view name = {});
  // Merge consumes the following unlabeled block on the same stack scope.
  // Only the consumed block and erased labels invalidate their own references.
  Result<void> merge_fallthrough(BasicBlock &);
  // Separate co-located anchors without copying instructions or labels.
  Result<std::size_t> separate_label_anchors(BasicBlock &);
  // Validate the whole removal set before erasing mutually referencing blocks.
  Result<void> erase_blocks(std::span<BasicBlock *const>);
  Result<std::size_t> erase_unused_labels();
  Result<Memory> literal_memory(const GlobalData &, const Register &) const;
  BasicBlock *next_block(const BasicBlock &) const;
  // Stays inside the block. A null result denotes the block's layout boundary.
  Instruction *next_instruction(const Instruction &) const;
  Result<void> set_stack_scope(BasicBlock &, const StackScope *);
  // Address lowering records storage identity without changing its binding.
  Result<void> materialize_register_storage(std::span<const Register *const>);
  Result<void> materialize_register_storage(const Register &reg) {
    const Register *registers[]{&reg}; return materialize_register_storage(registers);
  }
private:
  friend class SSARewriter;
  IRRewriter(Function &function, bool ssa_edit) : function_(function), ssa_edit_(ssa_edit) {}
  Result<void> writable() const;
  Result<BasicBlock *> insert_empty_block_before(BasicBlock &);
  Result<void> replace_owned(Instruction &, Opcode, std::vector<Operand>);
  Function &function_;
  bool ssa_edit_ = false;
};

std::string dump(const Context &);
std::string dump(const Function &);
bool needs_materialization(const Context &);
Result<MaterializationStats> materialize_stack_scopes(Context &);
Result<std::unique_ptr<Context>> materialize(const Context &);
} // namespace mirnext::ir
