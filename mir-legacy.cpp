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
  case Type::Kind::I8: return MIR_T_I8;
  case Type::Kind::U8: return MIR_T_U8;
  case Type::Kind::I16: return MIR_T_I16;
  case Type::Kind::U16: return MIR_T_U16;
  case Type::Kind::I32: return MIR_T_I32;
  case Type::Kind::U32: return MIR_T_U32;
  case Type::Kind::I64: return MIR_T_I64;
  case Type::Kind::U64: return MIR_T_U64;
  case Type::Kind::F: return MIR_T_F;
  case Type::Kind::D: return MIR_T_D;
  case Type::Kind::LD: return MIR_T_LD;
  case Type::Kind::P: return MIR_T_P;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext type"};
}

static Result<MIR_insn_code_t> lower_opcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::Mov: return MIR_MOV;
  case Opcode::FMov: return MIR_FMOV;
  case Opcode::DMov: return MIR_DMOV;
  case Opcode::LDMov: return MIR_LDMOV;
  case Opcode::Ext8: return MIR_EXT8;
  case Opcode::Ext16: return MIR_EXT16;
  case Opcode::Ext32: return MIR_EXT32;
  case Opcode::UExt8: return MIR_UEXT8;
  case Opcode::UExt16: return MIR_UEXT16;
  case Opcode::UExt32: return MIR_UEXT32;
  case Opcode::I2F: return MIR_I2F;
  case Opcode::I2D: return MIR_I2D;
  case Opcode::I2LD: return MIR_I2LD;
  case Opcode::UI2F: return MIR_UI2F;
  case Opcode::UI2D: return MIR_UI2D;
  case Opcode::UI2LD: return MIR_UI2LD;
  case Opcode::F2I: return MIR_F2I;
  case Opcode::D2I: return MIR_D2I;
  case Opcode::LD2I: return MIR_LD2I;
  case Opcode::F2D: return MIR_F2D;
  case Opcode::F2LD: return MIR_F2LD;
  case Opcode::D2F: return MIR_D2F;
  case Opcode::D2LD: return MIR_D2LD;
  case Opcode::LD2F: return MIR_LD2F;
  case Opcode::LD2D: return MIR_LD2D;
  case Opcode::Neg: return MIR_NEG;
  case Opcode::Negs: return MIR_NEGS;
  case Opcode::FNeg: return MIR_FNEG;
  case Opcode::DNeg: return MIR_DNEG;
  case Opcode::LDNeg: return MIR_LDNEG;
  case Opcode::Add: return MIR_ADD;
  case Opcode::Adds: return MIR_ADDS;
  case Opcode::FAdd: return MIR_FADD;
  case Opcode::DAdd: return MIR_DADD;
  case Opcode::LDAdd: return MIR_LDADD;
  case Opcode::Sub: return MIR_SUB;
  case Opcode::Subs: return MIR_SUBS;
  case Opcode::FSub: return MIR_FSUB;
  case Opcode::DSub: return MIR_DSUB;
  case Opcode::LDSub: return MIR_LDSUB;
  case Opcode::Mul: return MIR_MUL;
  case Opcode::Muls: return MIR_MULS;
  case Opcode::FMul: return MIR_FMUL;
  case Opcode::DMul: return MIR_DMUL;
  case Opcode::LDMul: return MIR_LDMUL;
  case Opcode::Div: return MIR_DIV;
  case Opcode::Divs: return MIR_DIVS;
  case Opcode::UDiv: return MIR_UDIV;
  case Opcode::UDivs: return MIR_UDIVS;
  case Opcode::FDiv: return MIR_FDIV;
  case Opcode::DDiv: return MIR_DDIV;
  case Opcode::LDDiv: return MIR_LDDIV;
  case Opcode::Mod: return MIR_MOD;
  case Opcode::Mods: return MIR_MODS;
  case Opcode::UMod: return MIR_UMOD;
  case Opcode::UMods: return MIR_UMODS;
  case Opcode::And: return MIR_AND;
  case Opcode::Ands: return MIR_ANDS;
  case Opcode::Or: return MIR_OR;
  case Opcode::Ors: return MIR_ORS;
  case Opcode::Xor: return MIR_XOR;
  case Opcode::Xors: return MIR_XORS;
  case Opcode::Lsh: return MIR_LSH;
  case Opcode::Lshs: return MIR_LSHS;
  case Opcode::Rsh: return MIR_RSH;
  case Opcode::Rshs: return MIR_RSHS;
  case Opcode::URsh: return MIR_URSH;
  case Opcode::URshs: return MIR_URSHS;
  case Opcode::Eq: return MIR_EQ;
  case Opcode::Eqs: return MIR_EQS;
  case Opcode::FEq: return MIR_FEQ;
  case Opcode::DEq: return MIR_DEQ;
  case Opcode::LDEq: return MIR_LDEQ;
  case Opcode::Ne: return MIR_NE;
  case Opcode::Nes: return MIR_NES;
  case Opcode::FNe: return MIR_FNE;
  case Opcode::DNe: return MIR_DNE;
  case Opcode::LDNe: return MIR_LDNE;
  case Opcode::Lt: return MIR_LT;
  case Opcode::Lts: return MIR_LTS;
  case Opcode::ULt: return MIR_ULT;
  case Opcode::ULts: return MIR_ULTS;
  case Opcode::FLt: return MIR_FLT;
  case Opcode::DLt: return MIR_DLT;
  case Opcode::LDLt: return MIR_LDLT;
  case Opcode::Le: return MIR_LE;
  case Opcode::Les: return MIR_LES;
  case Opcode::ULe: return MIR_ULE;
  case Opcode::ULes: return MIR_ULES;
  case Opcode::FLe: return MIR_FLE;
  case Opcode::DLe: return MIR_DLE;
  case Opcode::LDLe: return MIR_LDLE;
  case Opcode::Gt: return MIR_GT;
  case Opcode::Gts: return MIR_GTS;
  case Opcode::UGt: return MIR_UGT;
  case Opcode::UGts: return MIR_UGTS;
  case Opcode::FGt: return MIR_FGT;
  case Opcode::DGt: return MIR_DGT;
  case Opcode::LDGt: return MIR_LDGT;
  case Opcode::Ge: return MIR_GE;
  case Opcode::Ges: return MIR_GES;
  case Opcode::UGe: return MIR_UGE;
  case Opcode::UGes: return MIR_UGES;
  case Opcode::FGe: return MIR_FGE;
  case Opcode::DGe: return MIR_DGE;
  case Opcode::LDGe: return MIR_LDGE;
  case Opcode::Addo: return MIR_ADDO;
  case Opcode::Addos: return MIR_ADDOS;
  case Opcode::Subo: return MIR_SUBO;
  case Opcode::Subos: return MIR_SUBOS;
  case Opcode::Mulo: return MIR_MULO;
  case Opcode::Mulos: return MIR_MULOS;
  case Opcode::UMulo: return MIR_UMULO;
  case Opcode::UMulos: return MIR_UMULOS;
  case Opcode::Alloca: return MIR_ALLOCA;
  case Opcode::Jmp: return MIR_JMP;
  case Opcode::Bt: return MIR_BT;
  case Opcode::Bts: return MIR_BTS;
  case Opcode::Bf: return MIR_BF;
  case Opcode::Bfs: return MIR_BFS;
  case Opcode::Beq: return MIR_BEQ;
  case Opcode::Beqs: return MIR_BEQS;
  case Opcode::FBeq: return MIR_FBEQ;
  case Opcode::DBeq: return MIR_DBEQ;
  case Opcode::LDBeq: return MIR_LDBEQ;
  case Opcode::Bne: return MIR_BNE;
  case Opcode::Bnes: return MIR_BNES;
  case Opcode::FBne: return MIR_FBNE;
  case Opcode::DBne: return MIR_DBNE;
  case Opcode::LDBne: return MIR_LDBNE;
  case Opcode::Blt: return MIR_BLT;
  case Opcode::Blts: return MIR_BLTS;
  case Opcode::UBlt: return MIR_UBLT;
  case Opcode::UBlts: return MIR_UBLTS;
  case Opcode::FBlt: return MIR_FBLT;
  case Opcode::DBlt: return MIR_DBLT;
  case Opcode::LDBlt: return MIR_LDBLT;
  case Opcode::Ble: return MIR_BLE;
  case Opcode::Bles: return MIR_BLES;
  case Opcode::UBle: return MIR_UBLE;
  case Opcode::UBles: return MIR_UBLES;
  case Opcode::FBle: return MIR_FBLE;
  case Opcode::DBle: return MIR_DBLE;
  case Opcode::LDBle: return MIR_LDBLE;
  case Opcode::Bgt: return MIR_BGT;
  case Opcode::Bgts: return MIR_BGTS;
  case Opcode::UBgt: return MIR_UBGT;
  case Opcode::UBgts: return MIR_UBGTS;
  case Opcode::FBgt: return MIR_FBGT;
  case Opcode::DBgt: return MIR_DBGT;
  case Opcode::LDBgt: return MIR_LDBGT;
  case Opcode::Bge: return MIR_BGE;
  case Opcode::Bges: return MIR_BGES;
  case Opcode::UBge: return MIR_UBGE;
  case Opcode::UBges: return MIR_UBGES;
  case Opcode::FBge: return MIR_FBGE;
  case Opcode::DBge: return MIR_DBGE;
  case Opcode::LDBge: return MIR_LDBGE;
  case Opcode::Bo: return MIR_BO;
  case Opcode::UBo: return MIR_UBO;
  case Opcode::Bno: return MIR_BNO;
  case Opcode::UBno: return MIR_UBNO;
  case Opcode::Nop: return MIR_INVALID_INSN;
  case Opcode::Label: return MIR_LABEL;
  case Opcode::Ret: return MIR_RET;
  case Opcode::Call: return MIR_CALL;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext opcode"};
}

static Result<MIR_op_t> lower_operand(MIR_context_t ctx, const Operand &operand,
                                      const std::unordered_map<std::size_t, MIR_reg_t> &regs,
                                      const std::unordered_map<std::size_t, MIR_label_t> &labels,
                                      const std::unordered_map<const void *, MIR_item_t> &refs) {
  switch (operand.kind()) {
  case Operand::Kind::Int64:
    return MIR_new_int_op(ctx, operand.int64_value());
  case Operand::Kind::Float32:
    return MIR_new_float_op(ctx, operand.float32_value());
  case Operand::Kind::Float64:
    return MIR_new_double_op(ctx, operand.float64_value());
  case Operand::Kind::LongDouble:
    return MIR_new_ldouble_op(ctx, operand.long_double_value());
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
  case Operand::Kind::Reference: {
    const auto it = refs.find(operand.reference_pointer());
    if (it == refs.end()) return Error{ErrorCode::InvalidOperand, "unknown reference operand"};
    return MIR_new_ref_op(ctx, it->second);
  }
  }
  return Error{ErrorCode::InvalidOperand, "unsupported MIRNext operand"};
}

static Result<std::vector<MIR_op_t>>
lower_operands(MIR_context_t ctx, const std::vector<Operand> &operands,
               const std::unordered_map<std::size_t, MIR_reg_t> &regs,
               const std::unordered_map<std::size_t, MIR_label_t> &labels,
               const std::unordered_map<const void *, MIR_item_t> &refs) {
  std::vector<MIR_op_t> result;
  result.reserve(operands.size());
  for (const Operand &operand : operands) {
    Result<MIR_op_t> lowered = lower_operand(ctx, operand, regs, labels, refs);
    if (!lowered) return lowered.error();
    result.push_back(lowered.value());
  }
  return result;
}

Result<LegacyLoweredFunction> lower_to_legacy(LegacyContext &context, const Module &module,
                                              const Function &function) {
  MIR_context_t ctx = context.raw();
  if (ctx == nullptr) return Error{ErrorCode::InvalidArgument, "missing legacy context"};

  MIR_module_t legacy_module = MIR_new_module(ctx, std::string(module.name()).c_str());

  std::unordered_map<const void *, MIR_item_t> refs;
  refs.reserve(module.prototypes().size() + module.imports().size() + module.functions().size());

  for (const auto &prototype : module.prototypes()) {
    std::vector<MIR_type_t> return_types;
    return_types.reserve(prototype->return_types().size());
    for (Type type : prototype->return_types()) {
      Result<MIR_type_t> lowered = lower_type(type);
      if (!lowered) return lowered.error();
      return_types.push_back(lowered.value());
    }

    std::vector<MIR_var_t> parameters;
    parameters.reserve(prototype->parameters().size());
    for (const Prototype::Parameter &parameter : prototype->parameters()) {
      Result<MIR_type_t> type = lower_type(parameter.type);
      if (!type) return type.error();
      parameters.push_back(MIR_var_t{type.value(), parameter.name.c_str(), 0});
    }
    MIR_item_t item = MIR_new_proto_arr(ctx, std::string(prototype->name()).c_str(),
                                        return_types.size(), return_types.data(),
                                        parameters.size(), parameters.data());
    refs.emplace(prototype.get(), item);
  }

  for (const auto &import : module.imports()) {
    MIR_item_t item = MIR_new_import(ctx, std::string(import->name()).c_str());
    refs.emplace(import.get(), item);
  }

  for (const auto &current_function : module.functions()) {
    MIR_item_t item = MIR_new_forward(ctx, std::string(current_function->name()).c_str());
    refs.emplace(current_function.get(), item);
  }

  MIR_item_t requested_function = nullptr;
  for (const auto &current_function : module.functions()) {
    std::vector<MIR_type_t> return_types;
    return_types.reserve(current_function->return_types().size());
    for (Type type : current_function->return_types()) {
      Result<MIR_type_t> lowered = lower_type(type);
      if (!lowered) return lowered.error();
      return_types.push_back(lowered.value());
    }

    std::vector<MIR_var_t> arguments;
    arguments.reserve(current_function->arguments().size());
    for (const Register &argument : current_function->arguments()) {
      Result<MIR_type_t> type = lower_type(argument.type());
      if (!type) return type.error();
      arguments.push_back(MIR_var_t{type.value(), argument.name().data(), 0});
    }

    MIR_item_t legacy_function
        = MIR_new_func_arr(ctx, std::string(current_function->name()).c_str(),
                           return_types.size(), return_types.data(), arguments.size(),
                           arguments.data());
    if (current_function.get() == &function) requested_function = legacy_function;

    std::unordered_map<std::size_t, MIR_reg_t> regs;
    regs.reserve(current_function->arguments().size() + current_function->local_registers().size());
    for (const Register &argument : current_function->arguments()) {
      MIR_reg_t reg = MIR_reg(ctx, argument.name().data(), legacy_function->u.func);
      regs.emplace(argument.id(), reg);
    }
    for (const Register &local : current_function->local_registers()) {
      Result<MIR_type_t> type = lower_type(local.type());
      if (!type) return type.error();
      MIR_reg_t reg
          = MIR_new_func_reg(ctx, legacy_function->u.func, type.value(), local.name().data());
      regs.emplace(local.id(), reg);
    }

    std::unordered_map<std::size_t, MIR_label_t> labels;
    for (const auto &instruction : current_function->instructions()) {
      if (instruction->opcode() == Opcode::Label) {
        labels.emplace(instruction->label_id(), MIR_new_label(ctx));
      }
    }

    for (const auto &instruction : current_function->instructions()) {
      if (instruction->opcode() == Opcode::Label) {
        const auto it = labels.find(instruction->label_id());
        if (it == labels.end()) return Error{ErrorCode::InvalidOperand, "unknown label instruction"};
        MIR_append_insn(ctx, legacy_function, it->second);
        continue;
      }

      Result<std::vector<MIR_op_t>> operands
          = lower_operands(ctx, instruction->operands(), regs, labels, refs);
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
  }

  if (requested_function == nullptr) return Error{ErrorCode::UnknownName, "function is not in module"};
  MIR_finish_module(ctx);
  return LegacyLoweredFunction{legacy_module, requested_function};
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

Result<double> interpret_double(LegacyContext &context, MIR_module_t module, MIR_item_t function,
                                const double *args, std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy execution input"};
  }
  std::vector<MIR_val_t> values(arg_count);
  for (std::size_t i = 0; i < arg_count; ++i) values[i].d = args[i];
  MIR_load_module(context.raw(), module);
  MIR_link(context.raw(), MIR_set_interp_interface, nullptr);
  MIR_val_t result;
  MIR_interp_arr(context.raw(), function, &result, arg_count, values.data());
  return result.d;
}

Result<double> generate_and_call_double(LegacyContext &context, MIR_module_t module,
                                        MIR_item_t function, const double *args,
                                        std::size_t arg_count) {
  if (context.raw() == nullptr || module == nullptr || function == nullptr || arg_count > 1) {
    return Error{ErrorCode::InvalidArgument, "invalid legacy generation input"};
  }
  MIR_load_module(context.raw(), module);
  MIR_gen_init(context.raw());
  MIR_gen_set_optimize_level(context.raw(), 2);
  MIR_link(context.raw(), MIR_set_gen_interface, nullptr);
  void *addr = MIR_gen(context.raw(), function);
  double result = 0.0;
  if (arg_count == 0) {
    result = reinterpret_cast<double (*)()>(addr)();
  } else {
    result = reinterpret_cast<double (*)(double)>(addr)(args[0]);
  }
  MIR_gen_finish(context.raw());
  return result;
}

} // namespace mirnext
