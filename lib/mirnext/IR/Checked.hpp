/* Licensed under the MIT License. Private checked mutation primitives. */
#pragma once
#include "mirnext/IR/IR.hpp"
#include "mirnext/IR/ConstructionState.hpp"
namespace mirnext::ir {
Result<MaterializationStats> materialize_stack_scopes (Function &);

Result<void> validate_instruction (const Function &, Opcode, std::span<const Operand>);
Result<void> validate_initializer (const Module &, const DataInitializer &);
Result<void> validate_variable_binding (const Symbol &, Type, bool writable);
Result<MaterializationStats> materialize_stack_scopes (Program &);

class InstructionBuilder {
 public:
  explicit InstructionBuilder (Function &function) : function_ (&function) {}
  explicit InstructionBuilder (BasicBlock &block)
    : function_ (block.function_), block_ (&block), scope_ (block.scope_) {}
  Result<void> set_insert_point (BasicBlock &);
  void clear_insert_point () { block_ = nullptr; }
  const BasicBlock *insert_block () const { return block_; }
  const StackScope *stack_scope () const { return scope_; }
  Result<BasicBlock &> create_block (std::string_view name = {});
  Result<StackScope &> begin_stack_scope (std::string_view name = {});
  Result<void> end_stack_scope (const StackScope &);
  Result<Instruction &> emit (Opcode, std::span<const Operand>);
  Result<Instruction &> emit (Opcode opcode, std::initializer_list<Operand> operands) {
    return emit (opcode, std::span<const Operand> (operands.begin (), operands.size ()));
  }
  Result<const Register &> binary (Opcode, Operand lhs, Operand rhs, std::string_view name = {});
  ::mirnext::ir::detail::ExpressionState expression (const Register &);
  ::mirnext::ir::detail::ExpressionState expression (Memory);
  ::mirnext::ir::detail::ExpressionState expression (const GlobalVariable &);
  ::mirnext::ir::detail::ExpressionState literal (Operand);
  template <class Boolean>
    requires (
      std::is_same_v<std::remove_cvref_t<Boolean>, bool>)::mirnext::ir::detail::ExpressionState
  literal (Boolean value) {
    return literal (Type (Type::Kind::Bool), Operand (value));
  }
  ::mirnext::ir::detail::ExpressionState literal (Type, Operand);
  Result<Instruction &> assign (const Register &, Operand);
  Result<Instruction &> assign (const Register &, const ::mirnext::ir::detail::ExpressionState &);
  Result<const Register &> unary (Opcode, Operand, std::string_view name = {});
  Result<const Register &> allocate_stack (Operand size, std::string_view name = {});
  Result<const Register &> stack_save (std::string_view name = {});
  Result<Instruction &> stack_restore (const Register &);
  Result<const Register &> address_of (const Register &, std::string_view name = {});
  Result<const Register &> address_of (const Symbol &, std::int64_t displacement = 0,
                                       std::string_view name = {});
  Result<const Register &> address_of (const Label &, std::string_view name = {});
  Result<const Register &> address_of (const GlobalVariable &, std::string_view name = {});
  Result<const Register &> load (Memory, std::string_view name = {});
  Result<const Register &> load (const GlobalVariable &, std::string_view name = {});
  Result<Instruction &> store (Memory, Operand);
  Result<Instruction &> store (Memory, const ::mirnext::ir::detail::ExpressionState &);
  Result<Instruction &> store (const GlobalVariable &, Operand);
  Result<Instruction &> store (const GlobalVariable &,
                               const ::mirnext::ir::detail::ExpressionState &);
  Result<Instruction &> branch_if (Operand, const Label &, bool when_true = true);
  Result<Instruction &> branch_if (const ::mirnext::ir::detail::ExpressionState &, const Label &,
                                   bool when_true = true);
  Result<Instruction &> branch (Comparison, const ::mirnext::ir::detail::ExpressionState &,
                                const ::mirnext::ir::detail::ExpressionState &, const Label &);
  Result<Instruction &> switch_to (Operand, std::span<const Label *const>);
  Result<::mirnext::ir::detail::CallResultState> call (
    const Symbol &callee, std::span<const ::mirnext::ir::detail::CallArgumentState> arguments = {});
  Result<::mirnext::ir::detail::CallResultState> call (
    const Function &callee,
    std::span<const ::mirnext::ir::detail::CallArgumentState> arguments = {}) {
    return call (callee.symbol (), arguments);
  }
  Result<::mirnext::ir::detail::CallResultState> call (
    const Function &callee,
    std::initializer_list<::mirnext::ir::detail::CallArgumentState> arguments) {
    return call (callee.symbol (),
                 std::span<const ::mirnext::ir::detail::CallArgumentState> (arguments.begin (),
                                                                            arguments.size ()));
  }
  Result<::mirnext::ir::detail::CallResultState> call_indirect (
    const Symbol &signature, Operand callee,
    std::span<const ::mirnext::ir::detail::CallArgumentState> arguments = {});
  Result<::mirnext::ir::detail::CallResultState> call (
    const Symbol &callee,
    std::initializer_list<::mirnext::ir::detail::CallArgumentState> arguments) {
    return call (callee,
                 std::span<const ::mirnext::ir::detail::CallArgumentState> (arguments.begin (),
                                                                            arguments.size ()));
  }
  Result<::mirnext::ir::detail::CallResultState> call_indirect (
    const Symbol &signature, Operand callee,
    std::initializer_list<::mirnext::ir::detail::CallArgumentState> arguments) {
    return call_indirect (signature, std::move (callee),
                          std::span<
                            const ::mirnext::ir::detail::CallArgumentState> (arguments.begin (),
                                                                             arguments.size ()));
  }
  Result<::mirnext::ir::detail::CallResultState> inline_call (
    const Symbol &callee, std::span<const ::mirnext::ir::detail::CallArgumentState> arguments = {});
  Result<::mirnext::ir::detail::CallResultState> inline_call (
    const Function &callee,
    std::span<const ::mirnext::ir::detail::CallArgumentState> arguments = {}) {
    return inline_call (callee.symbol (), arguments);
  }
  Result<::mirnext::ir::detail::CallResultState> inline_call (
    const Function &callee,
    std::initializer_list<::mirnext::ir::detail::CallArgumentState> arguments) {
    return inline_call (callee.symbol (),
                        std::span<
                          const ::mirnext::ir::detail::CallArgumentState> (arguments.begin (),
                                                                           arguments.size ()));
  }
  Result<::mirnext::ir::detail::CallResultState> inline_call (
    const Symbol &callee,
    std::initializer_list<::mirnext::ir::detail::CallArgumentState> arguments) {
    return inline_call (callee,
                        std::span<
                          const ::mirnext::ir::detail::CallArgumentState> (arguments.begin (),
                                                                           arguments.size ()));
  }
  Result<Instruction &> jump_call (const Symbol &callee);
  Result<Instruction &> jump_call (const Function &callee) { return jump_call (callee.symbol ()); }
  Result<Instruction &> jump_call (const Symbol &signature, Operand callee);
  Result<Instruction &> ret_to (Operand continuation);
  Result<Instruction &> start_varargs (Operand list);
  Result<Instruction &> end_varargs (Operand list);
  Result<const Register &> next_vararg_address (Type, Operand list, std::string_view name = {});
  Result<const Register &> next_vararg (Type, Operand list, std::string_view name = {});
  Result<Instruction &> next_vararg_block (const Register &destination, Operand list,
                                           Type block_type);
  Result<Instruction &> jump (const Label &);
  Result<Instruction &> ret (std::span<const Operand> = {});
  Result<Instruction &> ret (Operand operand) { return emit (Opcode::Ret, {std::move (operand)}); }
  Result<Instruction &> ret (const ::mirnext::ir::detail::ExpressionState &);

 private:
  friend class ::mirnext::ir::detail::ExpressionState;
  friend class MIRBuilder;
  Error fail (Error);
  Function *function_;
  BasicBlock *block_ = nullptr;
  const StackScope *scope_ = nullptr;
  Result<const Register &> value_operation (Opcode, std::span<const Operand>, Type,
                                            std::string_view);
  Result<::mirnext::ir::detail::CallResultState> build_call (
    Opcode, const Symbol &, Operand, std::span<const ::mirnext::ir::detail::CallArgumentState>);
  Instruction *append_validated (Opcode, std::vector<Operand>);
  Result<Instruction &> access_variable (const GlobalVariable &, std::optional<Operand> stored,
                                         std::string_view);
  Result<Operand> expression_operand (const ::mirnext::ir::detail::ExpressionState &,
                                      std::optional<Type> expected = {});
  ::mirnext::ir::detail::ExpressionState convert_expression (
    const ::mirnext::ir::detail::ExpressionState &, Type);
};

class InstructionRewriter {
 public:
  explicit InstructionRewriter (Function &function) : function_ (function) {}
  const MemoryLocation &create_memory_location ();
  // Annotation edits do not change operands, SSA uses or executable semantics.
  Result<void> set_memory_location (Instruction &, std::size_t, const MemoryLocation *);
  Result<void> replace_operand (Instruction &, std::size_t, Operand);
  Result<void> replace (Instruction &, Opcode, std::span<const Operand>);
  Result<void> erase (Instruction &);
  Result<void> move_before (Instruction &, BasicBlock &, const Instruction *before = nullptr);
  Result<void> move_block_before (BasicBlock &, BasicBlock *before = nullptr);
  Result<void> replace_uses (const Register &, const Register &);
  // A null anchor appends. Terminators can only be appended to an open block.
  Result<Instruction &> insert (BasicBlock &, const Instruction *before, Opcode,
                                std::span<const Operand>);
  Result<Instruction &> insert (BasicBlock &, const Instruction *before, Opcode,
                                std::vector<Operand> &&);
  Result<Instruction &> insert (BasicBlock &block, const Instruction *before, Opcode opcode,
                                std::initializer_list<Operand> operands) {
    return insert (block, before, opcode,
                   std::span<const Operand> (operands.begin (), operands.size ()));
  }
  Result<Instruction &> insert_before (Instruction &, Opcode, std::span<const Operand>);
  Result<Instruction &> insert_before (Instruction &i, Opcode opcode,
                                       std::initializer_list<Operand> operands) {
    return insert_before (i, opcode,
                          std::span<const Operand> (operands.begin (), operands.size ()));
  }
  Result<Instruction &> insert_after (Instruction &, Opcode, std::span<const Operand>);
  Result<Instruction &> insert_after (Instruction &i, Opcode opcode,
                                      std::initializer_list<Operand> operands) {
    return insert_after (i, opcode, std::span<const Operand> (operands.begin (), operands.size ()));
  }
  // Split transfers the suffix without moving labels or copying instructions.
  Result<BasicBlock &> split_before (Instruction &, std::string_view name = {});
  // Merge consumes the following unlabeled block on the same stack scope.
  // Only the consumed block and erased labels invalidate their own references.
  Result<void> merge_fallthrough (BasicBlock &);
  // Separate co-located anchors without copying instructions or labels.
  Result<std::size_t> separate_label_anchors (BasicBlock &);
  // Validate the whole removal set before erasing mutually referencing blocks.
  Result<void> erase_blocks (std::span<BasicBlock *const>);
  Result<std::size_t> erase_unused_labels ();
  Result<Memory> literal_memory (const GlobalData &, const Register &) const;
  BasicBlock *next_block (const BasicBlock &) const;
  // Stays inside the block. A null result denotes the block's layout boundary.
  Instruction *next_instruction (const Instruction &) const;
  Result<void> set_stack_scope (BasicBlock &, const StackScope *);
  // Address lowering records storage identity without changing its binding.
  Result<void> materialize_register_storage (std::span<const Register *const>);
  Result<void> materialize_register_storage (const Register &reg) {
    const Register *registers[]{&reg};
    return materialize_register_storage (registers);
  }

 private:
  friend class SSAUpdater;
  InstructionRewriter (Function &function, bool ssa_edit)
    : function_ (function), ssa_edit_ (ssa_edit) {}
  Result<void> writable () const;
  Result<BasicBlock &> insert_empty_block_before (BasicBlock &);
  Result<void> replace_owned (Instruction &, Opcode, std::vector<Operand>);
  Function &function_;
  bool ssa_edit_ = false;
};

}  // namespace mirnext::ir
