/* Target register rules derived from MIR. Licensed under the MIT License. */
#include "mirnext/Target/TargetMachine.hpp"
#include "mirnext/Target/X86ABI.hpp"
#include <algorithm>

namespace mirnext::target {
Result<TargetMachine> TargetMachine::native(Architecture requested) {
  auto rules = AddressingRules::get(requested); if (!rules) return rules.error();
#if defined(__aarch64__) || defined(_M_ARM64)
  constexpr auto architecture = Architecture::AArch64;
#ifdef __APPLE__
  constexpr auto abi = ABI::AArch64MacOS;
#else
  constexpr auto abi = ABI::AArch64Linux;
#endif
#elif defined(__x86_64__) || defined(_M_X64)
  constexpr auto architecture = Architecture::X86_64;
#ifdef _WIN32
  constexpr auto abi = ABI::X86Windows;
#else
  constexpr auto abi = ABI::X86SysV;
#endif
#else
  return Error{ErrorCode::InvalidArgument, "no native machine target"};
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__x86_64__) || defined(_M_X64)
  if (rules->architecture() != architecture) return Error{ErrorCode::InvalidArgument, "cross-target machine compilation is unsupported"};
  TargetMachine machine(architecture, abi);
  static constexpr std::string_view arm[] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23", "r24", "r25", "r26", "r27", "r28", "r29", "r30", "sp",
    "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
    "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"};
  static constexpr std::string_view x86[] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "st0", "st1"};
  auto names = architecture == Architecture::AArch64 ? std::span<const std::string_view>(arm) : std::span<const std::string_view>(x86);
  for (unsigned n = 0; n < names.size(); ++n) {
    auto bank = architecture == Architecture::AArch64 ? (n < 32 ? RegisterBank::Integer : RegisterBank::Floating)
      : n < 16 ? RegisterBank::Integer : n < 32 ? RegisterBank::Floating : RegisterBank::X87;
    bool reserved = architecture == Architecture::AArch64
      ? n == 29 || n == 31 || n == 9 || n == 10 || n == 48 || n == 49 || (abi == ABI::AArch64MacOS && n == 18)
      : n == 4 || n == 5 || n == 10 || n == 11 || n >= 32 || n == (abi == ABI::X86Windows ? 20u : 24u) || n == (abi == ABI::X86Windows ? 21u : 25u);
    machine.registers_.push_back({PhysicalRegister(architecture, n), names[n], bank, reserved});
  }
  return machine;
#endif
}
std::string_view TargetMachine::name() const {
  switch (abi_) {
  case ABI::X86SysV: return "x86_64-sysv"; case ABI::X86Windows: return "x86_64-windows";
  case ABI::AArch64Linux: return "aarch64-linux"; case ABI::AArch64MacOS: return "aarch64-macos";
  } return "invalid-target";
}
Result<PhysicalRegister> TargetMachine::find_register(std::string_view name) const {
  for (auto &info : registers_) if (info.name == name) return info.reg;
  return Error{ErrorCode::InvalidArgument, "unknown hardware register for " + std::string(this->name()) + ": " + std::string(name)};
}
const PhysicalRegisterInfo *TargetMachine::register_info(PhysicalRegister reg) const {
  return reg.architecture() == architecture_ && reg.encoding() < registers_.size() ? &registers_[reg.encoding()] : nullptr;
}
bool TargetMachine::accepts(PhysicalRegister reg, ir::Type type) const {
  auto info = register_info(reg); if (!info || !type.valid() || (!type.is_scalar() && !type.is_block())) return false;
  type = scalar_type(type);
  if (architecture_ == Architecture::X86_64 && type.kind() == ir::Type::Kind::LongDouble) return false;
  return type.is_float() ? info->bank != RegisterBank::Integer : info->bank == RegisterBank::Integer;
}
bool TargetMachine::call_clobbers(PhysicalRegister reg, ir::Type type) const {
  if (!register_info(reg)) return false;
  const auto n = reg.encoding();
  if (architecture_ == Architecture::AArch64)
    return n < 32 ? !(19 <= n && n <= 28) : type.kind() == ir::Type::Kind::LongDouble || !(40 <= n && n <= 47);
  return !x86_callee_saved_register(abi_,static_cast<X86Register>(n));
}
ir::Type TargetMachine::scalar_type(ir::Type type) const {
  if (type.is_integer() || type.is_block()) return ir::Type::i64();
  if (type.kind() == ir::Type::Kind::LongDouble && sizeof(long double) == sizeof(double)) return ir::Type(ir::Type::Kind::F64);
  return type;
}
Result<TypeLayout> TargetMachine::layout(ir::Type type) const {
  if (!type.valid() || !type.is_scalar()) return Error{ErrorCode::InvalidArgument, "layout requires a scalar type"};
  using K = ir::Type::Kind;
  switch (type.kind()) {
  case K::I8: case K::U8: return TypeLayout{1, 1};
  case K::I16: case K::U16: return TypeLayout{2, 2};
  case K::I32: case K::U32: case K::F32: return TypeLayout{4, 4};
  case K::LongDouble: return TypeLayout{sizeof(long double), alignof(long double)};
  default: return TypeLayout{8, 8};
  }
}
ir::Opcode TargetMachine::opcode(ir::Opcode code) const {
  if (sizeof(long double) != sizeof(double)) return code;
  using O = ir::Opcode;
  switch (code) {
  case O::LDMov: return O::DMov; case O::I2LD: return O::I2D; case O::UI2LD: return O::UI2D;
  case O::LD2I: return O::D2I; case O::F2LD: return O::F2D; case O::D2LD: case O::LD2D: return O::DMov;
  case O::LD2F: return O::D2F; case O::LDNeg: return O::DNeg; case O::LDAdd: return O::DAdd;
  case O::LDSub: return O::DSub; case O::LDMul: return O::DMul; case O::LDDiv: return O::DDiv;
  case O::LDEq: return O::DEq; case O::LDNe: return O::DNe; case O::LDLt: return O::DLt;
  case O::LDLe: return O::DLe; case O::LDGt: return O::DGt; case O::LDGe: return O::DGe;
  case O::LDBeq: return O::DBeq; case O::LDBne: return O::DBne; case O::LDBlt: return O::DBlt;
  case O::LDBle: return O::DBle; case O::LDBgt: return O::DBgt; case O::LDBge: return O::DBge;
  default: return code;
  }
}
}
