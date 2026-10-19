/* Licensed under the MIT License. Precompiled x86 encoding declarations. */
#pragma once
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include "mirnext/CodeGen/Detail/Encoding.hpp"
#include "mirnext/Target/X86ABI.hpp"
namespace mirnext::codegen {
enum class X86EncodingKind {
  Byte, Rex, Register, RegisterRM, ByteRegisterRM, Memory, TemporaryMemory,
  SwitchMemory, AddressAdd, AddressDisplacement, AddressScale, Immediate8,
  Immediate32, Immediate64, SwitchTable, RIPMemory, Label8, Label32,
  CallPointer, Extension, OpcodeRegister, OpcodePhysical, PoolConstant,
  PhysicalRegister, PhysicalRM, Literal8, Literal32
};
struct X86EncodingOperation { X86EncodingKind kind; std::uint64_t value = 0; };
struct X86EncodingWord { std::size_t first_operation, operation_count, maximum_size; };
struct X86EncodingRecipe {
  std::span<const X86EncodingWord> words;
  std::span<const X86EncodingOperation> operations;
  std::size_t maximum_size;
};
std::optional<X86EncodingRecipe> x86_encoding_recipe(std::size_t pattern_index);
namespace detail {
struct X86AddressEncoding {
  unsigned mod = 0,rm = 0,scale = 0,index = 0,base = 0,displacement_bytes = 0;
  bool sib = false,rex_b = false,rex_x = false;
  std::uint32_t displacement = 0;
};
Result<X86AddressEncoding> encode_x86_address(std::optional<target::X86Register> base,
  std::optional<target::X86Register> index,std::int64_t displacement,unsigned scale = 1);
}
}
