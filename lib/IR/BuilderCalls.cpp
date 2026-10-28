/* Licensed under the MIT License. Calls publish results only after complete validation. */
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/IR/IR.hpp"

namespace mirnext::ir {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
}  // namespace
Result<detail::CallResultState> InstructionBuilder::build_call (
  Opcode opcode, const Symbol &signature, Operand callee,
  std::span<const detail::CallArgumentState> arguments) {
  if ((function_->module ().error ()).has_value ()) return (function_->module ().error ()).value ();
  if (!block_ || block_->terminator ().get ())
    return fail (invalid ("call requires an open insertion block"));
  if (&signature.module () != &function_->module () || !signature.signature ().get ())
    return fail (invalid ("call requires a signature from the caller module"));
  const auto &types = *signature.signature ().get ();
  std::vector<Operand> operands;
  operands.reserve (2 + types.results.size () + arguments.size ());
  operands.emplace_back (signature);
  operands.push_back (std::move (callee));
  std::vector<std::unique_ptr<Register>> pending;
  pending.reserve (types.results.size ());
  std::vector<const Register *> results;
  results.reserve (types.results.size ());
  for (auto type : types.results) {
    auto reg = std::unique_ptr<Register> (new Register (*function_, type, {}, {}, false));
    results.push_back (reg.get ());
    operands.emplace_back (*reg);
    pending.push_back (std::move (reg));
  }
  std::size_t index = 0;
  for (const auto &argument : arguments) {
    if (auto block = std::get_if<detail::BlockArgumentState> (&argument.storage ())) {
      if (!block->memory ().type.is_block ())
        return fail (invalid ("detail::BlockArgumentState requires a block type"));
      operands.emplace_back (block->memory ());
    } else if (auto expression = std::get_if<detail::ExpressionState> (&argument.storage ())) {
      auto expected = index < types.parameters.size ()
                        ? std::optional<Type> (types.parameters[index])
                        : std::nullopt;
      auto value = expression_operand (*expression, expected);
      if (!(value).has_value ()) return value.error ();
      operands.push_back (std::move ((value).value ()));
    } else
      operands.push_back (std::get<Operand> (argument.storage ()));
    ++index;
  }
  auto checked = validate_instruction (*function_, opcode, operands);
  if (!(checked).has_value ()) return fail (checked.error ());
  // The same allocations become the owned registers; operands never need remapping.
  for (auto &reg : pending) {
    function_->registers_.push_back (std::move (reg));
    function_->registers_changed ();
  }
  return detail::CallResultState (*append_validated (opcode, std::move (operands)),
                                  std::move (results));
}
Result<detail::CallResultState> InstructionBuilder::call (
  const Symbol &callee, std::span<const detail::CallArgumentState> arguments) {
  return build_call (Opcode::Call, callee, Operand (callee), arguments);
}
Result<detail::CallResultState> InstructionBuilder::call_indirect (
  const Symbol &signature, Operand callee, std::span<const detail::CallArgumentState> arguments) {
  return build_call (Opcode::Call, signature, std::move (callee), arguments);
}
Result<detail::CallResultState> InstructionBuilder::inline_call (
  const Symbol &callee, std::span<const detail::CallArgumentState> arguments) {
  return build_call (Opcode::Inline, callee, Operand (callee), arguments);
}
Result<Instruction &> InstructionBuilder::jump_call (const Symbol &callee) {
  return jump_call (callee, Operand (callee));
}
Result<Instruction &> InstructionBuilder::jump_call (const Symbol &signature, Operand callee) {
  return emit (Opcode::JCall, {Operand (signature), std::move (callee)});
}
Result<Instruction &> InstructionBuilder::ret_to (Operand continuation) {
  return emit (Opcode::JRet, {std::move (continuation)});
}
Result<Instruction &> InstructionBuilder::start_varargs (Operand list) {
  return emit (Opcode::VaStart, {std::move (list)});
}
Result<Instruction &> InstructionBuilder::end_varargs (Operand list) {
  return emit (Opcode::VaEnd, {std::move (list)});
}
Result<const Register &> InstructionBuilder::next_vararg_address (Type type, Operand list,
                                                                  std::string_view name) {
  if (!type.valid () || !type.is_scalar ()) return fail (invalid ("va_arg requires a scalar type"));
  const Operand inputs[]{std::move (list), Operand (detail::NodeAccess::make_Memory (type))};
  return value_operation (Opcode::VaArg, inputs, Type::pointer (), name);
}
Result<const Register &> InstructionBuilder::next_vararg (Type type, Operand list,
                                                          std::string_view name) {
  if ((function_->module ().error ()).has_value ()) return (function_->module ().error ()).value ();
  if (name.find ('\0') != name.npos || (!name.empty () && function_->find_register (name).get ()))
    return fail (invalid ("invalid or duplicate va_arg result name"));
  auto address = next_vararg_address (type, std::move (list));
  if (!(address).has_value ()) return address.error ();
  return load (detail::NodeAccess::make_Memory (type, &(address).value ()), name);
}
Result<Instruction &> InstructionBuilder::next_vararg_block (const Register &destination,
                                                             Operand list, Type type) {
  if (!type.valid () || type.kind () != Type::Kind::Block || type.block_size () == 0)
    return fail (invalid ("va_block_arg requires a nonempty ordinary block type"));
  if (&destination.function () != function_
      || (!destination.type ().is_integer () && !destination.type ().is_block ()))
    return fail (invalid ("va_block_arg destination must be an address register in this function"));
  return emit (Opcode::VaBlockArg, {Operand (destination), std::move (list),
                                    Operand (static_cast<std::int64_t> (type.block_size ())),
                                    Operand (static_cast<std::int64_t> (type.block_class ()))});
}
}  // namespace mirnext::ir
