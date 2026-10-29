#include "mirnext/IR/Vector.hpp"
#include "mirnext/CodeGen/Detail/Vector.hpp"
#include <bit>
/* Licensed under the MIT License. Register-only v128 lowering on SSE2/NEON. */
#include "mirnext/CodeGen/Detail/NativeVector.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include <algorithm>
#include <array>
#include <cstring>

namespace mirnext::codegen::detail {
namespace {
using ir::Opcode;
using ir::Type;
using P = MachinePseudoOpcode;
using K = Type::Kind;
struct VectorPreparation {
  MachineFunction &function;
  MachineBasicBlock &block;
  MachineRewriter rewriter;
  VectorConstantCache &cache;
  std::optional<Error> error;
  bool arm;
  VectorPreparation (MachineFunction &f, MachineBasicBlock &b, VectorConstantCache &c)
    : function (f),
      block (b),
      rewriter (f),
      cache (c),
      arm (f.target ().architecture () == target::Architecture::AArch64) {}
  MachineOperand number (std::uint64_t n) { return MachineOperand (n); }
  MachineOperand reg (Type type) {
    if (error) return number (0);
    auto result = access (function).create_register (type);
    if (!result.has_value ()) {
      error = result.error ();
      return number (0);
    }
    return MachineOperand (result.value ());
  }
  void emit (MachineOpcode opcode, std::vector<MachineOperand> operands) {
    if (error) return;
    auto result = rewriter.append (block, opcode, std::move (operands));
    if (!result.has_value ()) error = result.error ();
  }
  static Opcode move (Type type) {
    return type.is_vector ()        ? Opcode::VMov
           : type.kind () == K::F32 ? Opcode::FMov
           : type.kind () == K::F64 ? Opcode::DMov
                                    : Opcode::Mov;
  }
  MachineOperand input (const MachineOperand &value, Type type) {
    if (operand_access (value).get<const MachineVirtualRegister *> ()
        || operand_access (value).get<target::PhysicalRegister> ())
      return value;
    auto result = reg (type);
    emit (move (type), {result, value});
    return result;
  }
  MachineOperand op (P opcode, Type type, std::initializer_list<MachineOperand> inputs = {}) {
    auto output = reg (type);
    std::vector<MachineOperand> operands{output};
    operands.insert (operands.end (), inputs.begin (), inputs.end ());
    // Ties are explicit before allocation. A fresh temporary preserves aliased inputs.
    if (native_vector_descriptor (opcode)->tied) {
      emit (move (type), {output, operands[1]});
      operands[1] = output;
    }
    emit (opcode, std::move (operands));
    return output;
  }
  MachineOperand binary (Opcode code, Type type, MachineOperand a, MachineOperand b) {
    auto output = reg (type);
    emit (code, {output, a, b});
    return output;
  }
  MachineOperand bit_not (Type type, MachineOperand a) {
    if (arm) return op (P::A64VectorNot, type, {a});
    ir::VectorConstant ones{type};
    ones.bytes.fill (std::byte (255));
    return binary (Opcode::VXor, type, a, constant (ones));
  }
  MachineOperand constant (const ir::VectorConstant &constant) {
    auto key = std::pair (constant.type, constant.bytes);
    if (auto at = cache.find (key); at != cache.end ()) return at->second;
    MachineOperand result = number (0);
    if (std::ranges::all_of (constant.bytes, [] (auto b) { return b == std::byte (0); }))
      result = op (arm ? P::A64VectorZero : P::X86VectorZero, constant.type);
    else if (std::ranges::all_of (constant.bytes, [] (auto b) { return b == std::byte (255); }))
      result = op (arm ? P::A64VectorOnes : P::X86VectorOnes, constant.type);
    else {
      std::uint64_t low, high;
      std::memcpy (&low, constant.bytes.data (), 8);
      std::memcpy (&high, constant.bytes.data () + 8, 8);
      auto first = input (number (low), Type::i64 ());
      if (arm) {
        result = op (P::A64VectorDupGP, constant.type, {first, number (8)});
        if (low != high)
          result = op (P::A64VectorInsertGP, constant.type,
                       {result, input (number (high), Type::i64 ()), number (24)});
      } else {
        first = op (P::X86VectorFromGP, constant.type, {first});
        auto second = low == high ? first
                                  : op (P::X86VectorFromGP, constant.type,
                                        {input (number (high), Type::i64 ())});
        result = op (P::X86Unpack64, constant.type, {first, second});
      }
    }
    if (!error) cache.emplace (std::move (key), result);
    return result;
  }
  MachineOperand splat (Type type, MachineOperand scalar) {
    const unsigned width = 16 / type.vector_lanes ();
    scalar = input (scalar, type.vector_lane_type ().is_integer () ? Type::i64 ()
                                                                   : type.vector_lane_type ());
    if (arm)
      return op (type.vector_lane_type ().is_float () ? P::A64VectorDupLane : P::A64VectorDupGP,
                 type, {scalar, number (width)});
    auto vector
      = op (type.vector_lane_type ().is_float () ? P::X86VectorCopyBits : P::X86VectorFromGP, type,
            {scalar});
    if (width == 1) vector = op (P::X86Unpack8, type, {vector, vector});
    if (width <= 2) vector = op (P::X86Unpack16, type, {vector, vector});
    return width <= 4 ? op (P::X86Shuffle32, type, {vector, number (0)})
                      : op (P::X86Unpack64, type, {vector, vector});
  }
  MachineOperand shifted (Type type, MachineOperand value, unsigned amount, bool left) {
    if (!amount) return value;
    return op (left ? P::X86VectorLeft : P::X86VectorRight, type, {value, number (amount)});
  }
  MachineOperand extract (Type type, MachineOperand value, unsigned lane) {
    const auto scalar = type.vector_lane_type ();
    const unsigned width = 16 / type.vector_lanes ();
    const bool signed_integer
      = scalar.is_integer () && static_cast<unsigned> (type.vector_element ()) % 2 == 0;
    if (arm) {
      auto code = scalar.is_float ()            ? P::A64VectorDupLane
                  : signed_integer && width < 8 ? P::A64VectorExtractSigned
                                                : P::A64VectorExtract;
      return op (code, scalar.is_float () ? scalar : Type::i64 (),
                 {value, number (width + lane * width * 2)});
    }
    value = shifted (type, value, lane * width, false);
    if (scalar.is_float ()) return op (P::X86VectorCopyBits, scalar, {value});
    value = op (P::X86VectorToGP, Type::i64 (), {value});
    if (width < 8) {
      constexpr Opcode extensions[]{Opcode::Ext8, Opcode::Ext16, Opcode::Ext32};
      constexpr Opcode unsigned_extensions[]{Opcode::UExt8, Opcode::UExt16, Opcode::UExt32};
      auto code = (signed_integer ? extensions : unsigned_extensions)[width == 1   ? 0
                                                                      : width == 2 ? 1
                                                                                   : 2];
      auto output = reg (Type::i64 ());
      emit (code, {output, value});
      value = output;
    }
    return value;
  }
  MachineOperand insert (Type type, MachineOperand value, MachineOperand scalar, unsigned lane) {
    const unsigned width = 16 / type.vector_lanes ();
    scalar = input (scalar,
                    type.vector_lane_type ().is_float () ? type.vector_lane_type () : Type::i64 ());
    if (arm)
      return op (type.vector_lane_type ().is_float () ? P::A64VectorInsertFP : P::A64VectorInsertGP,
                 type, {value, scalar, number (width + lane * width * 2)});
    auto low = op (type.vector_lane_type ().is_float () ? P::X86VectorCopyBits : P::X86VectorFromGP,
                   type, {scalar});
    low = shifted (type, low, 16 - width, true);
    low = shifted (type, low, 16 - width, false);
    low = shifted (type, low, lane * width, true);
    ir::VectorConstant mask{type};
    mask.bytes.fill (std::byte (255));
    std::fill_n (mask.bytes.begin () + lane * width, width, std::byte (0));
    return binary (Opcode::VOr, type, low, binary (Opcode::VAnd, type, value, constant (mask)));
  }
  MachineOperand shuffle (Type type, MachineOperand a, MachineOperand b,
                          const ir::VectorConstant &mask) {
    const unsigned lanes = type.vector_lanes (), width = 16 / lanes;
    auto index = [&] (unsigned n) { return std::to_integer<unsigned> (mask.bytes[n]); };
    bool identity_a = true, identity_b = true, broadcast = true;
    for (unsigned n = 0; n < lanes; ++n) {
      identity_a &= index (n) == n;
      identity_b &= index (n) == n + lanes;
      broadcast &= index (n) == index (0);
    }
    if (identity_a) return a;
    if (identity_b) return b;
    if (arm) {
      if (broadcast)
        return op (P::A64VectorDupLane, type,
                   {index (0) < lanes ? a : b, number (width + (index (0) % lanes) * width * 2)});
      bool contiguous = true, reverse = true, zip1 = true, zip2 = true;
      for (unsigned n = 0; n < lanes; ++n) {
        contiguous &= index (n) == (index (0) + n) % (lanes * 2);
        reverse &= index (n) == lanes - 1 - n;
        zip1 &= index (n) == n / 2 + (n % 2 ? lanes : 0);
        zip2 &= index (n) == n / 2 + lanes / 2 + (n % 2 ? lanes : 0);
      }
      if (contiguous)
        return op (P::A64VectorExt, type,
                   {index (0) < lanes ? a : b, index (0) < lanes ? b : a,
                    number ((index (0) % lanes) * width)});
      unsigned size = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3;
      constexpr P zip_first[]{P::A64VectorZip18, P::A64VectorZip116, P::A64VectorZip132,
                              P::A64VectorZip164};
      constexpr P zip_second[]{P::A64VectorZip28, P::A64VectorZip216, P::A64VectorZip232,
                               P::A64VectorZip264};
      if (zip1 || zip2) return op ((zip1 ? zip_first : zip_second)[size], type, {a, b});
      if (reverse) {
        constexpr P rev[]{P::A64VectorRev8, P::A64VectorRev16, P::A64VectorRev32};
        auto reversed = width < 8 ? op (rev[size], type, {a}) : a;
        return op (P::A64VectorExt, type, {reversed, reversed, number (8)});
      }
      auto bytes = Type::vector128 (Type::VectorElement::U8, 16);
      ir::VectorConstant first{bytes}, second{bytes};
      for (unsigned n = 0; n < 16; ++n) {
        unsigned at = index (n / width) * width + n % width;
        first.bytes[n] = std::byte (at < 16 ? at : 255);
        second.bytes[n] = std::byte (at >= 16 ? at - 16 : 255);
      }
      auto one = op (P::A64VectorTable, type, {a, constant (first)});
      auto two = op (P::A64VectorTable, type, {b, constant (second)});
      return binary (Opcode::VOr, type, one, two);
    }
    if (broadcast)
      return splat (type, extract (type, index (0) < lanes ? a : b, index (0) % lanes));
    if (width >= 4) {
      bool only_a = true, only_b = true;
      unsigned immediate = 0;
      for (unsigned n = 0; n < lanes; ++n) {
        only_a &= index (n) < lanes;
        only_b &= index (n) >= lanes;
        unsigned lane = index (n) % lanes;
        if (width == 4)
          immediate |= lane << (2 * n);
        else
          immediate |= (2 * lane | ((2 * lane + 1) << 2)) << (4 * n);
      }
      if (only_a || only_b) return op (P::X86Shuffle32, type, {only_a ? a : b, number (immediate)});
    }
    bool unpack = true, unpack_high = true;
    for (unsigned n = 0; n < lanes; ++n) {
      unpack &= index (n) == n / 2 + (n % 2 ? lanes : 0);
      unpack_high &= index (n) == n / 2 + lanes / 2 + (n % 2 ? lanes : 0);
    }
    if (unpack || unpack_high) {
      constexpr P codes[]{P::X86Unpack8, P::X86Unpack16, P::X86Unpack32, P::X86Unpack64};
      if (unpack_high) {
        a = shifted (type, a, 8, false);
        b = shifted (type, b, 8, false);
      }
      return op (codes[width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3], type, {a, b});
    }
    // General SSE2 shuffle: isolate each lane with zero-filling byte shifts,
    // position it, then merge. No optional PSHUFB/PINSR/PEXTR instructions.
    auto result = op (P::X86VectorZero, type);
    for (unsigned n = 0; n < lanes; ++n) {
      unsigned at = index (n) % lanes;
      auto lane = index (n) < lanes ? a : b;
      lane = shifted (type, lane, 16 - (at + 1) * width, true);
      lane = shifted (type, lane, 16 - width, false);
      lane = shifted (type, lane, n * width, true);
      result = binary (Opcode::VOr, type, result, lane);
    }
    return result;
  }
  MachineOperand compare (Opcode code, Type type, MachineOperand a, MachineOperand b) {
    const unsigned width = 16 / type.vector_lanes ();
    const auto mask = type.vector_mask_type ();
    bool eq = code == Opcode::VEq || code == Opcode::VNe;
    bool negate = code == Opcode::VNe || code == Opcode::VLe || code == Opcode::VGe;
    bool swap = code == Opcode::VLt || code == Opcode::VGe;
    if (type.vector_lane_type ().is_float ()) {
      bool flip = code == Opcode::VGt || code == Opcode::VGe;
      if (flip) std::swap (a, b);
      if (arm) {
        P op_code;
        if (eq)
          op_code = width == 4 ? P::A64VectorFEq32 : P::A64VectorFEq64;
        else {
          std::swap (a, b);
          bool inclusive = code == Opcode::VLe || code == Opcode::VGe;
          op_code = width == 4 ? (inclusive ? P::A64VectorFGe32 : P::A64VectorFGt32)
                               : (inclusive ? P::A64VectorFGe64 : P::A64VectorFGt64);
        }
        auto result = op (op_code, mask, {a, b});
        return code == Opcode::VNe ? bit_not (mask, result) : result;
      }
      unsigned predicate
        = eq ? (code == Opcode::VNe ? 4 : 0) : (code == Opcode::VLe || code == Opcode::VGe ? 2 : 1);
      return op (width == 4 ? P::X86CompareF32 : P::X86CompareF64, mask,
                 {a, b, number (predicate)});
    }
    if (swap) std::swap (a, b);
    bool unsigned_integer = static_cast<unsigned> (type.vector_element ()) % 2 != 0;
    MachineOperand result = number (0);
    const unsigned size = width == 1 ? 0 : width == 2 ? 1 : width == 4 ? 2 : 3;
    if (arm) {
      constexpr P equals[]{P::A64VectorEq8, P::A64VectorEq16, P::A64VectorEq32, P::A64VectorEq64};
      constexpr P greater[]{P::A64VectorGt8, P::A64VectorGt16, P::A64VectorGt32, P::A64VectorGt64};
      constexpr P higher[]{P::A64VectorHi8, P::A64VectorHi16, P::A64VectorHi32, P::A64VectorHi64};
      result = op ((eq ? equals : unsigned_integer ? higher : greater)[size], mask, {a, b});
    } else {
      if (!eq && unsigned_integer) {
        ir::VectorConstant bias{type};
        for (unsigned n = width - 1; n < 16; n += width) bias.bytes[n] = std::byte (128);
        auto sign = constant (bias);
        a = binary (Opcode::VXor, type, a, sign);
        b = binary (Opcode::VXor, type, b, sign);
      }
      constexpr P equals[]{P::X86VectorEq8, P::X86VectorEq16, P::X86VectorEq32};
      constexpr P greater[]{P::X86VectorGt8, P::X86VectorGt16, P::X86VectorGt32};
      if (width < 8)
        result = op ((eq ? equals : greater)[size], mask, {a, b});
      else {
        auto equal32 = op (P::X86VectorEq32, mask, {a, b});
        if (eq) {
          auto swapped = op (P::X86Shuffle32, mask, {equal32, number (0xb1)});
          result = binary (Opcode::VAnd, mask, equal32, swapped);
        } else {
          auto high = op (P::X86VectorGt32, mask, {a, b});
          high = op (P::X86Shuffle32, mask, {high, number (0xf5)});
          auto high_eq = op (P::X86Shuffle32, mask, {equal32, number (0xf5)});
          ir::VectorConstant bias{type};
          for (unsigned n : {3u, 11u}) bias.bytes[n] = std::byte (128);
          auto sign = constant (bias);
          auto low_a = binary (Opcode::VXor, type, a, sign);
          auto low_b = binary (Opcode::VXor, type, b, sign);
          auto low = op (P::X86VectorGt32, mask, {low_a, low_b});
          low = op (P::X86Shuffle32, mask, {low, number (0xa0)});
          result = binary (Opcode::VOr, mask, high, binary (Opcode::VAnd, mask, high_eq, low));
        }
      }
    }
    return negate ? bit_not (mask, result) : result;
  }
  MachineOperand gp (Opcode code, MachineOperand a, MachineOperand b) {
    return binary (code, Type::i64 (), input (a, Type::i64 ()), input (b, Type::i64 ()));
  }
  MachineOperand scalar (Opcode code, Type type, MachineOperand a) {
    auto result = reg (type);
    emit (code, {result, a});
    return result;
  }
  MachineOperand choose (MachineOperand condition, MachineOperand a, MachineOperand b) {
    auto mask = gp (Opcode::Sub, number (0), condition);
    return gp (Opcode::Or, gp (Opcode::And, a, mask),
               gp (Opcode::And, b, gp (Opcode::Xor, mask, number (UINT64_MAX))));
  }
  MachineOperand select (Type type, MachineOperand mask, MachineOperand a, MachineOperand b) {
    return binary (Opcode::VOr, type, binary (Opcode::VAnd, type, a, mask),
                   binary (Opcode::VAnd, type, b, bit_not (type, mask)));
  }
  MachineOperand zero (Type type) { return op (arm ? P::A64VectorZero : P::X86VectorZero, type); }
  MachineOperand raw_lane (Type type, MachineOperand value, unsigned lane) {
    auto bits = ir::detail::vector_integer (128 / type.vector_lanes (), false);
    return extract (bits, value, lane);
  }
  MachineOperand insert_bits (Type type, MachineOperand value, MachineOperand bits, unsigned lane) {
    unsigned width = 16 / type.vector_lanes ();
    if (arm)
      return op (P::A64VectorInsertGP, type,
                 {value, input (bits, Type::i64 ()), number (width + lane * width * 2)});
    return insert (ir::detail::vector_integer (width * 8, false), value, bits, lane);
  }
  MachineOperand splat_bits (Type type, std::uint64_t bits) {
    ir::VectorConstant c{type};
    unsigned width = 16 / type.vector_lanes ();
    for (unsigned n = 0; n < type.vector_lanes (); ++n)
      std::memcpy (c.bytes.data () + n * width, &bits, width);
    return constant (c);
  }
  MachineOperand integer_clamp (MachineOperand value, std::int64_t low, std::int64_t high) {
    value = choose (gp (Opcode::Lt, value, number (std::uint64_t (low))),
                    number (std::uint64_t (low)), value);
    return choose (gp (Opcode::Gt, value, number (std::uint64_t (high))),
                   number (std::uint64_t (high)), value);
  }
  MachineOperand extend (Type source, Type result, MachineOperand a, bool high) {
    unsigned bits = 128 / source.vector_lanes (), size = bits == 8 ? 0 : bits == 16 ? 1 : 2;
    bool sign = ir::detail::vector_signed (source);
    if (arm) {
      constexpr P signed_low[]{P::A64ExtendS8Low, P::A64ExtendS16Low, P::A64ExtendS32Low};
      constexpr P signed_high[]{P::A64ExtendS8High, P::A64ExtendS16High, P::A64ExtendS32High};
      constexpr P unsigned_low[]{P::A64ExtendU8Low, P::A64ExtendU16Low, P::A64ExtendU32Low};
      constexpr P unsigned_high[]{P::A64ExtendU8High, P::A64ExtendU16High, P::A64ExtendU32High};
      return op ((sign ? (high ? signed_high : signed_low) : (high ? unsigned_high : unsigned_low))
                   [size],
                 result, { a });
    }
    if (high) a = shifted (source, a, 8, false);
    auto fill = sign ? compare (Opcode::VLt, source, a, zero (source)) : zero (source);
    constexpr P unpack[]{P::X86Unpack8, P::X86Unpack16, P::X86Unpack32};
    return op (unpack[size], result, {a, fill});
  }
  MachineOperand multiply (Type type, MachineOperand a, MachineOperand b) {
    unsigned bits = 128 / type.vector_lanes ();
    if (type.vector_lane_type ().is_float ())
      return op (arm ? (bits == 32 ? P::A64MultiplyF32 : P::A64MultiplyF64)
                     : (bits == 32 ? P::X86MultiplyF32 : P::X86MultiplyF64),
                 type, {a, b});
    if (arm && bits < 64)
      return op (bits == 16 ? P::A64Multiply16 : P::A64Multiply32, type, {a, b});
    if (!arm) {
      if (bits == 16) return op (P::X86Multiply16, type, {a, b});
      if (bits == 32) {
        auto even = op (P::X86MultiplyU32Wide, type, {a, b});
        auto odd = op (P::X86MultiplyU32Wide, type,
                       {shifted (type, a, 4, false), shifted (type, b, 4, false)});
        even = op (P::X86Shuffle32, type, {even, number (0x88)});
        odd = op (P::X86Shuffle32, type, {odd, number (0x88)});
        return op (P::X86Unpack32, type, {even, odd});
      }
      auto first = op (P::X86MultiplyU32Wide, type, {a, b});
      auto high_a = op (P::X86BitsRight64, type, {a, number (32)});
      auto high_b = op (P::X86BitsRight64, type, {b, number (32)});
      auto cross = binary (Opcode::VAdd, type, op (P::X86MultiplyU32Wide, type, {high_a, b}),
                           op (P::X86MultiplyU32Wide, type, {a, high_b}));
      cross = op (P::X86BitsLeft64, type, {cross, number (32)});
      return binary (Opcode::VAdd, type, first, cross);
    }
    // a*b modulo 2^64: the full low-word product plus cross products shifted
    // by 32. Keeping this in NEON avoids four UMOV and two INS transfers.
    auto words = ir::detail::vector_integer (32, false);
    auto reversed = op (P::A64VectorRev32, words, {b});
    auto cross = op (P::A64Multiply32, words, {a, reversed});
    cross = op (P::A64PairAdd32, words, {cross, cross});
    auto shifted_cross = op (P::A64WidenShift32, type, {cross});
    auto low_a = op (P::A64NarrowBits64, words, {a});
    auto low_b = op (P::A64NarrowBits64, words, {b});
    // Compute the full low-word product in parallel with the cross products.
    // A separate ADD shortens the dependency chain compared with UMLAL.
    auto low_product = op (P::A64ExtmulU32Low, type, {low_a, low_b});
    return binary (Opcode::VAdd, type, low_product, shifted_cross);
  }
  MachineOperand rounded_bits (Opcode code, unsigned width, MachineOperand bits) {
    const unsigned p = width == 4 ? 23 : 52, bias = width == 4 ? 127 : 1023;
    const std::uint64_t sign = UINT64_C (1) << (width * 8 - 1);
    const std::uint64_t inf = width == 4 ? UINT64_C (0x7f800000) : UINT64_C (0x7ff0000000000000);
    const std::uint64_t one = width == 4 ? UINT64_C (0x3f800000) : UINT64_C (0x3ff0000000000000);
    auto negative = gp (Opcode::Ne, gp (Opcode::And, bits, number (sign)), number (0));
    auto absolute = gp (Opcode::And, bits, number (sign - 1));
    auto exponent = gp (Opcode::URsh, absolute, number (p));
    auto small = gp (Opcode::ULt, exponent, number (bias));
    auto large = gp (Opcode::UGe, exponent, number (bias + p));
    auto shift = choose (large, number (0),
                         choose (small, number (p), gp (Opcode::Sub, number (bias + p), exponent)));
    auto unit = gp (Opcode::Lsh, number (1), shift);
    auto mask = gp (Opcode::Sub, unit, number (1));
    auto fraction = gp (Opcode::And, absolute, mask);
    auto integral = gp (Opcode::And, absolute, gp (Opcode::Xor, mask, number (UINT64_MAX)));
    auto nonzero = gp (Opcode::Ne, absolute, number (0));
    auto increment = number (0), small_increment = number (0);
    if (code == Opcode::VCeil || code == Opcode::VFloor) {
      auto direction = code == Opcode::VFloor ? negative : gp (Opcode::Eq, negative, number (0));
      increment = gp (Opcode::And, direction, gp (Opcode::Ne, fraction, number (0)));
      small_increment = gp (Opcode::And, direction, nonzero);
    } else if (code == Opcode::VNearest) {
      auto half = gp (Opcode::URsh, unit, number (1));
      auto odd = gp (Opcode::Ne, gp (Opcode::And, integral, unit), number (0));
      increment = gp (Opcode::Or, gp (Opcode::UGt, fraction, half),
                      gp (Opcode::And, gp (Opcode::Eq, fraction, half), odd));
      small_increment = gp (Opcode::UGt, absolute, number (one - (UINT64_C (1) << p)));
    }
    integral = gp (Opcode::Add, integral, gp (Opcode::Mul, increment, unit));
    auto result = choose (small, choose (small_increment, number (one), number (0)), integral);
    result = choose (large, absolute, result);
    result = gp (Opcode::Or, result, gp (Opcode::And, bits, number (sign)));
    auto nan = gp (Opcode::UGt, absolute, number (inf));
    return gp (Opcode::Or, result, choose (nan, number (UINT64_C (1) << (p - 1)), number (0)));
  }
  MachineOperand scalar_from_bits (unsigned width, MachineOperand bits) {
    auto type = Type::vector128 (width == 4 ? Type::VectorElement::F32 : Type::VectorElement::F64,
                                 width == 4 ? 4 : 2);
    auto vector
      = op (arm ? P::A64VectorDupGP : P::X86VectorFromGP, type,
            arm ? std::initializer_list<MachineOperand>{input (bits, Type::i64 ()), number (width)}
                : std::initializer_list<MachineOperand>{input (bits, Type::i64 ())});
    return arm ? op (P::A64VectorDupLane, type.vector_lane_type (), {vector, number (width)})
               : op (P::X86VectorCopyBits, type.vector_lane_type (), {vector});
  }
  MachineOperand float_literal (unsigned width, double value) {
    std::uint64_t bits = width == 4 ? std::bit_cast<std::uint32_t> (float (value))
                                    : std::bit_cast<std::uint64_t> (value);
    return scalar_from_bits (width, input (number (bits), Type::i64 ()));
  }
  MachineOperand trunc_sat (Type source, Type destination, MachineOperand a, unsigned n) {
    const unsigned width = 16 / source.vector_lanes ();
    const bool sign = ir::detail::vector_signed (destination);
    auto x = extract (source, a, n);
    auto high = float_literal (width, sign ? 2147483648.0 : 4294967296.0);
    auto low = float_literal (width, sign ? -2147483648.0 : 0.0);
    auto fp_compare
      = [&] (Opcode single, Opcode twice, MachineOperand first, MachineOperand second) {
          return binary (width == 4 ? single : twice, Type::i64 (), first, second);
        };
    auto over = fp_compare (Opcode::FGe, Opcode::DGe, x, high);
    auto under = fp_compare (Opcode::FLt, Opcode::DLt, x, low);
    auto numeric = fp_compare (Opcode::FEq, Opcode::DEq, x, x);
    auto outside = gp (Opcode::Or, over, under);
    auto valid = gp (Opcode::And, numeric, gp (Opcode::Eq, outside, number (0)));
    auto safe_bits = gp (Opcode::And, raw_lane (source, a, n), gp (Opcode::Sub, number (0), valid));
    auto safe = scalar_from_bits (width, safe_bits);
    auto value = scalar (width == 4 ? Opcode::F2I : Opcode::D2I, Type::i64 (), safe);
    value = choose (over, number (sign ? UINT64_C (2147483647) : UINT64_C (4294967295)), value);
    return choose (under, number (sign ? std::uint64_t (INT64_C (-2147483648)) : 0), value);
  }
  MachineOperand extended (Opcode code, Type source, Type destination,
                           std::span<const MachineOperand> operands) {
    auto a = input (operands[1], source);
    auto b = operands.size () > 2 ? operands[2] : number (0);
    const unsigned width = 16 / source.vector_lanes (), bits = width * 8,
                   lanes = source.vector_lanes ();
    const bool sign = ir::detail::vector_signed (source);
    if (code == Opcode::VBitcast) return a;
    if (code == Opcode::VBitselect)
      return select (destination, a, input (operands[2], destination),
                     input (operands[3], destination));
    if (code == Opcode::VMul) return multiply (source, a, input (b, source));
    if (code == Opcode::VDiv)
      return op (arm ? (width == 4 ? P::A64DivideF32 : P::A64DivideF64)
                     : (width == 4 ? P::X86DivideF32 : P::X86DivideF64),
                 source, {a, input (b, source)});
    if (code == Opcode::VSqrt)
      return op (arm ? (width == 4 ? P::A64SqrtF32 : P::A64SqrtF64)
                     : (width == 4 ? P::X86SqrtF32 : P::X86SqrtF64),
                 source, {a});
    if (code == Opcode::VNeg || code == Opcode::VAbs) {
      if (source.vector_lane_type ().is_float ()) {
        auto sign_bit = splat_bits (source, UINT64_C (1) << (bits - 1));
        return code == Opcode::VNeg ? binary (Opcode::VXor, source, a, sign_bit)
                                    : binary (Opcode::VAnd, source, a, bit_not (source, sign_bit));
      }
      if (arm) {
        constexpr P negate[]{P::A64Negate8, P::A64Negate16, P::A64Negate32, P::A64Negate64};
        constexpr P absolute[]{P::A64Absolute8, P::A64Absolute16, P::A64Absolute32,
                               P::A64Absolute64};
        return op ((code == Opcode::VNeg ? negate : absolute)[width == 1   ? 0
                                                              : width == 2 ? 1
                                                              : width == 4 ? 2
                                                                           : 3],
                   source, {a});
      }
      auto negated = binary (Opcode::VSub, source, zero (source), a);
      return code == Opcode::VNeg
               ? negated
               : select (source, compare (Opcode::VLt, source, a, zero (source)), negated, a);
    }
    if (code == Opcode::VAndNot)
      return binary (Opcode::VAnd, source, a, bit_not (source, input (b, source)));
    if (code == Opcode::VAnyTrue || code == Opcode::VAllTrue || code == Opcode::VBitmask) {
      auto answer = number (code == Opcode::VAllTrue ? 1 : 0);
      auto shape = code == Opcode::VAnyTrue ? ir::detail::vector_integer (64, false) : source;
      for (unsigned n = 0; n < shape.vector_lanes (); ++n) {
        auto value = raw_lane (shape, a, n);
        if (code == Opcode::VBitmask)
          value = gp (Opcode::Lsh, gp (Opcode::URsh, value, number (bits - 1)), number (n));
        else
          value = gp (Opcode::Ne, value, number (0));
        answer = gp (code == Opcode::VAllTrue ? Opcode::And : Opcode::Or, answer, value);
      }
      return answer;
    }
    if (code == Opcode::VMin || code == Opcode::VMax || code == Opcode::VPMin
        || code == Opcode::VPMax) {
      b = input (b, source);
      bool minimum = code == Opcode::VMin || code == Opcode::VPMin;
      if (source.vector_lane_type ().is_float ()) {
        if (code == Opcode::VPMin || code == Opcode::VPMax)
          return select (source,
                         minimum ? compare (Opcode::VLt, source, b, a)
                                 : compare (Opcode::VLt, source, a, b),
                         b, a);
        auto result
          = select (source, compare (minimum ? Opcode::VLt : Opcode::VGt, source, a, b), a, b);
        auto z = zero (source);
        auto both_zero = binary (Opcode::VAnd, source, compare (Opcode::VEq, source, a, z),
                                 compare (Opcode::VEq, source, b, z));
        result = select (source, both_zero,
                         binary (minimum ? Opcode::VOr : Opcode::VAnd, source, a, b), result);
        auto numeric = binary (Opcode::VAnd, source, compare (Opcode::VEq, source, a, a),
                               compare (Opcode::VEq, source, b, b));
        auto canonical
          = splat_bits (source, width == 4 ? UINT64_C (0x7fc00000) : UINT64_C (0x7ff8000000000000));
        return select (source, numeric, result, canonical);
      }
      if (arm) {
        constexpr P min_s[]{P::A64MinS8, P::A64MinS16, P::A64MinS32},
          min_u[]{P::A64MinU8, P::A64MinU16, P::A64MinU32};
        constexpr P max_s[]{P::A64MaxS8, P::A64MaxS16, P::A64MaxS32},
          max_u[]{P::A64MaxU8, P::A64MaxU16, P::A64MaxU32};
        return op ((minimum ? (sign ? min_s : min_u) : (sign ? max_s : max_u)) [width == 1   ? 0
                                                                                : width == 2 ? 1
                                                                                             : 2],
                   source, { a, b });
      }
      return select (source, compare (minimum ? Opcode::VLt : Opcode::VGt, source, a, b), a, b);
    }
    if (code == Opcode::VAddSat || code == Opcode::VSubSat || code == Opcode::VAvgr) {
      b = input (b, source);
      unsigned n = width == 1 ? 0 : 1;
      if (code == Opcode::VAvgr)
        return op (arm ? (n ? P::A64Average16 : P::A64Average8)
                       : (n ? P::X86Average16 : P::X86Average8),
                   source, {a, b});
      constexpr P arm_add_s[]{P::A64SatAddS8, P::A64SatAddS16},
        arm_add_u[]{P::A64SatAddU8, P::A64SatAddU16};
      constexpr P arm_sub_s[]{P::A64SatSubS8, P::A64SatSubS16},
        arm_sub_u[]{P::A64SatSubU8, P::A64SatSubU16};
      constexpr P x86_add_s[]{P::X86SatAddS8, P::X86SatAddS16},
        x86_add_u[]{P::X86SatAddU8, P::X86SatAddU16};
      constexpr P x86_sub_s[]{P::X86SatSubS8, P::X86SatSubS16},
        x86_sub_u[]{P::X86SatSubU8, P::X86SatSubU16};
      auto table = arm ? (code == Opcode::VAddSat ? (sign ? arm_add_s : arm_add_u)
                                                  : (sign ? arm_sub_s : arm_sub_u))
                       : (code == Opcode::VAddSat ? (sign ? x86_add_s : x86_add_u)
                                                  : (sign ? x86_sub_s : x86_sub_u));
      return op (table[n], source, {a, b});
    }
    if (code == Opcode::VPopcnt) {
      if (arm) return op (P::A64CountBytes, source, {a});
      auto shifted_one = op (P::X86BitsRight16, source, {a, number (1)});
      a = binary (Opcode::VSub, source, a,
                  binary (Opcode::VAnd, source, shifted_one, splat_bits (source, 0x55)));
      a = binary (Opcode::VAdd, source, binary (Opcode::VAnd, source, a, splat_bits (source, 0x33)),
                  binary (Opcode::VAnd, source, op (P::X86BitsRight16, source, {a, number (2)}),
                          splat_bits (source, 0x33)));
      return binary (Opcode::VAnd, source,
                     binary (Opcode::VAdd, source, a,
                             op (P::X86BitsRight16, source, {a, number (4)})),
                     splat_bits (source, 0x0f));
    }
    if (code == Opcode::VShl || code == Opcode::VShr) {
      auto count = gp (Opcode::And, b, number (bits - 1));
      if (arm) {
        if (code == Opcode::VShr) count = gp (Opcode::Sub, number (0), count);
        auto shifts = splat (ir::detail::vector_integer (bits, true), count);
        constexpr P signed_shift[]{P::A64SShift8, P::A64SShift16, P::A64SShift32, P::A64SShift64};
        constexpr P unsigned_shift[]{P::A64UShift8, P::A64UShift16, P::A64UShift32, P::A64UShift64};
        return op ((code == Opcode::VShr && sign ? signed_shift : unsigned_shift)[width == 1   ? 0
                                                                                  : width == 2 ? 1
                                                                                  : width == 4 ? 2
                                                                                               : 3],
                   source, {a, shifts});
      }
      if (width > 1 && !(width == 8 && sign && code == Opcode::VShr)) {
        auto shifts = op (P::X86VectorFromGP, ir::detail::vector_integer (64, false),
                          {input (count, Type::i64 ())});
        constexpr P left[]{P::X86VariableLeft16, P::X86VariableLeft32, P::X86VariableLeft64};
        constexpr P right[]{P::X86VariableRight16, P::X86VariableRight32, P::X86VariableRight64};
        constexpr P signed_right[]{P::X86VariableSignedRight16, P::X86VariableSignedRight32};
        unsigned n = width == 2 ? 0 : width == 4 ? 1 : 2;
        return op ((code == Opcode::VShl ? left
                    : sign               ? signed_right
                                         : right)[n],
                   source, {a, shifts});
      }
      auto result = zero (destination);
      for (unsigned n = 0; n < lanes; ++n)
        result = insert_bits (destination, result,
                              gp (code == Opcode::VShl ? Opcode::Lsh
                                  : sign               ? Opcode::Rsh
                                                       : Opcode::URsh,
                                  extract (source, a, n), count),
                              n);
      return result;
    }
    if (code == Opcode::VSwizzle) {
      b = input (b, ir::detail::vector_integer (8, false));
      if (arm) return op (P::A64VectorTable, source, {a, b});
      auto result = zero (source);
      for (unsigned n = 0; n < 16; ++n) {
        auto selected = compare (Opcode::VEq, ir::detail::vector_integer (8, false), b,
                                 splat_bits (ir::detail::vector_integer (8, false), n));
        result = binary (Opcode::VOr, source, result,
                         binary (Opcode::VAnd, source, splat (source, raw_lane (source, a, n)),
                                 selected));
      }
      return result;
    }
    if (code == Opcode::VExtendLow || code == Opcode::VExtendHigh)
      return extend (source, destination, a, code == Opcode::VExtendHigh);
    if (code == Opcode::VExtmulLow || code == Opcode::VExtmulHigh) {
      bool high = code == Opcode::VExtmulHigh;
      if (arm) {
        constexpr P sl[]{P::A64ExtmulS8Low, P::A64ExtmulS16Low, P::A64ExtmulS32Low},
          sh[]{P::A64ExtmulS8High, P::A64ExtmulS16High, P::A64ExtmulS32High};
        constexpr P ul[]{P::A64ExtmulU8Low, P::A64ExtmulU16Low, P::A64ExtmulU32Low},
          uh[]{P::A64ExtmulU8High, P::A64ExtmulU16High, P::A64ExtmulU32High};
        return op ((sign ? (high ? sh : sl) : (high ? uh : ul)) [width == 1   ? 0
                                                                 : width == 2 ? 1
                                                                              : 2],
                   destination, { a, input (b, source) });
      }
      return multiply (destination, extend (source, destination, a, high),
                       extend (source, destination, input (b, source), high));
    }
    if (code == Opcode::VExtaddPairwise) {
      if (arm)
        return op (sign ? (width == 1 ? P::A64PairwiseS8 : P::A64PairwiseS16)
                        : (width == 1 ? P::A64PairwiseU8 : P::A64PairwiseU16),
                   destination, {a});
      auto even
        = binary (Opcode::VAnd, destination, a, splat_bits (destination, width == 1 ? 255 : 65535));
      P left = width == 1 ? P::X86BitsLeft16 : P::X86BitsLeft32;
      P right = sign ? (width == 1 ? P::X86BitsSignedRight16 : P::X86BitsSignedRight32)
                     : (width == 1 ? P::X86BitsRight16 : P::X86BitsRight32);
      if (sign)
        even
          = op (right, destination, {op (left, destination, {even, number (bits)}), number (bits)});
      auto odd = op (right, destination, {a, number (bits)});
      return binary (Opcode::VAdd, destination, even, odd);
    }
    if (code == Opcode::VNarrowSat) {
      bool signed_result = ir::detail::vector_signed (destination);
      if (arm) {
        auto first = op (signed_result ? (width == 2 ? P::A64NarrowS16Low : P::A64NarrowS32Low)
                                       : (width == 2 ? P::A64NarrowU16Low : P::A64NarrowU32Low),
                         destination, {a});
        return op (signed_result ? (width == 2 ? P::A64NarrowS16High : P::A64NarrowS32High)
                                 : (width == 2 ? P::A64NarrowU16High : P::A64NarrowU32High),
                   destination, {first, input (b, source)});
      }
      if (width == 2 || signed_result)
        return op (width == 2 ? (signed_result ? P::X86NarrowS16 : P::X86NarrowU16)
                              : P::X86NarrowS32,
                   destination, {a, input (b, source)});
      auto result = zero (destination);
      b = input (b, source);
      for (unsigned n = 0; n < lanes * 2; ++n)
        result
          = insert_bits (destination, result,
                         integer_clamp (extract (source, n < lanes ? a : b, n % lanes), 0, 65535),
                         n);
      return result;
    }
    if (code == Opcode::VDot) {
      b = input (b, source);
      if (!arm) return op (P::X86Dot16, destination, {a, b});
      auto low = op (P::A64ExtmulS16Low, destination, {a, b});
      auto high = op (P::A64ExtmulS16High, destination, {a, b});
      return op (P::A64PairAdd32, destination, {low, high});
    }
    if (code == Opcode::VQ15MulrSat && arm)
      return op (P::A64Q15Multiply, destination, {a, input (b, source)});
    if (code == Opcode::VDot || code == Opcode::VQ15MulrSat) {
      auto result = zero (destination);
      b = input (b, source);
      for (unsigned n = 0; n < destination.vector_lanes (); ++n) {
        auto i = code == Opcode::VDot ? 2 * n : n;
        auto product = gp (Opcode::Mul, extract (source, a, i), extract (source, b, i));
        if (code == Opcode::VDot)
          product = gp (Opcode::Add, product,
                        gp (Opcode::Mul, extract (source, a, i + 1), extract (source, b, i + 1)));
        else
          product = integer_clamp (gp (Opcode::Rsh, gp (Opcode::Add, product, number (16384)),
                                       number (15)),
                                   -32768, 32767);
        result = insert_bits (destination, result, product, n);
      }
      return result;
    }
    if (code == Opcode::VCeil || code == Opcode::VFloor || code == Opcode::VTrunc
        || code == Opcode::VNearest) {
      if (arm) {
        P instruction = code == Opcode::VCeil    ? (width == 4 ? P::A64CeilF32 : P::A64CeilF64)
                        : code == Opcode::VFloor ? (width == 4 ? P::A64FloorF32 : P::A64FloorF64)
                        : code == Opcode::VTrunc
                          ? (width == 4 ? P::A64TruncF32 : P::A64TruncF64)
                          : (width == 4 ? P::A64NearestF32 : P::A64NearestF64);
        return op (instruction, source, {a});
      }
      auto result = zero (destination);
      for (unsigned n = 0; n < lanes; ++n)
        result = insert_bits (destination, result,
                              rounded_bits (code, width, raw_lane (source, a, n)), n);
      return result;
    }
    if (code == Opcode::VConvert || code == Opcode::VConvertLow || code == Opcode::VConvertZero
        || code == Opcode::VTruncSat || code == Opcode::VTruncSatZero) {
      if (arm) {
        if (code == Opcode::VConvert)
          return op (sign ? P::A64ConvertS32 : P::A64ConvertU32, destination, {a});
        if (code == Opcode::VConvertLow) {
          if (source.vector_lane_type ().is_float ())
            return op (P::A64PromoteLow, destination, {a});
          auto wide = extend (source, ir::detail::vector_widened (source), a, false);
          return op (sign ? P::A64ConvertS64 : P::A64ConvertU64, destination, {wide});
        }
        if (code == Opcode::VConvertZero) return op (P::A64DemoteZero, destination, {a});
        if (code == Opcode::VTruncSat)
          return op (ir::detail::vector_signed (destination) ? P::A64TruncSatS32
                                                             : P::A64TruncSatU32,
                     destination, {a});
      }
      auto result = zero (destination);
      for (unsigned n = 0; n < std::min (lanes, destination.vector_lanes ()); ++n) {
        if (code == Opcode::VTruncSat || code == Opcode::VTruncSatZero)
          result = insert_bits (destination, result, trunc_sat (source, destination, a, n), n);
        else {
          auto lane = extract (source, a, n);
          Opcode convert = source.vector_lane_type ().is_integer ()
                             ? (destination.vector_lanes () == 4 ? Opcode::I2F : Opcode::I2D)
                             : (width == 4 ? Opcode::F2D : Opcode::D2F);
          auto converted = scalar (convert, destination.vector_lane_type (), lane);
          result = insert (destination, result, converted, n);
        }
      }
      return result;
    }
    error = Error{ErrorCode::InvalidOperand, "native SIMD128 operation has no lowering"};
    return number (0);
  }
  Result<void> status () const { return error ? Result<void> (*error) : Result<void> (); }
};
}  // namespace
Result<MachineOperand> prepare_vector_constant (MachineFunction &function, MachineBasicBlock &block,
                                                const ir::VectorConstant &constant,
                                                VectorConstantCache &cache) {
  VectorPreparation p (function, block, cache);
  auto result = p.constant (constant);
  auto status = p.status ();
  if (!status.has_value ()) return status.error ();
  return result;
}
Result<void> prepare_vector_operation (MachineFunction &function, MachineBasicBlock &block,
                                       Opcode opcode, Type type,
                                       std::span<const MachineOperand> operands,
                                       VectorConstantCache &cache,
                                       const ir::VectorConstant *shuffle_mask) {
  VectorPreparation p (function, block, cache);
  if (function.target ().architecture () != target::Architecture::AArch64
      && function.target ().architecture () != target::Architecture::X86_64)
    return Error{ErrorCode::InvalidOperand, "unsupported native vector target"};
  MachineOperand result (std::uint64_t (0));
  Type output_type
    = operand_access (operands[0]).get<const MachineVirtualRegister *> ()
        ? (*operand_access (operands[0]).get<const MachineVirtualRegister *> ())->type ()
        : operand_access (operands[0]).get<MachineMemory> ()->type;
  if (opcode != Opcode::VSplat && opcode != Opcode::VExtract && opcode != Opcode::VInsert
      && opcode != Opcode::VShuffle && !ir::detail::vector_comparison (opcode)) {
    result = p.extended (opcode, type, output_type, operands);
    p.emit (p.move (output_type), {operands[0], result});
    return p.status ();
  }
  if (opcode == Opcode::VSplat)
    result = p.splat (type, operands[1]);
  else {
    auto first = p.input (operands[1], type);
    if (opcode == Opcode::VExtract || opcode == Opcode::VInsert) {
      auto lane = native_integer (operands[opcode == Opcode::VExtract ? 2 : 3]);
      if (!lane || *lane >= type.vector_lanes ())
        return Error{ErrorCode::InvalidOperand, "invalid vector lane index at machine preparation"};
      if (opcode == Opcode::VExtract) {
        result = p.extract (type, first, *lane);
      } else
        result = p.insert (type, first, operands[2], *lane);
    } else {
      auto second = p.input (operands[2], type);
      if (opcode == Opcode::VShuffle) {
        if (!shuffle_mask) return Error{ErrorCode::InvalidOperand, "missing vector shuffle mask"};
        for (unsigned n = 0; n < type.vector_lanes (); ++n)
          if (std::to_integer<unsigned> (shuffle_mask->bytes[n]) >= type.vector_lanes () * 2)
            return Error{ErrorCode::InvalidOperand, "invalid vector shuffle index"};
        result = p.shuffle (type, first, second, *shuffle_mask);
      } else if (ir::detail::vector_comparison (opcode)) {
        result = p.compare (opcode, type, first, second);
      } else
        return Error{ErrorCode::InvalidOperand, "unknown native vector operation"};
    }
  }
  p.emit (p.move (output_type), {operands[0], result});
  return p.status ();
}
}  // namespace mirnext::codegen::detail
