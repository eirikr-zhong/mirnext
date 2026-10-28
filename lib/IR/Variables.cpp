#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
/* Licensed under the MIT License. Typed bindings share the module's canonical symbols and storage.
 */
#include "mirnext/IR/IR.hpp"
#include <algorithm>

namespace mirnext::ir {
namespace {
Error invalid (std::string message) { return {ErrorCode::InvalidOperand, std::move (message)}; }
std::size_t storage_size (Type type) {
  switch (type.kind ()) {
  case Type::Kind::I8:
  case Type::Kind::U8: return 1;
  case Type::Kind::I16:
  case Type::Kind::U16: return 2;
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::F32: return 4;
  case Type::Kind::LongDouble: return sizeof (long double);
  default: return 8;
  }
}
Opcode move_opcode (Type type) {
  return type.kind () == Type::Kind::F32          ? Opcode::FMov
         : type.kind () == Type::Kind::F64        ? Opcode::DMov
         : type.kind () == Type::Kind::LongDouble ? Opcode::LDMov
                                                  : Opcode::Mov;
}
bool compatible_storage (Type binding, Type storage) {
  if (binding == storage) return true;
  return (binding.kind () == Type::Kind::Bool || binding.kind () == Type::Kind::Pointer)
         && (storage.kind () == Type::Kind::I64 || storage.kind () == Type::Kind::U64);
}
}  // namespace
Result<void> validate_variable_binding (const Symbol &symbol, Type type, bool writable) {
  if (!type.valid () || !type.is_scalar ()) return invalid ("variable type must be scalar");
  if (symbol.name ().empty ()) return invalid ("variable requires a named symbol");
  if (symbol.kind () == Symbol::Kind::Import && !symbol.signature ().get ()) return {};
  if (symbol.kind () != Symbol::Kind::Data || !symbol.data ().get ())
    return invalid ("variable requires defined data or a data import");
  if (writable && !symbol.data ().get ()->writable ())
    return invalid ("cannot make a read-only data binding writable");
  const auto &initializer = symbol.data ().get ()->initializer ();
  if (auto data = std::get_if<InitializedData> (&initializer)) {
    if (data->values.size () != 1 || !compatible_storage (type, data->type))
      return invalid ("variable type does not match scalar data storage");
  } else if (auto data = std::get_if<ZeroData> (&initializer)) {
    if (data->size != storage_size (type))
      return invalid ("variable size does not match zero-initialized storage");
  } else if (std::holds_alternative<SymbolReference> (initializer)
             || std::holds_alternative<LabelReference> (initializer)) {
    if (type.kind () != Type::Kind::Pointer && type.kind () != Type::Kind::I64
        && type.kind () != Type::Kind::U64)
      return invalid ("reference variable requires a pointer-sized type");
  } else if (auto expression = std::get_if<ExpressionData> (&initializer)) {
    if (!compatible_storage (type,
                             detail::NodeAccess::function (*(expression))->signature ().results[0]))
      return invalid ("expression variable type mismatch");
  }
  return {};
}
Result<const GlobalVariable &> Module::bind_variable (const Symbol &symbol, Type type,
                                                      bool writable) {
  if (error_) return *error_;
  if (&symbol.module () != this)
    return fail (invalid ("variable symbol belongs to another module"));
  auto checked = validate_variable_binding (symbol, type, writable);
  if (!(checked).has_value ()) return fail (checked.error ());
  if (symbol.variable ().get ()) {
    if (symbol.variable ().get ()->type () != type
        || symbol.variable ().get ()->writable () != writable)
      return fail (invalid ("variable already has a different binding"));
    return *(symbol.variable ().get ());
  }
  const_cast<Symbol &> (symbol).variable_
    = std::unique_ptr<GlobalVariable> (new GlobalVariable (symbol, type, writable));
  ++data_revision_;
  ++revisions_.symbols;
  return *(symbol.variable ().get ());
}
Result<const GlobalVariable &> Module::create_variable (std::string_view name, Type type,
                                                        std::optional<Operand> initial,
                                                        bool writable) {
  if (error_) return *error_;
  if (name.empty () || !type.valid () || !type.is_scalar ())
    return fail (invalid ("variable requires a name and scalar type"));
  if (initial && type.kind () == Type::Kind::Bool) {
    if (auto value = detail::operand_access (*(initial)).get<std::int64_t> ())
      initial = Operand (*value != 0);
    else if (auto value = detail::operand_access (*(initial)).get<std::uint64_t> ())
      initial = Operand (*value != 0);
  }
  DataInitializer initializer = initial ? DataInitializer (InitializedData{type, {*initial}})
                                        : DataInitializer (ZeroData{storage_size (type)});
  // Pointer initializers retain relocations rather than prematurely computing addresses.
  if (initial && type.kind () == Type::Kind::Pointer)
    if (auto reference = detail::operand_access (*(initial)).get<SymbolReference> ())
      initializer = *reference;
  auto data = create_data (name, std::move (initializer), writable);
  if (!(data).has_value ()) return data.error ();
  return bind_variable (data.value ().symbol (), type, writable);
}
Result<const GlobalVariable &> Module::import_variable (std::string_view name, Type type,
                                                        bool writable) {
  if (error_) return *error_;
  if (!type.valid () || !type.is_scalar ())
    return fail (invalid ("imported variable type must be scalar"));
  auto symbol = import (name);
  if (!(symbol).has_value ()) return symbol.error ();
  return bind_variable ((symbol).value (), type, writable);
}
Result<const GlobalVariable &> Module::create_variable (std::string_view name,
                                                        const Function &expression, bool writable) {
  if (error_) return *error_;
  if (name.empty ()) return fail (invalid ("variable requires a named symbol"));
  auto data = create_data (name, detail::NodeAccess::make_ExpressionData (&expression), writable);
  if (!(data).has_value ()) return data.error ();
  return bind_variable (data.value ().symbol (), expression.signature ().results[0], writable);
}
Result<const Register &> InstructionBuilder::address_of (const GlobalVariable &variable,
                                                         std::string_view name) {
  return address_of (variable.symbol (), 0, name);
}
Result<const Register &> InstructionBuilder::load (const GlobalVariable &variable,
                                                   std::string_view name) {
  auto instruction = access_variable (variable, {}, name);
  if (!(instruction).has_value ()) return instruction.error ();
  return *(*detail::operand_access (instruction.value ().operands ()[0]).get<const Register *> ());
}
Result<Instruction &> InstructionBuilder::store (const GlobalVariable &variable, Operand value) {
  return access_variable (variable, std::move (value), {});
}
Result<Instruction &> InstructionBuilder::access_variable (const GlobalVariable &variable,
                                                           std::optional<Operand> stored,
                                                           std::string_view name) {
  if ((function_->module ().error ()).has_value ()) return (function_->module ().error ()).value ();
  if (!block_ || block_->terminator ().get ())
    return fail (invalid ("variable access requires an open insertion block"));
  if (&variable.symbol ().module () != &function_->module ())
    return fail (invalid ("variable belongs to another module; use an import"));
  if (stored && !variable.writable ())
    return fail (invalid ("cannot store through a read-only variable binding"));
  if (name.find ('\0') != name.npos || (!name.empty () && function_->find_register (name).get ()))
    return fail (invalid ("invalid or duplicate variable result name"));
  struct Operation {
    Opcode opcode;
    std::vector<Operand> operands;
  };
  std::vector<Operation> operations;
  operations.reserve (3);
  std::vector<std::unique_ptr<Register>> pending;
  pending.reserve (3);
  auto temporary = [&] (Type type, std::string_view name = {}) {
    auto reg = std::unique_ptr<Register> (new Register (*function_, type, name, {}, false));
    auto result = reg.get ();
    pending.push_back (std::move (reg));
    return result;
  };
  auto address = temporary (Type::pointer ());
  operations.push_back ({Opcode::Mov, {Operand (*address), Operand (variable.symbol ())}});
  auto type = variable.type ();
  const bool boolean = type.kind () == Type::Kind::Bool;
  if (stored) {
    if (boolean) {
      if (auto value = detail::operand_access (*(stored)).get<std::int64_t> ())
        stored = Operand (*value != 0);
      else if (auto value = detail::operand_access (*(stored)).get<std::uint64_t> ())
        stored = Operand (*value != 0);
      else {
        auto normalized = temporary (type);
        operations.push_back ({Opcode::Ne, {Operand (*normalized), *stored, Operand (0)}});
        stored = Operand (*normalized);
      }
    }
    operations.push_back (
      {move_opcode (type), {Operand (detail::NodeAccess::make_Memory (type, address)), *stored}});
  } else {
    auto result = temporary (boolean ? Type::i64 () : type, boolean ? std::string_view{} : name);
    operations.push_back (
      {move_opcode (type),
       {Operand (*result), Operand (detail::NodeAccess::make_Memory (type, address))}});
    if (boolean) {
      auto normalized = temporary (type, name);
      operations.push_back ({Opcode::Ne, {Operand (*normalized), Operand (*result), Operand (0)}});
    }
  }
  for (auto &operation : operations) {
    auto checked = validate_instruction (*function_, operation.opcode, operation.operands);
    if (!(checked).has_value ()) return fail (checked.error ());
  }
  for (auto &reg : pending) {
    if (!reg->name ().empty ()) function_->register_names_.emplace (reg->name (), reg.get ());
    function_->registers_.push_back (std::move (reg));
    function_->registers_changed ();
  }
  Instruction *last = nullptr;
  for (auto &operation : operations)
    last = append_validated (operation.opcode, std::move (operation.operands));
  return *(last);
}
}  // namespace mirnext::ir
