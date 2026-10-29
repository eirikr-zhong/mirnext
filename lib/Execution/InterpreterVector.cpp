/* Licensed under the MIT License. Portable SIMD128 reference semantics. */
#include "mirnext/Execution/detail/Interpreter.hpp"
#include "mirnext/IR/Vector.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
namespace mirnext::execution::detail {
namespace {
using O = ir::Opcode;
using T = ir::Type;
std::uint64_t read_lane (const ScalarValue &value, unsigned width, unsigned n) {
  std::uint64_t bits = 0;
  std::memcpy (&bits, value.storage ().data () + width * n, width);
  return bits;
}
std::int64_t signed_lane (std::uint64_t bits, unsigned width) {
  auto sign = UINT64_C (1) << (width * 8 - 1);
  return std::bit_cast<std::int64_t> ((bits ^ sign) - sign);
}
std::uint64_t mask_for (unsigned width) {
  return width == 8 ? UINT64_MAX : (UINT64_C (1) << (width * 8)) - 1;
}
template <class F>
std::uint64_t floating_lane (O op, std::uint64_t first, std::uint64_t second) {
  using U = std::conditional_t<sizeof (F) == 4, std::uint32_t, std::uint64_t>;
  F a = std::bit_cast<F> (U (first)), b = std::bit_cast<F> (U (second)), result{};
  if (op == O::VPMin) return b < a ? second : first;
  if (op == O::VPMax) return a < b ? second : first;
  if (op == O::VMin || op == O::VMax) {
    if (std::isnan (a) || std::isnan (b))
      return sizeof (F) == 4 ? UINT64_C (0x7fc00000) : UINT64_C (0x7ff8000000000000);
    if (a == F (0) && b == F (0)) return op == O::VMin ? first | second : first & second;
    return op == O::VMin ? (a < b ? first : second) : (a > b ? first : second);
  }
  switch (op) {
  case O::VAdd: result = a + b; break;
  case O::VSub: result = a - b; break;
  case O::VMul: result = a * b; break;
  case O::VDiv: result = a / b; break;
  case O::VSqrt: result = std::sqrt (a); break;
  case O::VCeil: result = std::ceil (a); break;
  case O::VFloor: result = std::floor (a); break;
  case O::VTrunc: result = std::trunc (a); break;
  case O::VNearest: {
    if (!std::isfinite (a) || a == F (0)) {
      result = a;
      break;
    }
    F absolute = std::fabs (a), integral = std::floor (absolute), fraction = absolute - integral;
    if (fraction > F (0.5) || (fraction == F (0.5) && std::fmod (integral, F (2)) != F (0)))
      integral += F (1);
    result = std::copysign (integral, a);
    break;
  }
  default: break;
  }
  if (std::isnan (result))
    return sizeof (F) == 4 ? UINT64_C (0x7fc00000) : UINT64_C (0x7ff8000000000000);
  return std::bit_cast<U> (result);
}
template <class A>
bool compare (O op, A a, A b) {
  switch (op) {
  case O::VEq: return a == b;
  case O::VNe: return a != b;
  case O::VLt: return a < b;
  case O::VLe: return a <= b;
  case O::VGt: return a > b;
  case O::VGe: return a >= b;
  default: return false;
  }
}
}  // namespace
Result<ScalarValue> evaluate_vector (O op, T destination, std::span<const ScalarValue> values) {
  std::array<T, 3> types{T::i64 (), T::i64 (), T::i64 ()};
  if (values.empty () || values.size () > 3)
    return Error{ErrorCode::InvalidOperand, "invalid SIMD128 operand count"};
  for (unsigned n = 0; n < values.size (); ++n) types[n] = values[n].type ();
  if (!ir::detail::vector_types (op, destination, std::span (types).first (values.size ())))
    return Error{ErrorCode::InvalidOperand, "invalid SIMD128 operand/result types"};
  ScalarValue::Storage result{};
  const auto source = values[0].type ();
  const unsigned lanes = source.is_vector () ? source.vector_lanes () : destination.vector_lanes ();
  const unsigned width = 16 / lanes;
  const unsigned out_width = destination.is_vector () ? 16 / destination.vector_lanes () : 8;
  const bool sign = ir::detail::vector_signed (source);
  auto put = [&] (unsigned n, std::uint64_t bits) {
    std::memcpy (result.data () + n * out_width, &bits, out_width);
  };
  auto finish = [&] { return ScalarValue::from_storage (destination, result); };
  if (op == O::VMov || op == O::VBitcast) {
    result = values[0].storage ();
    return finish ();
  }
  if (op == O::VSplat) {
    for (unsigned n = 0; n < destination.vector_lanes (); ++n)
      std::memcpy (result.data () + n * out_width, values[0].storage ().data (), out_width);
    return finish ();
  }
  if (op == O::VExtract || op == O::VInsert) {
    auto index = values[op == O::VExtract ? 1 : 2].as_u64 ();
    if (!index.has_value () || index.value () >= lanes)
      return Error{ErrorCode::InvalidOperand, "invalid SIMD128 lane"};
    if (op == O::VInsert) {
      result = values[0].storage ();
      std::memcpy (result.data () + index.value () * width, values[1].storage ().data (), width);
    } else {
      auto bits = read_lane (values[0], width, index.value ());
      if (sign) bits = std::bit_cast<std::uint64_t> (signed_lane (bits, width));
      std::memcpy (result.data (), &bits, source.vector_lane_type ().is_float () ? width : 8);
    }
    return finish ();
  }
  if (op == O::VShuffle || op == O::VSwizzle) {
    for (unsigned n = 0; n < lanes; ++n) {
      auto index = std::to_integer<unsigned> (values[op == O::VShuffle ? 2 : 1].storage ()[n]);
      if (op == O::VSwizzle && index >= 16) continue;
      if (index >= 2 * lanes)
        return Error{ErrorCode::InvalidOperand, "invalid SIMD128 shuffle index"};
      put (n, read_lane (values[index < lanes ? 0 : 1], width, index % lanes));
    }
    return finish ();
  }
  if (op == O::VBitselect) {
    for (unsigned n = 0; n < 16; ++n)
      result[n] = (values[0].storage ()[n] & values[1].storage ()[n])
                  | (~values[0].storage ()[n] & values[2].storage ()[n]);
    return finish ();
  }
  if (op == O::VAnyTrue || op == O::VAllTrue || op == O::VBitmask) {
    std::uint64_t answer = op == O::VAllTrue;
    for (unsigned n = 0; n < lanes; ++n) {
      auto a = read_lane (values[0], width, n);
      if (op == O::VAnyTrue)
        answer |= a != 0;
      else if (op == O::VAllTrue)
        answer &= a != 0;
      else
        answer |= ((a >> (width * 8 - 1)) & 1) << n;
    }
    put (0, answer);
    return finish ();
  }
  if (op == O::VExtendLow || op == O::VExtendHigh || op == O::VExtmulLow || op == O::VExtmulHigh
      || op == O::VExtaddPairwise || op == O::VDot) {
    const bool high = op == O::VExtendHigh || op == O::VExtmulHigh;
    for (unsigned n = 0; n < destination.vector_lanes (); ++n) {
      auto read = [&] (unsigned arg, unsigned i) {
        auto bits = read_lane (values[arg], width, i);
        return sign ? std::bit_cast<std::uint64_t> (signed_lane (bits, width)) : bits;
      };
      auto i = op == O::VExtaddPairwise || op == O::VDot ? 2 * n : n + (high ? lanes / 2 : 0);
      auto value = read (0, i);
      if (op == O::VExtmulLow || op == O::VExtmulHigh) value *= read (1, i);
      if (op == O::VExtaddPairwise) value += read (0, i + 1);
      if (op == O::VDot) value = value * read (1, i) + read (0, i + 1) * read (1, i + 1);
      put (n, value);
    }
    return finish ();
  }
  if (op == O::VNarrowSat) {
    const bool signed_result = ir::detail::vector_signed (destination);
    auto low = signed_result ? -(INT64_C (1) << (out_width * 8 - 1)) : 0;
    auto high = signed_result ? (INT64_C (1) << (out_width * 8 - 1)) - 1
                              : (INT64_C (1) << (out_width * 8)) - 1;
    for (unsigned n = 0; n < lanes * 2; ++n)
      put (n, std::uint64_t (
                std::clamp (signed_lane (read_lane (values[n / lanes], width, n % lanes), width),
                            low, high)));
    return finish ();
  }
  if (op == O::VConvert || op == O::VConvertLow || op == O::VConvertZero || op == O::VTruncSat
      || op == O::VTruncSatZero) {
    unsigned count = std::min (lanes, destination.vector_lanes ());
    for (unsigned n = 0; n < count; ++n) {
      auto bits = read_lane (values[0], width, n);
      if (op == O::VTruncSat || op == O::VTruncSatZero) {
        double value = width == 4 ? double (std::bit_cast<float> (std::uint32_t (bits)))
                                  : std::bit_cast<double> (bits);
        bool signed_result = ir::detail::vector_signed (destination);
        double low = signed_result ? -2147483648.0 : 0.0,
               high = signed_result ? 2147483647.0 : 4294967295.0;
        std::int64_t integer = std::isnan (value) ? 0
                               : value <= low     ? std::int64_t (low)
                               : value >= high    ? std::int64_t (high)
                                                  : std::int64_t (std::trunc (value));
        put (n, std::uint64_t (integer));
      } else {
        double value = source.vector_lane_type ().is_float ()
                         ? (width == 4 ? double (std::bit_cast<float> (std::uint32_t (bits)))
                                       : std::bit_cast<double> (bits))
                       : sign ? double (signed_lane (bits, width))
                              : double (bits);
        if (out_width == 4)
          put (n, std::bit_cast<std::uint32_t> (float (value)));
        else
          put (n, std::bit_cast<std::uint64_t> (value));
      }
    }
    return finish ();
  }
  const auto mask = mask_for (width);
  for (unsigned n = 0; n < lanes; ++n) {
    auto a = read_lane (values[0], width, n),
         b = values.size () > 1 && values[1].type ().is_vector () ? read_lane (values[1], width, n)
                                                                  : 0;
    auto sa = signed_lane (a, width), sb = signed_lane (b, width);
    std::uint64_t bits = 0;
    if (op == O::VAnd)
      bits = a & b;
    else if (op == O::VOr)
      bits = a | b;
    else if (op == O::VXor)
      bits = a ^ b;
    else if (op == O::VAndNot)
      bits = a & ~b;
    else if (op == O::VNeg || op == O::VAbs) {
      if (source.vector_lane_type ().is_float ())
        bits = op == O::VNeg ? a ^ (UINT64_C (1) << (width * 8 - 1))
                             : a & ~(UINT64_C (1) << (width * 8 - 1));
      else
        bits = op == O::VNeg || sa < 0 ? 0 - a : a;
    } else if (ir::detail::vector_comparison (op)) {
      bool yes
        = source.vector_lane_type ().is_float ()
            ? (width == 4 ? compare (op, std::bit_cast<float> (std::uint32_t (a)),
                                     std::bit_cast<float> (std::uint32_t (b)))
                          : compare (op, std::bit_cast<double> (a), std::bit_cast<double> (b)))
          : sign ? compare (op, sa, sb)
                 : compare (op, a, b);
      bits = yes ? mask : 0;
    } else if (source.vector_lane_type ().is_float ())
      bits = width == 4 ? floating_lane<float> (op, a, b) : floating_lane<double> (op, a, b);
    else
      switch (op) {
      case O::VAdd: bits = a + b; break;
      case O::VSub: bits = a - b; break;
      case O::VMul: bits = a * b; break;
      case O::VShl:
      case O::VShr: {
        auto count = values[1].as_u64 ();
        if (!count.has_value ()) return count.error ();
        auto k = count.value () & (width * 8 - 1);
        bits = op == O::VShl ? a << k : sign ? std::uint64_t (sa >> k) : a >> k;
        break;
      }
      case O::VMin: bits = sign ? (sa < sb ? a : b) : (a < b ? a : b); break;
      case O::VMax: bits = sign ? (sa > sb ? a : b) : (a > b ? a : b); break;
      case O::VAddSat:
      case O::VSubSat: {
        auto low = sign ? -(INT64_C (1) << (width * 8 - 1)) : 0;
        auto high = sign ? (INT64_C (1) << (width * 8 - 1)) - 1 : std::int64_t (mask);
        auto first = sign ? sa : std::int64_t (a), second = sign ? sb : std::int64_t (b);
        bits = std::uint64_t (
          std::clamp (op == O::VAddSat ? first + second : first - second, low, high));
        break;
      }
      case O::VAvgr: bits = (a + b + 1) >> 1; break;
      case O::VPopcnt: bits = std::popcount (a); break;
      case O::VQ15MulrSat:
        bits = std::uint64_t (std::min ((sa * sb + 16384) >> 15, INT64_C (32767)));
        break;
      default: return Error{ErrorCode::InvalidOperand, "unknown SIMD128 operation"};
      }
    put (n, bits & mask);
  }
  return finish ();
}
}  // namespace mirnext::execution::detail
