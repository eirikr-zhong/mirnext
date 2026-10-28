/* Licensed under the MIT License. Native target facts shared by machine passes. */
#pragma once
#include "mirnext/Target/Addressing.hpp"
#include <optional>

namespace mirnext::target {
enum class ABI { X86SysV, X86Windows, AArch64Linux, AArch64MacOS };
enum class RegisterBank { Integer, Floating, X87 };
class PhysicalRegister {
 public:
  Architecture architecture () const { return architecture_; }
  unsigned encoding () const { return encoding_; }
  friend bool operator== (PhysicalRegister, PhysicalRegister) = default;

 private:
  friend class TargetMachine;
  PhysicalRegister (Architecture architecture, unsigned encoding)
    : architecture_ (architecture), encoding_ (encoding) {}
  Architecture architecture_;
  unsigned encoding_;
};
struct PhysicalRegisterInfo {
  PhysicalRegister reg;
  std::string_view name;
  RegisterBank bank;
  bool reserved;
};
struct TypeLayout {
  std::size_t size, alignment;
  friend bool operator== (TypeLayout, TypeLayout) = default;
};
class TargetMachine {
 public:
  // Only the current host is a supported compilation target. Facts for another
  // host must not accidentally enable cross-target lowering or execution.
  static Result<TargetMachine> native (Architecture = Architecture::Native);
  Architecture architecture () const { return architecture_; }
  ABI abi () const { return abi_; }
  std::string_view name () const;
  unsigned pointer_bits () const { return 64; }
  std::size_t stack_alignment () const { return 16; }
  std::span<const PhysicalRegisterInfo> registers () const { return registers_; }
  Result<PhysicalRegister> find_register (std::string_view) const;
  OptionalRef<const PhysicalRegisterInfo> register_info (PhysicalRegister) const;
  bool accepts (PhysicalRegister, ir::Type) const;
  bool call_clobbers (PhysicalRegister, ir::Type) const;
  Result<TypeLayout> layout (ir::Type) const;
  ir::Type scalar_type (ir::Type) const;
  ir::Opcode opcode (ir::Opcode) const;

 private:
  TargetMachine (Architecture architecture, ABI abi) : architecture_ (architecture), abi_ (abi) {}
  Architecture architecture_;
  ABI abi_;
  std::vector<PhysicalRegisterInfo> registers_;
};
}  // namespace mirnext::target
