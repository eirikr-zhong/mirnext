/* Licensed under the MIT License. Private checked IR storage access. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include "mirnext/IR/SSA.hpp"
namespace mirnext::ir::detail {
struct ProgramStorage {
  Program *owner;
  std::set<std::string, std::less<>> strings;
  std::set<Type> types;
  std::set<Signature> signatures;
  std::vector<std::unique_ptr<Module>> modules;
  std::optional<Error> edit_error;
  std::size_t pending_edits = 0;
};
}  // namespace mirnext::ir::detail
namespace mirnext::ir::detail {
class ProgramAccess {
 public:
  explicit ProgramAccess (Program &value) : value_ (value) {}
  Result<Module &> create_module (std::string_view name) {
    return value_.create_module (std::move (name));
  }

 private:
  Program &value_;
};
inline ProgramAccess access (Program &value) { return ProgramAccess (value); }
class FunctionAccess {
 public:
  explicit FunctionAccess (Function &value) : value_ (value) {}
  void finish_stack_materialization () {
    for (auto &scope : value_.scopes_) scope->materialized_ = true;
    value_.changed (true);
  }
  void track_handles () { value_.tracks_handles_ = true; }
  bool erased (const void *node, std::uint64_t revision) const {
    auto at = value_.erased_nodes_.find (node);
    return at != value_.erased_nodes_.end () && revision <= at->second;
  }
  Result<BasicBlock &> create_block (std::string_view name = {},
                                     const StackScope *scope = nullptr) {
    return value_.create_block (std::move (name), scope);
  }
  Result<BasicBlock &> create_block_after (BasicBlock &block, std::string_view name = {},
                                           const StackScope *scope = nullptr) {
    return value_.create_block_after (block, std::move (name), scope);
  }
  Result<BasicBlock &> create_block_before (BasicBlock &block, std::string_view name = {},
                                            const StackScope *scope = nullptr) {
    return value_.create_block_before (block, std::move (name), scope);
  }
  Result<StackScope &> create_stack_scope (const StackScope *parent = nullptr,
                                           std::string_view name = {}) {
    return value_.create_stack_scope (parent, std::move (name));
  }
  Result<Label &> create_label (BasicBlock &block, std::string_view name = {}) {
    return value_.create_label (block, std::move (name));
  }
  Result<Register &> create_register (Type type, std::string_view name = {},
                                      std::string_view hardware = {}) {
    return value_.create_register (std::move (type), std::move (name), std::move (hardware));
  }

 private:
  Function &value_;
};
inline FunctionAccess access (Function &value) { return FunctionAccess (value); }
class ModuleAccess {
 public:
  explicit ModuleAccess (Module &value) : value_ (value) {}
  Result<Symbol &> reserve_symbol (std::string_view name) {
    return value_.reserve_symbol (std::move (name));
  }
  Result<Symbol &> forward (std::string_view name) { return value_.forward (std::move (name)); }
  Result<Symbol &> declare_data (std::string_view name) {
    return value_.declare_data (std::move (name));
  }
  Result<Symbol &> declare_function (std::string_view name, Signature signature) {
    return value_.declare_function (std::move (name), std::move (signature));
  }
  Result<Symbol &> prototype (std::string_view name, Signature signature) {
    return value_.prototype (std::move (name), std::move (signature));
  }
  Result<Function &> define_function (Symbol &symbol,
                                      std::span<const std::string_view> arguments = {}) {
    return value_.define_function (symbol, std::move (arguments));
  }
  Result<Function &> create_function (std::string_view name, Signature signature,
                                      std::span<const std::string_view> arguments = {}) {
    return value_.create_function (std::move (name), std::move (signature), std::move (arguments));
  }
  Result<Symbol &> import (std::string_view name, std::optional<Signature> signature = {}) {
    return value_.import (std::move (name), std::move (signature));
  }
  Result<GlobalData &> define_data (Symbol &symbol, DataInitializer initial, bool writable = true) {
    return value_.define_data (symbol, std::move (initial), std::move (writable));
  }
  Result<GlobalData &> create_data (std::string_view name, DataInitializer initial,
                                    bool writable = true) {
    return value_.create_data (std::move (name), std::move (initial), std::move (writable));
  }
  Result<GlobalData &> create_literal (Operand value) {
    return value_.create_literal (std::move (value));
  }
  Result<const GlobalVariable &> create_variable (std::string_view name, Type type,
                                                  std::optional<Operand> initial = {},
                                                  bool writable = true) {
    return value_.create_variable (std::move (name), std::move (type), std::move (initial),
                                   std::move (writable));
  }
  Result<const GlobalVariable &> create_variable (std::string_view name, const Function &initial,
                                                  bool writable = true) {
    return value_.create_variable (std::move (name), initial, std::move (writable));
  }
  Result<const GlobalVariable &> import_variable (std::string_view name, Type type,
                                                  bool writable = true) {
    return value_.import_variable (std::move (name), std::move (type), std::move (writable));
  }
  Result<const GlobalVariable &> bind_variable (const Symbol &symbol, Type type,
                                                bool writable = true) {
    return value_.bind_variable (symbol, std::move (type), std::move (writable));
  }
  Result<void> export_symbol (const Symbol &symbol) { return value_.export_symbol (symbol); }
  Result<void> rename_symbol (const Symbol &symbol, std::string_view name) {
    return value_.rename_symbol (symbol, std::move (name));
  }

 private:
  Module &value_;
};
inline ModuleAccess access (Module &value) { return ModuleAccess (value); }
struct ReferenceAccess {
  template <class T>
  static std::span<T *const> pointers (OptionalReferenceRange<T> value) {
    return value.values_;
  }
  static const std::list<SSAUse *> &pointers (SSAUseRange value) { return value.uses_; }
  template <class T>
  static std::span<T *const> pointers (ReferenceRange<T> value) {
    return value.values_;
  }
};
class OperandAccess {
 public:
  explicit OperandAccess (const Operand &value) : value_ (value) {}
  template <class T>
  const T *get () const {
    return std::get_if<T> (&value_.value_);
  }
  const auto &storage () const { return value_.value_; }

 private:
  const Operand &value_;
};
inline OperandAccess operand_access (const Operand &value) { return OperandAccess (value); }

class NodeAccess {
 public:
  static auto items (const Module &module) {
    return module.items () | std::views::transform ([] (const ModuleItem &item) -> const auto & {
             return item.value_;
           });
  }
  static SymbolReference make_SymbolReference (const Symbol *symbol,
                                               std::int64_t displacement = 0) {
    return SymbolReference (SymbolReference::Key{}, symbol, std::move (displacement));
  }
  static const Symbol *symbol (const SymbolReference &value) { return value.symbol_; }
  static const Symbol *&symbol (SymbolReference &value) { return value.symbol_; }
  static LabelReference make_LabelReference (const Label *label, const Label *base = nullptr,
                                             std::int64_t displacement = 0) {
    return LabelReference (LabelReference::Key{}, label, base, std::move (displacement));
  }
  static const Label *label (const LabelReference &value) { return value.label_; }
  static const Label *&label (LabelReference &value) { return value.label_; }
  static const Label *base (const LabelReference &value) { return value.base_; }
  static const Label *&base (LabelReference &value) { return value.base_; }
  static ExpressionData make_ExpressionData (const Function *function) {
    return ExpressionData (ExpressionData::Key{}, function);
  }
  static const Function *function (const ExpressionData &value) { return value.function_; }
  static const Function *&function (ExpressionData &value) { return value.function_; }
  static ForwardDeclaration make_ForwardDeclaration (const Symbol *symbol) {
    return ForwardDeclaration (ForwardDeclaration::Key{}, symbol);
  }
  static const Symbol *symbol (const ForwardDeclaration &value) { return value.symbol_; }
  static const Symbol *&symbol (ForwardDeclaration &value) { return value.symbol_; }
  static ExportDeclaration make_ExportDeclaration (const Symbol *symbol) {
    return ExportDeclaration (ExportDeclaration::Key{}, symbol);
  }
  static const Symbol *symbol (const ExportDeclaration &value) { return value.symbol_; }
  static const Symbol *&symbol (ExportDeclaration &value) { return value.symbol_; }
  static DataDeclaration make_DataDeclaration (const Symbol *symbol) {
    return DataDeclaration (DataDeclaration::Key{}, symbol);
  }
  static const Symbol *symbol (const DataDeclaration &value) { return value.symbol_; }
  static const Symbol *&symbol (DataDeclaration &value) { return value.symbol_; }
  static SSAInstructionUse make_SSAInstructionUse (const Instruction *instruction,
                                                   std::size_t operand, SSAOperandPart part) {
    return SSAInstructionUse (SSAInstructionUse::Key{}, instruction, std::move (operand),
                              std::move (part));
  }
  static const Instruction *instruction (const SSAInstructionUse &value) {
    return value.instruction_;
  }
  static const Instruction *&instruction (SSAInstructionUse &value) { return value.instruction_; }
  static SSAPhiUse make_SSAPhiUse (const SSAPhi *phi, std::size_t input) {
    return SSAPhiUse (SSAPhiUse::Key{}, phi, std::move (input));
  }
  static const SSAPhi *phi (const SSAPhiUse &value) { return value.phi_; }
  static const SSAPhi *&phi (SSAPhiUse &value) { return value.phi_; }
  static SSAHardwareUse make_SSAHardwareUse (const Instruction *ret, const Register *hardware) {
    return SSAHardwareUse (SSAHardwareUse::Key{}, ret, hardware);
  }
  static const Instruction *ret (const SSAHardwareUse &value) { return value.ret_; }
  static const Instruction *&ret (SSAHardwareUse &value) { return value.ret_; }
  static const Register *hardware (const SSAHardwareUse &value) { return value.hardware_; }
  static const Register *&hardware (SSAHardwareUse &value) { return value.hardware_; }
  static Memory make_Memory (Type type, const Register *base = nullptr,
                             const Register *index = nullptr, std::int64_t displacement = 0,
                             unsigned scale = 1, std::string alias = {},
                             std::string nonalias = {}) {
    return Memory (type, ::mirnext::detail::borrow_optional (base),
                   ::mirnext::detail::borrow_optional (index), displacement, scale,
                   std::move (alias), std::move (nonalias));
  }
  static const Register *base (const Memory &value) { return value.base_; }
  static const Register *&base (Memory &value) { return value.base_; }
  static const Register *index (const Memory &value) { return value.index_; }
  static const Register *&index (Memory &value) { return value.index_; }
};

}  // namespace mirnext::ir::detail
