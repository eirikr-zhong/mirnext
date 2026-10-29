/* ABI rules derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. No runtime or machine-instruction state. */
#include "mirnext/Target/X86ABI.hpp"
#include <algorithm>

namespace mirnext::target {
namespace {
using ir::Type;
using K = Type::Kind;
using R = X86Register;
bool supported (ABI abi) { return abi == ABI::X86SysV || abi == ABI::X86Windows; }
Error invalid (std::string reason) {
  return {ErrorCode::InvalidArgument, "x86 ABI: " + std::move (reason)};
}
}  // namespace
std::optional<R> x86_integer_argument_register (ABI abi, std::size_t n) {
  constexpr R sysv[]{R::DI, R::SI, R::DX, R::CX, R::R8, R::R9};
  constexpr R windows[]{R::CX, R::DX, R::R8, R::R9};
  auto regs = abi == ABI::X86Windows ? std::span<const R> (windows) : std::span<const R> (sysv);
  return supported (abi) && n < regs.size () ? std::optional (regs[n]) : std::nullopt;
}
std::optional<R> x86_floating_argument_register (ABI abi, std::size_t n) {
  return supported (abi) && n < (abi == ABI::X86Windows ? 4u : 8u)
           ? std::optional (static_cast<R> (static_cast<unsigned> (R::XMM0) + n))
           : std::nullopt;
}
bool x86_callee_saved_register (ABI abi, R reg) {
  const auto n = static_cast<unsigned> (reg);
  return supported (abi)
         && (reg == R::BX || (n >= 12 && n <= 15)
             || (abi == ABI::X86Windows && (reg == R::SI || reg == R::DI || (n >= 22 && n <= 31))));
}
Result<std::int64_t> x86_spill_displacement (ABI abi, bool frame_pointer, bool variadic, Type type,
                                             std::size_t byte_offset) {
  if (!supported (abi) || !type.valid () || (!type.is_scalar () && !type.is_vector ())
      || byte_offset % 8)
    return invalid ("invalid spill slot");
  const auto size = type.is_vector () || type.kind () == K::LongDouble ? 16u : 8u;
  const auto save = variadic && abi == ABI::X86SysV ? 176u : 0u;
  if (byte_offset > std::size_t (INT64_MAX) - size - save)
    return invalid ("spill displacement overflow");
  return frame_pointer ? -static_cast<std::int64_t> (byte_offset + size + save)
                       : static_cast<std::int64_t> (byte_offset);
}
std::optional<R> x86_reload_register (ABI abi, Type type, bool first) {
  if (!supported (abi) || !type.valid () || (!type.is_scalar () && !type.is_vector ())
      || type.kind () == K::LongDouble)
    return {};
  if (!type.is_float () && !type.is_vector ()) return first ? R::R10 : R::R11;
  return abi == ABI::X86Windows ? (first ? R::XMM4 : R::XMM5) : (first ? R::XMM8 : R::XMM9);
}
Result<X86ArgumentLayout> layout_x86_arguments (ABI abi, std::span<const Type> types) {
  if (!supported (abi)) return invalid ("unsupported target");
  const bool win = abi == ABI::X86Windows;
  X86ArgumentLayout out;
  out.stack_bytes = win ? 32 : 0;
  out.arguments.reserve (types.size ());
  auto assign = [&] (Type type) -> std::optional<R> {
    if (type.kind () == K::LongDouble) return {};
    if (type.is_float () || type.is_vector ()) {
      if (win) ++out.integer_count;
      return x86_floating_argument_register (abi, out.floating_count++);
    }
    if (win) ++out.floating_count;
    return x86_integer_argument_register (abi, out.integer_count++);
  };
  for (auto type : types) {
    if (!type.valid () || (!type.is_scalar () && !type.is_vector () && !type.is_block ()))
      return invalid ("invalid parameter type");
    const bool block = type.kind () == K::Block;
    if (type.is_block ()
        && (!type.block_size () || type.block_size () > std::size_t (INT64_MAX) - 15))
      return invalid ("invalid block size");
    auto classification = static_cast<unsigned> (type.block_class ());
    if (block && classification
        && (type.block_size () > 16 || (classification >= 3 && type.block_size () <= 8)))
      return invalid ("invalid block classification");
    X86ArgumentLocation loc;
    loc.indirect = win && type.is_vector ();
    loc.aggregate = block;
    loc.by_value = block && (!win || type.block_size () <= 8);
    loc.size = block ? (type.block_size () + 7) / 8 * 8
               : type.is_vector () || type.kind () == K::LongDouble ? 16
                                                                    : 8;
    if (type.is_vector () || type.kind () == K::F32 || type.kind () == K::F64)
      out.variadic_xmm_count = std::min (8u, out.variadic_xmm_count + 1);
    if (block && !win && (classification == 1 || classification == 2)) {
      auto scalar = classification == 1 ? Type::i64 () : Type (K::F64);
      auto available = [&] (std::size_t n) {
        return classification == 1 ? x86_integer_argument_register (abi, n)
                                   : x86_floating_argument_register (abi, n);
      };
      auto count = classification == 1 ? out.integer_count : out.floating_count;
      if (available (count) && (loc.size <= 8 || available (count + 1))) {
        loc.registers[loc.register_count++] = *assign (scalar);
        if (loc.size > 8) loc.registers[loc.register_count++] = *assign (scalar);
      }
    } else if (block && !win && (classification == 3 || classification == 4)) {
      if (x86_integer_argument_register (abi, out.integer_count)
          && x86_floating_argument_register (abi, out.floating_count)) {
        loc.registers[loc.register_count++]
          = *assign (classification == 3 ? Type::i64 () : Type (K::F64));
        loc.registers[loc.register_count++]
          = *assign (classification == 3 ? Type (K::F64) : Type::i64 ());
      }
    } else if (!block || win) {
      if (auto reg = assign (block || loc.indirect ? Type::i64 () : type))
        loc.registers[loc.register_count++] = *reg;
    }
    if (!loc.register_count) {
      if (!win && type.is_vector ()) {
        if (out.stack_bytes > std::size_t (INT64_MAX) - 15)
          return invalid ("vector stack alignment overflow");
        out.stack_bytes = (out.stack_bytes + 15) / 16 * 16;
      }
      loc.stack_offset = out.stack_bytes;
      auto size = win ? 8 : loc.size;
      if (size > std::size_t (INT64_MAX) - 63 - out.stack_bytes)
        return invalid ("argument stack area overflow");
      out.stack_bytes += size;
    }
    out.arguments.push_back (loc);
  }
  return out;
}
Result<std::vector<R>> layout_x86_results (ABI abi, std::span<const Type> types) {
  if (!supported (abi)) return invalid ("unsupported target");
  if (abi == ABI::X86Windows && types.size () > 1)
    return invalid ("Windows does not support multiple return values");
  unsigned ints = 0, floats = 0, x87 = 0;
  std::vector<R> out;
  out.reserve (types.size ());
  for (auto t : types) {
    if (!t.valid () || (!t.is_scalar () && !t.is_vector ())) return invalid ("invalid return type");
    if (t.is_vector () || t.kind () == K::F32 || t.kind () == K::F64) {
      if (floats == 2) return invalid ("floating return registers exhausted");
      out.push_back (floats++ ? R::XMM1 : R::XMM0);
    } else if (t.kind () == K::LongDouble) {
      if (x87 == 2) return invalid ("x87 return registers exhausted");
      out.push_back (x87++ ? R::ST1 : R::ST0);
    } else {
      if (ints == 2) return invalid ("integer return registers exhausted");
      out.push_back (ints++ ? R::DX : R::AX);
    }
  }
  return out;
}
}  // namespace mirnext::target
