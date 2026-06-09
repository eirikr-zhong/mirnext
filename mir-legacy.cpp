/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir-legacy.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace mirnext {

LegacyContext::LegacyContext() : ctx_(MIR_init()) {}

LegacyContext::~LegacyContext() {
  if (ctx_ != nullptr) MIR_finish(ctx_);
}

MIR_context_t LegacyContext::raw() const noexcept { return ctx_; }

static Result<MIR_type_t> lower_type(Type type) {
  switch (type.kind()) {
  case Type::Kind::I64: return MIR_T_I64;
  case Type::Kind::U8: return MIR_T_U8;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext type"};
}

static Result<MIR_insn_code_t> lower_opcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::Mov: return MIR_MOV;
  case Opcode::Add: return MIR_ADD;
  case Opcode::Bge: return MIR_BGE;
  case Opcode::Blt: return MIR_BLT;
  case Opcode::Alloca: return MIR_ALLOCA;
  case Opcode::Jmp: return MIR_JMP;
  case Opcode::Beq: return MIR_BEQ;
  case Opcode::Nop: return MIR_INVALID_INSN;
  case Opcode::Label: return MIR_LABEL;
  case Opcode::Ret: return MIR_RET;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext opcode"};
}

static Result<MIR_op_t> lower_operand(MIR_context_t ctx, const Operand &operand,
                                      const std::unordered_map<std::size_t, MIR_reg_t> &regs,
                                      const std::unordered_map<std::size_t, MIR_label_t> &labels) {
  switch (operand.kind()) {
  case Operand::Kind::Int64:
    return MIR_new_int_op(ctx, operand.int64_value());
  case Operand::Kind::Register: {
    const auto it = regs.find(operand.register_id());
    if (it == regs.end()) return Error{ErrorCode::InvalidOperand, "unknown register operand"};
    return MIR_new_reg_op(ctx, it->second);
  }
  case Operand::Kind::LabelRef: {
    const auto it = labels.find(operand.label_id());
    if (it == labels.end()) return Error{ErrorCode::InvalidOperand, "unknown label operand"};
    return MIR_new_label_op(ctx, it->second);
  }
  case Operand::Kind::Memory: {
    const auto base = regs.find(operand.memory_base_register_id());
    const auto index = regs.find(operand.memory_index_register_id());
    if (base == regs.end() || index == regs.end()) {
      return Error{ErrorCode::InvalidOperand, "unknown memory register operand"};
    }
    Result<MIR_type_t> type = lower_type(operand.memory_type());
    if (!type) return type.error();
    return MIR_new_mem_op(ctx, type.value(), operand.memory_displacement(), base->second,
                          index->second, static_cast<MIR_scale_t>(operand.memory_scale()));
  }
  }
  return Error{ErrorCode::InvalidOperand, "unsupported MIRNext operand"};
}

static Result<std::vector<MIR_op_t>>
lower_operands(MIR_context_t ctx, const std::vector<Operand> &operands,
               const std::unordered_map<std::size_t, MIR_reg_t> &regs,
               const std::unordered_map<std::size_t, MIR_label_t> &labels) {
  std::vector<MIR_op_t> result;
  result.reserve(operands.size());
  for (const Operand &operand : operands) {
    Result<MIR_op_t> lowered = lower_operand(ctx, operand, regs, labels);
    if (!lowered) return lowered.error();
    result.push_back(lowered.value());
  }
  return result;
}

Result<LegacyLoweredFunction> lower_to_legacy(LegacyContext &context, const Module &module,
                                              const Function &function) {
  MIR_context_t ctx = context.raw();
  if (ctx == nullptr) return Error{ErrorCode::InvalidArgument, "missing legacy context"};

  std::vector<MIR_type_t> return_types;
  return_types.reserve(function.return_types().size());
  for (Type type : function.return_types()) {
    Result<MIR_type_t> lowered = lower_type(type);
    if (!lowered) return lowered.error();
    return_types.push_back(lowered.value());
  }

  std::vector<MIR_var_t> arguments;
  arguments.reserve(function.arguments().size());
  for (const Register &argument : function.arguments()) {
    Result<MIR_type_t> type = lower_type(argument.type());
    if (!type) return type.error();
    arguments.push_back(MIR_var_t{type.value(), argument.name().data(), 0});
  }

  MIR_module_t legacy_module = MIR_new_module(ctx, std::string(module.name()).c_str());
  MIR_item_t legacy_function = MIR_new_func_arr(ctx, std::string(function.name()).c_str(),
                                                return_types.size(), return_types.data(),
                                                arguments.size(), arguments.data());

  std::unordered_map<std::size_t, MIR_reg_t> regs;
  regs.reserve(function.arguments().size() + function.local_registers().size());
  for (const Register &argument : function.arguments()) {
    MIR_reg_t reg = MIR_reg(ctx, argument.name().data(), legacy_function->u.func);
    regs.emplace(argument.id(), reg);
  }
  for (const Register &local : function.local_registers()) {
    Result<MIR_type_t> type = lower_type(local.type());
    if (!type) return type.error();
    MIR_reg_t reg = MIR_new_func_reg(ctx, legacy_function->u.func, type.value(), local.name().data());
    regs.emplace(local.id(), reg);
  }

  std::unordered_map<std::size_t, MIR_label_t> labels;
  for (const auto &instruction : function.instructions()) {
    if (instruction->opcode() == Opcode::Label) {
      labels.emplace(instruction->label_id(), MIR_new_label(ctx));
    }
  }

  for (const auto &instruction : function.instructions()) {
    if (instruction->opcode() == Opcode::Label) {
      const auto it = labels.find(instruction->label_id());
      if (it == labels.end()) return Error{ErrorCode::InvalidOperand, "unknown label instruction"};
      MIR_append_insn(ctx, legacy_function, it->second);
      continue;
    }

    Result<std::vector<MIR_op_t>> operands
        = lower_operands(ctx, instruction->operands(), regs, labels);
    if (!operands) return operands.error();

    Result<MIR_insn_code_t> opcode = lower_opcode(instruction->opcode());
    if (!opcode) return opcode.error();
    if (opcode.value() == MIR_INVALID_INSN) {
      return Error{ErrorCode::InvalidArgument, "unsupported MIRNext instruction"};
    }
    MIR_insn_t legacy_instruction
        = MIR_new_insn_arr(ctx, opcode.value(), operands.value().size(), operands.value().data());
    MIR_append_insn(ctx, legacy_function, legacy_instruction);
  }

  MIR_finish_func(ctx);
  MIR_finish_module(ctx);
  return LegacyLoweredFunction{legacy_module, legacy_function};
}

Result<std::int64_t> interpret_i64(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                   const std::int64_t *args, std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy execution input"};
  }
  std::vector<MIR_val_t> values(arg_count);
  for (std::size_t i = 0; i < arg_count; ++i) values[i].i = args[i];
  MIR_load_module(context.raw(), module);
  MIR_link(context.raw(), MIR_set_interp_interface, nullptr);
  MIR_val_t result;
  MIR_interp_arr(context.raw(), function, &result, arg_count, values.data());
  return result.i;
}

Result<std::int64_t> generate_and_call_i64(LegacyContext &context, MIR_module_t module,
                                           MIR_item_t function, const std::int64_t *args,
                                           std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr || arg_count > 1) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy generation input"};
  }
  MIR_load_module(context.raw(), module);
  MIR_gen_init(context.raw());
  MIR_gen_set_optimize_level(context.raw(), 2);
  MIR_link(context.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(context.raw(), function);
  std::int64_t result = 0;
  if (arg_count == 0) {
    result = reinterpret_cast<std::int64_t (*)()>(addr)();
  } else {
    result = reinterpret_cast<std::int64_t (*)(std::int64_t)>(addr)(args[0]);
  }
  MIR_gen_finish(context.raw());
  return result;
}

} // namespace mirnext
