/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <utility>

namespace mirnext {

IRBuilder::IRBuilder(Context &context) noexcept : context_(&context) {}

void IRBuilder::set_insert_point(Function &function) noexcept { insert_point_ = &function; }

Function *IRBuilder::insert_point() const noexcept { return insert_point_; }

Result<Label> IRBuilder::create_label() {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return function.value()->create_label();
}

Result<Instruction *> IRBuilder::bind(Label label) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append_label(label);
}

Result<Instruction *> IRBuilder::create_alloca(Register dst, Operand size) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Alloca, {Operand(dst), size}));
}

Result<Instruction *> IRBuilder::create_mov(Operand dst, Operand src) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(Opcode::Mov, {dst, src}));
}

Result<Instruction *> IRBuilder::create_mov(Register dst, Operand src) {
  return create_mov(Operand(dst), src);
}

Result<Instruction *> IRBuilder::create_add(Register dst, Operand lhs, Operand rhs) {
  return create_binary(Opcode::Add, dst, lhs, rhs);
}

Result<Instruction *> IRBuilder::create_unary(Opcode opcode, Register dst, Operand src) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(opcode, {Operand(dst), src}));
}

Result<Instruction *> IRBuilder::create_binary(Opcode opcode, Register dst, Operand lhs,
                                               Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(opcode, {Operand(dst), lhs, rhs}));
}

Result<Instruction *> IRBuilder::create_branch(Opcode opcode, Label target) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(opcode, {Operand(target)}));
}

Result<Instruction *> IRBuilder::create_branch(Opcode opcode, Label target, Operand value) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(opcode, {Operand(target), value}));
}

Result<Instruction *> IRBuilder::create_branch(Opcode opcode, Label target, Operand lhs,
                                               Operand rhs) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append(Instruction(opcode, {Operand(target), lhs, rhs}));
}

#define MIRNEXT_DEFINE_UNARY_BUILDER(method, opcode)                         \
  Result<Instruction *> IRBuilder::method(Register dst, Operand src) {        \
    return create_unary(Opcode::opcode, dst, src);                            \
  }

#define MIRNEXT_DEFINE_BINARY_BUILDER(method, opcode)                                      \
  Result<Instruction *> IRBuilder::method(Register dst, Operand lhs, Operand rhs) {         \
    return create_binary(Opcode::opcode, dst, lhs, rhs);                                    \
  }

#define MIRNEXT_DEFINE_BRANCH1_BUILDER(method, opcode)                         \
  Result<Instruction *> IRBuilder::method(Label target, Operand value) {        \
    return create_branch(Opcode::opcode, target, value);                        \
  }

#define MIRNEXT_DEFINE_BRANCH2_BUILDER(method, opcode)                                      \
  Result<Instruction *> IRBuilder::method(Label target, Operand lhs, Operand rhs) {          \
    return create_branch(Opcode::opcode, target, lhs, rhs);                                  \
  }

#define MIRNEXT_DEFINE_BRANCH0_BUILDER(method, opcode)                 \
  Result<Instruction *> IRBuilder::method(Label target) {              \
    return create_branch(Opcode::opcode, target);                      \
  }

MIRNEXT_DEFINE_UNARY_BUILDER(create_neg, Neg)
MIRNEXT_DEFINE_UNARY_BUILDER(create_negs, Negs)
MIRNEXT_DEFINE_UNARY_BUILDER(create_fmov, FMov)
MIRNEXT_DEFINE_UNARY_BUILDER(create_dmov, DMov)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ldmov, LDMov)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ext8, Ext8)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ext16, Ext16)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ext32, Ext32)
MIRNEXT_DEFINE_UNARY_BUILDER(create_uext8, UExt8)
MIRNEXT_DEFINE_UNARY_BUILDER(create_uext16, UExt16)
MIRNEXT_DEFINE_UNARY_BUILDER(create_uext32, UExt32)
MIRNEXT_DEFINE_UNARY_BUILDER(create_i2f, I2F)
MIRNEXT_DEFINE_UNARY_BUILDER(create_i2d, I2D)
MIRNEXT_DEFINE_UNARY_BUILDER(create_i2ld, I2LD)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ui2f, UI2F)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ui2d, UI2D)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ui2ld, UI2LD)
MIRNEXT_DEFINE_UNARY_BUILDER(create_f2i, F2I)
MIRNEXT_DEFINE_UNARY_BUILDER(create_d2i, D2I)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ld2i, LD2I)
MIRNEXT_DEFINE_UNARY_BUILDER(create_f2d, F2D)
MIRNEXT_DEFINE_UNARY_BUILDER(create_f2ld, F2LD)
MIRNEXT_DEFINE_UNARY_BUILDER(create_d2f, D2F)
MIRNEXT_DEFINE_UNARY_BUILDER(create_d2ld, D2LD)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ld2f, LD2F)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ld2d, LD2D)
MIRNEXT_DEFINE_UNARY_BUILDER(create_fneg, FNeg)
MIRNEXT_DEFINE_UNARY_BUILDER(create_dneg, DNeg)
MIRNEXT_DEFINE_UNARY_BUILDER(create_ldneg, LDNeg)
MIRNEXT_DEFINE_BINARY_BUILDER(create_adds, Adds)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fadd, FAdd)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dadd, DAdd)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldadd, LDAdd)
MIRNEXT_DEFINE_BINARY_BUILDER(create_sub, Sub)
MIRNEXT_DEFINE_BINARY_BUILDER(create_subs, Subs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fsub, FSub)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dsub, DSub)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldsub, LDSub)
MIRNEXT_DEFINE_BINARY_BUILDER(create_mul, Mul)
MIRNEXT_DEFINE_BINARY_BUILDER(create_muls, Muls)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fmul, FMul)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dmul, DMul)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldmul, LDMul)
MIRNEXT_DEFINE_BINARY_BUILDER(create_div, Div)
MIRNEXT_DEFINE_BINARY_BUILDER(create_divs, Divs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_udiv, UDiv)
MIRNEXT_DEFINE_BINARY_BUILDER(create_udivs, UDivs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fdiv, FDiv)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ddiv, DDiv)
MIRNEXT_DEFINE_BINARY_BUILDER(create_lddiv, LDDiv)
MIRNEXT_DEFINE_BINARY_BUILDER(create_mod, Mod)
MIRNEXT_DEFINE_BINARY_BUILDER(create_mods, Mods)
MIRNEXT_DEFINE_BINARY_BUILDER(create_umod, UMod)
MIRNEXT_DEFINE_BINARY_BUILDER(create_umods, UMods)
MIRNEXT_DEFINE_BINARY_BUILDER(create_and, And)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ands, Ands)
MIRNEXT_DEFINE_BINARY_BUILDER(create_or, Or)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ors, Ors)
MIRNEXT_DEFINE_BINARY_BUILDER(create_xor, Xor)
MIRNEXT_DEFINE_BINARY_BUILDER(create_xors, Xors)
MIRNEXT_DEFINE_BINARY_BUILDER(create_lsh, Lsh)
MIRNEXT_DEFINE_BINARY_BUILDER(create_lshs, Lshs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_rsh, Rsh)
MIRNEXT_DEFINE_BINARY_BUILDER(create_rshs, Rshs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ursh, URsh)
MIRNEXT_DEFINE_BINARY_BUILDER(create_urshs, URshs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_eq, Eq)
MIRNEXT_DEFINE_BINARY_BUILDER(create_eqs, Eqs)
MIRNEXT_DEFINE_BINARY_BUILDER(create_feq, FEq)
MIRNEXT_DEFINE_BINARY_BUILDER(create_deq, DEq)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldeq, LDEq)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ne, Ne)
MIRNEXT_DEFINE_BINARY_BUILDER(create_nes, Nes)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fne, FNe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dne, DNe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldne, LDNe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_lt, Lt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_lts, Lts)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ult, ULt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ults, ULts)
MIRNEXT_DEFINE_BINARY_BUILDER(create_flt, FLt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dlt, DLt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldlt, LDLt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_le, Le)
MIRNEXT_DEFINE_BINARY_BUILDER(create_les, Les)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ule, ULe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ules, ULes)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fle, FLe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dle, DLe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldle, LDLe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_gt, Gt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_gts, Gts)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ugt, UGt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ugts, UGts)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fgt, FGt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dgt, DGt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldgt, LDGt)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ge, Ge)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ges, Ges)
MIRNEXT_DEFINE_BINARY_BUILDER(create_uge, UGe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_uges, UGes)
MIRNEXT_DEFINE_BINARY_BUILDER(create_fge, FGe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_dge, DGe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_ldge, LDGe)
MIRNEXT_DEFINE_BINARY_BUILDER(create_addo, Addo)
MIRNEXT_DEFINE_BINARY_BUILDER(create_addos, Addos)
MIRNEXT_DEFINE_BINARY_BUILDER(create_subo, Subo)
MIRNEXT_DEFINE_BINARY_BUILDER(create_subos, Subos)
MIRNEXT_DEFINE_BINARY_BUILDER(create_mulo, Mulo)
MIRNEXT_DEFINE_BINARY_BUILDER(create_mulos, Mulos)
MIRNEXT_DEFINE_BINARY_BUILDER(create_umulo, UMulo)
MIRNEXT_DEFINE_BINARY_BUILDER(create_umulos, UMulos)

Result<Instruction *> IRBuilder::create_jmp(Label target) {
  return create_branch(Opcode::Jmp, target);
}

MIRNEXT_DEFINE_BRANCH1_BUILDER(create_bt, Bt)
MIRNEXT_DEFINE_BRANCH1_BUILDER(create_bts, Bts)
MIRNEXT_DEFINE_BRANCH1_BUILDER(create_bf, Bf)
MIRNEXT_DEFINE_BRANCH1_BUILDER(create_bfs, Bfs)

Result<Instruction *> IRBuilder::create_beq(Label target, Operand lhs, Operand rhs) {
  return create_branch(Opcode::Beq, target, lhs, rhs);
}

Result<Instruction *> IRBuilder::create_bge(Label target, Operand lhs, Operand rhs) {
  return create_branch(Opcode::Bge, target, lhs, rhs);
}

Result<Instruction *> IRBuilder::create_blt(Label target, Operand lhs, Operand rhs) {
  return create_branch(Opcode::Blt, target, lhs, rhs);
}

MIRNEXT_DEFINE_BRANCH2_BUILDER(create_beqs, Beqs)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fbeq, FBeq)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dbeq, DBeq)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldbeq, LDBeq)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bne, Bne)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bnes, Bnes)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fbne, FBne)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dbne, DBne)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldbne, LDBne)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bges, Bges)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ubge, UBge)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ubges, UBges)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fbge, FBge)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dbge, DBge)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldbge, LDBge)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_blts, Blts)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ublt, UBlt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ublts, UBlts)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fblt, FBlt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dblt, DBlt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldblt, LDBlt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ble, Ble)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bles, Bles)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_uble, UBle)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ubles, UBles)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fble, FBle)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dble, DBle)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldble, LDBle)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bgt, Bgt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_bgts, Bgts)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ubgt, UBgt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ubgts, UBgts)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_fbgt, FBgt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_dbgt, DBgt)
MIRNEXT_DEFINE_BRANCH2_BUILDER(create_ldbgt, LDBgt)
MIRNEXT_DEFINE_BRANCH0_BUILDER(create_bo, Bo)
MIRNEXT_DEFINE_BRANCH0_BUILDER(create_ubo, UBo)
MIRNEXT_DEFINE_BRANCH0_BUILDER(create_bno, Bno)
MIRNEXT_DEFINE_BRANCH0_BUILDER(create_ubno, UBno)

#undef MIRNEXT_DEFINE_UNARY_BUILDER
#undef MIRNEXT_DEFINE_BINARY_BUILDER
#undef MIRNEXT_DEFINE_BRANCH0_BUILDER
#undef MIRNEXT_DEFINE_BRANCH1_BUILDER
#undef MIRNEXT_DEFINE_BRANCH2_BUILDER

Result<Instruction *> IRBuilder::create_ret(std::vector<Operand> values) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();
  return &function.value()->append_ret(std::move(values));
}

Result<Instruction *> IRBuilder::create_call(const Prototype &prototype, Operand callee,
                                             std::vector<Operand> results,
                                             std::vector<Operand> arguments) {
  Result<Function *> function = require_insert_point();
  if (!function) return function.error();

  std::vector<Operand> operands;
  operands.reserve(2 + results.size() + arguments.size());
  operands.push_back(Operand::ref(prototype));
  operands.push_back(callee);
  operands.insert(operands.end(), results.begin(), results.end());
  operands.insert(operands.end(), arguments.begin(), arguments.end());
  return &function.value()->append(Instruction(Opcode::Call, std::move(operands)));
}

Result<Function *> IRBuilder::require_insert_point() const {
  if (insert_point_ == nullptr) {
    return Error{ErrorCode::NoInsertPoint, "builder has no insert point"};
  }
  return insert_point_;
}


} // namespace mirnext
