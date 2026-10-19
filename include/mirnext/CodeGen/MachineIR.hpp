/* Licensed under the MIT License. Block-owned, typed machine instructions. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
#include <list>
#include <optional>
#include <unordered_set>

namespace mirnext::codegen {
namespace detail { struct EdgeSpillEngine; struct MachineEdgeEditor; struct PostAllocationEngine; struct FrameEngine; struct EncodingEngine; struct X86EncodingEngine; }
class MachineFunction;
class MachineBasicBlock;
class MachineRewriter;
enum class Builtin;
class MachineVirtualRegister {
public:
  MachineVirtualRegister(const MachineVirtualRegister &) = delete;
  MachineVirtualRegister &operator=(const MachineVirtualRegister &) = delete;
  const MachineFunction &function() const { return *function_; }
  ir::Type type() const { return type_; }
  ir::Type declared_type() const { return declared_; }
  std::string_view name() const { return name_; }
  bool is_argument() const { return argument_; }
  bool is_memory_backed() const { return memory_backed_; }
  std::optional<target::PhysicalRegister> binding() const { return binding_; }
private:
  friend class MachineFunction;
  MachineVirtualRegister(MachineFunction &, ir::Type, std::string, bool, bool, std::optional<target::PhysicalRegister>);
  MachineFunction *function_;
  ir::Type type_, declared_;
  std::string name_;
  bool argument_, memory_backed_;
  std::optional<target::PhysicalRegister> binding_;
};
using MachineRegister = std::variant<const MachineVirtualRegister *, target::PhysicalRegister>;
class MachineStackSlot {
public:
  MachineStackSlot(const MachineStackSlot &) = delete;
  MachineStackSlot &operator=(const MachineStackSlot &) = delete;
  const MachineFunction &function() const { return *function_; }
  std::size_t size() const { return size_; }
  std::size_t alignment() const { return alignment_; }
private:
  friend class MachineFunction;
  MachineStackSlot(MachineFunction &function, std::size_t size, std::size_t alignment)
    : function_(&function), size_(size), alignment_(alignment) {}
  MachineFunction *function_;
  std::size_t size_, alignment_;
};
class MachineLabel {
public:
  MachineLabel(const MachineLabel &) = delete;
  MachineLabel &operator=(const MachineLabel &) = delete;
  const MachineBasicBlock &block() const { return *block_; }
  std::string_view name() const { return name_; }
  bool address_taken() const { return address_taken_; }
  bool is_function_entry() const { return function_entry_; }
  std::optional<std::size_t> source_ordinal() const { return source_ordinal_; }
private:
  friend class MachineFunction;
  friend class MachineRewriter;
  MachineLabel(MachineBasicBlock &block, std::string name, bool address_taken,std::optional<std::size_t> source_ordinal,bool function_entry)
    : block_(&block), name_(std::move(name)), address_taken_(address_taken),function_entry_(function_entry),source_ordinal_(source_ordinal) {}
  MachineBasicBlock *block_;
  std::string name_;
  bool address_taken_;
  bool function_entry_;
  std::optional<std::size_t> source_ordinal_;
};
struct MachineSymbol {
  enum class Kind { Function, Data, Prototype, Import, Forward, BuiltinPrototype };
  std::string module, name;
  Kind kind = Kind::Forward;
  std::optional<ir::Signature> signature;
  bool exported = false;
  std::optional<std::size_t> anonymous_ordinal;
  std::optional<Builtin> builtin;
  friend bool operator==(const MachineSymbol &, const MachineSymbol &) = default;
};
struct MachineSymbolReference {
  const MachineSymbol *symbol = nullptr;
  std::int64_t displacement = 0;
  friend bool operator==(MachineSymbolReference, MachineSymbolReference) = default;
};
enum class Builtin { CopyBlock, CopyArgument, VariadicScalar, VariadicBlock, FloatToLongDouble,
  DoubleToLongDouble, IntegerToLongDouble, UnsignedToLongDouble, UnsignedToFloat, UnsignedToDouble,
  LongDoubleToFloat, LongDoubleToDouble, LongDoubleToInteger, LongDoubleAdd, LongDoubleSub,
  LongDoubleMul, LongDoubleDiv, LongDoubleNeg, LongDoubleEq, LongDoubleNe, LongDoubleLt,
  LongDoubleLe, LongDoubleGt, LongDoubleGe };
struct BuiltinReference { Builtin builtin; friend bool operator==(BuiltinReference, BuiltinReference) = default; };
class MachineMemoryLocation {
public:
  MachineMemoryLocation(const MachineMemoryLocation &) = delete;
  MachineMemoryLocation &operator=(const MachineMemoryLocation &) = delete;
  const MachineFunction &function() const { return *function_; }
  std::size_t creation_order() const { return order_; }
private:
  friend class MachineFunction;
  MachineMemoryLocation(MachineFunction &function,std::size_t order) : function_(&function),order_(order) {}
  MachineFunction *function_;
  std::size_t order_;
};
struct MachineMemory {
  ir::Type type;
  std::optional<MachineRegister> base, index;
  std::int64_t displacement = 0;
  unsigned scale = 1;
  std::string alias, nonalias;
  const MachineMemoryLocation *location = nullptr;
  friend bool operator==(const MachineMemory &a,const MachineMemory &b) {
    return a.type == b.type && a.base == b.base && a.index == b.index && a.displacement == b.displacement
      && a.scale == b.scale && a.alias == b.alias && a.nonalias == b.nonalias;
  }
};
struct StackSlotReference {
  const MachineStackSlot *slot = nullptr;
  std::int64_t displacement = 0;
  friend bool operator==(StackSlotReference, StackSlotReference) = default;
};
class MachineOperand {
public:
  using Storage = std::variant<std::int64_t, std::uint64_t, float, double, long double,
    const MachineVirtualRegister *, target::PhysicalRegister, MachineMemory,
    const MachineLabel *, MachineSymbolReference, StackSlotReference, BuiltinReference>;
  template<class T> requires std::is_constructible_v<Storage, T>
  explicit MachineOperand(T value) : storage_(std::move(value)) {}
  explicit MachineOperand(const MachineVirtualRegister &reg) : storage_(&reg) {}
  explicit MachineOperand(const MachineLabel &label) : storage_(&label) {}
  template<class T> const T *get() const { return std::get_if<T>(&storage_); }
  const Storage &storage() const { return storage_; }
  friend bool operator==(const MachineOperand &, const MachineOperand &) = default;
private:
  Storage storage_;
};
// USE is a backend liveness boundary (empty USE is the backend's no-op).
// Private SSE moves have no magic selector operand; alignment is explicit.
enum class MachinePseudoOpcode { Use, X86Mov128, X86Mov128Unaligned };
using MachineOpcode = std::variant<ir::Opcode, MachinePseudoOpcode>;
std::string_view opcode_name(MachineOpcode);
// A copied stack block retains its source value's analysis identity after
// allocation. This is a liveness dependency, never an address to encode.
struct CallArgumentDependency {
  std::size_t operand;
  const MachineVirtualRegister *value;
  friend bool operator==(const CallArgumentDependency &, const CallArgumentDependency &) = default;
};
class MachineInstruction {
public:
  struct ConstructionKey { private: friend class MachineRewriter; ConstructionKey() = default; };
  MachineInstruction(ConstructionKey, MachineBasicBlock &, MachineOpcode, std::vector<MachineOperand>);
  MachineInstruction(const MachineInstruction &) = delete;
  MachineInstruction &operator=(const MachineInstruction &) = delete;
  const MachineBasicBlock &block() const { return *block_; }
  MachineOpcode opcode() const { return opcode_; }
  std::span<const MachineOperand> operands() const { return operands_; }
  std::span<const target::PhysicalRegister> call_arguments() const { return call_arguments_; }
  std::span<const CallArgumentDependency> call_dependencies() const { return call_dependencies_; }
  bool has_lowered_call() const { return lowered_call_; }
  bool is_entry_setup() const { return entry_setup_; }
  std::uint64_t creation_order() const { return creation_order_; }
private:
  friend class MachineRewriter;
  MachineBasicBlock *block_;
  MachineOpcode opcode_;
  std::vector<MachineOperand> operands_;
  std::vector<target::PhysicalRegister> call_arguments_;
  std::vector<CallArgumentDependency> call_dependencies_;
  bool lowered_call_ = false;
  bool entry_setup_ = false;
  std::uint64_t creation_order_ = 0;
  std::list<MachineInstruction>::iterator position_;
};
class MachineBasicBlock {
public:
  MachineBasicBlock(const MachineBasicBlock &) = delete;
  MachineBasicBlock &operator=(const MachineBasicBlock &) = delete;
  const MachineFunction &function() const { return *function_; }
  std::string_view name() const { return name_; }
  auto instructions() const { return ir::NodeRange(instructions_.cbegin(), instructions_.cend()); }
  auto instructions() { return ir::NodeRange(instructions_.begin(), instructions_.end()); }
  std::span<const MachineLabel *const> labels() const { return labels_; }
private:
  friend class MachineFunction;
  friend class MachineRewriter;
  MachineBasicBlock(MachineFunction &function, std::string name) : function_(&function), name_(std::move(name)) {}
  MachineFunction *function_;
  std::string name_;
  std::list<MachineInstruction> instructions_;
  std::vector<const MachineLabel *> labels_;
  std::list<std::unique_ptr<MachineBasicBlock>>::iterator position_;
};
struct MachineFunctionState {
  bool uses_alloca = false, has_block_arguments = false, is_leaf = true, requires_frame_pointer = false;
  bool machinized = false, operands_legalized = false;
  bool operands_rewritten = false;
  bool edge_spills_inserted = false;
  bool post_allocation_optimized = false;
  bool frame_formed = false;
  struct X86 { std::int64_t initial_sp_from_bp = 8; std::size_t variadic_save_bytes = 0; };
  struct AArch64 { std::size_t aggregate_save_bytes = 0, variadic_save_bytes = 0; };
  std::variant<X86, AArch64> target = X86{};
};
class MachineFunction {
public:
  MachineFunction(const MachineFunction &) = delete;
  MachineFunction &operator=(const MachineFunction &) = delete;
  MachineFunction(std::string module, std::string name, ir::Signature, target::TargetMachine);
  std::string_view module_name() const { return module_; }
  std::string_view name() const { return name_; }
  const ir::Signature &signature() const { return signature_; }
  const target::TargetMachine &target() const { return target_; }
  const MachineFunctionState &state() const { return state_; }
  auto blocks() const { return ir::nodes(blocks_); }
  auto blocks() { return ir::mutable_nodes(blocks_); }
  auto registers() const { return ir::nodes(registers_); }
  auto labels() const { return ir::nodes(labels_); }
  auto symbols() const { return ir::nodes(symbols_); }
  auto stack_slots() const { return ir::nodes(slots_); }
  auto memory_locations() const { return ir::nodes(memory_locations_); }
  std::span<const MachineVirtualRegister *const> arguments() const { return arguments_; }
  std::span<const target::PhysicalRegister> global_registers() const { return global_registers_; }
  std::uint64_t revision() const { return revision_; }
  Result<MachineVirtualRegister *> create_register(ir::Type, std::string name = {}, bool argument = false,
    bool memory_backed = false, std::optional<target::PhysicalRegister> binding = {});
  Result<MachineStackSlot *> create_stack_slot(std::size_t size, std::size_t alignment);
  const MachineMemoryLocation &create_memory_location();
  MachineBasicBlock &create_block(std::string name = {});
  Result<MachineLabel *> create_label(MachineBasicBlock &, std::string name = {}, bool address_taken = false,
    std::optional<std::size_t> source_ordinal = {},bool function_entry = false);
  Result<const MachineSymbol *> intern_symbol(MachineSymbol);
  Result<void> verify() const;
  std::size_t instruction_count() const;
  std::string dump() const;
private:
  friend class MachineRewriter;
  std::string module_, name_;
  ir::Signature signature_;
  target::TargetMachine target_;
  MachineFunctionState state_;
  std::list<std::unique_ptr<MachineBasicBlock>> blocks_;
  std::vector<std::unique_ptr<MachineVirtualRegister>> registers_;
  std::vector<std::unique_ptr<MachineLabel>> labels_;
  std::unordered_set<std::size_t> source_label_ordinals_;
  std::vector<std::unique_ptr<MachineStackSlot>> slots_;
  std::vector<std::unique_ptr<MachineMemoryLocation>> memory_locations_;
  std::vector<std::unique_ptr<MachineSymbol>> symbols_;
  std::vector<const MachineVirtualRegister *> arguments_;
  std::vector<target::PhysicalRegister> global_registers_;
  ir::StringMap<const MachineVirtualRegister *> register_names_;
  std::unordered_map<unsigned, MachineVirtualRegister *> bindings_;
  ir::StringMap<std::vector<const MachineSymbol *>> symbol_names_;
  std::unordered_set<const MachineSymbol *> known_symbols_;
  std::uint64_t revision_ = 0;
  std::uint64_t next_instruction_ = 0;
};
class MachineRewriter {
public:
  explicit MachineRewriter(MachineFunction &function) : function_(function) {}
  Result<MachineInstruction *> append(MachineBasicBlock &, MachineOpcode, std::vector<MachineOperand>);
  Result<MachineInstruction *> insert_before(MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<MachineInstruction *> insert_after(MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<MachineInstruction *> previous(MachineInstruction &) const;
  Result<MachineInstruction *> next(MachineInstruction &) const;
  Result<void> replace(MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<void> erase(MachineInstruction &);
  Result<void> move_before(MachineInstruction &, MachineInstruction &);
  Result<void> move_to(MachineInstruction &, MachineBasicBlock &, bool at_end = true);
  Result<void> move_label(MachineLabel &, MachineBasicBlock &);
  Result<void> erase_label(MachineLabel &);
  Result<MachineBasicBlock *> create_block_before(MachineBasicBlock &);
  Result<MachineBasicBlock *> create_block_after(MachineBasicBlock &);
  Result<void> set_call_arguments(MachineInstruction &, std::span<const target::PhysicalRegister>);
  // Dependencies are attached after allocation; they cannot enter coalescing
  // or be mistaken for executable virtual addresses.
  Result<void> set_call_dependencies(MachineInstruction &, std::span<const CallArgumentDependency>);
  // Atomic replacement of an ABI-lowered call. Its block operands describe
  // physical argument locations, rather than public IR block addresses.
  Result<void> lower_call(MachineInstruction &, MachineOpcode, std::vector<MachineOperand>);
  Result<void> set_state(MachineFunctionState);
  Result<void> set_global_registers(std::span<const target::PhysicalRegister>);
  Result<void> set_entry_setup(MachineInstruction &,bool = true);
private:
  friend class MachineFunction;
  Result<void> validate(MachineOpcode, std::span<const MachineOperand>, bool lowered_call = false) const;
  Result<void> validate_dependencies(MachineOpcode, std::span<const MachineOperand>, std::span<const CallArgumentDependency>) const;
  MachineFunction &function_;
};
struct MachineControlFlow {
  enum class EdgeKind { Branch, Fallthrough, SwitchCase, Indirect, Entry, Exit };
  struct Edge { std::size_t source, destination; EdgeKind kind; std::size_t operand = SIZE_MAX; };
  // Entry and exit are derived boundaries, not owners of a second instruction body.
  std::vector<const MachineBasicBlock *> blocks;
  std::vector<Edge> edges;
  std::vector<std::vector<std::size_t>> incoming, outgoing;
  std::uint64_t revision = 0;
  static Result<MachineControlFlow> build(const MachineFunction &);
  Result<void> verify(const MachineFunction &) const;
  bool current(const MachineFunction &function) const { return revision == function.revision() && owner_ == &function; }
private:
  const MachineFunction *owner_ = nullptr;
  friend struct detail::EdgeSpillEngine;
  friend struct detail::MachineEdgeEditor;
};
// The result owns every name, operand, signature and relocation identity; it
// borrows no public IR node and can outlive the source Context/optimized result.
Result<std::unique_ptr<MachineFunction>> prepare_machine_ir(const ir::Function &, const target::TargetMachine &);
}
