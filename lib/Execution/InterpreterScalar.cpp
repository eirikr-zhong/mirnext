/* Licensed under the MIT License. Scalar interpreter arithmetic derived from MIR. */
#include "mirnext/Execution/detail/Interpreter.hpp"
#include "mirnext/Transforms/IntegerFolding.hpp"
#include <bit>
#include <cmath>
#include <limits>
namespace mirnext::execution::detail {
namespace {
using O = ir::Opcode;
Error invalid (std::string reason) { return {ErrorCode::InvalidOperand, std::move (reason)}; }
template <class T>
T raw (const ScalarValue &v) {
  T value;
  std::memcpy (&value, v.storage ().data (), sizeof value);
  return value;
}
template <class S>
void overflow (O op, std::uint64_t first, std::uint64_t second, OverflowFlags &flags) {
  using U = std::make_unsigned_t<S>;
  auto x = U (first), y = U (second);
  auto a = std::bit_cast<S> (x), b = std::bit_cast<S> (y);
  constexpr auto lo = std::numeric_limits<S>::min (), hi = std::numeric_limits<S>::max ();
  if (op == O::Addo) {
    flags.unsigned_value = x > std::numeric_limits<U>::max () - y;
    flags.signed_value = b >= 0 ? a > hi - b : a < lo - b;
  }
  if (op == O::Subo) {
    flags.unsigned_value = x < y;
    flags.signed_value = b < 0 ? a > hi + b : a < lo + b;
  }
  if (op == O::Mulo)
    flags.signed_value = !a        ? false
                         : a == -1 ? b < -hi
                         : a > 0   ? (b > 0 ? hi / a < b : lo / a > b)
                                   : (b > 0 ? lo / a < b : hi / a > b);
  if (op == O::UMulo) flags.unsigned_value = x && std::numeric_limits<U>::max () / x < y;
}
template <class T>
Result<ScalarValue> floating (O op, std::span<const ScalarValue> values) {
  T a = raw<T> (values[0]), b = values.size () > 1 ? raw<T> (values[1]) : T{};
  switch (op) {
  case O::FMov: return ScalarValue (a);
  case O::FNeg: return ScalarValue (-a);
  case O::FAdd: return ScalarValue (T (a + b));
  case O::FSub: return ScalarValue (T (a - b));
  case O::FMul: return ScalarValue (T (a * b));
  case O::FDiv: return ScalarValue (T (a / b));
  case O::FEq: return ScalarValue (std::int64_t (a == b));
  case O::FNe: return ScalarValue (std::int64_t (a != b));
  case O::FLt: return ScalarValue (std::int64_t (a < b));
  case O::FLe: return ScalarValue (std::int64_t (a <= b));
  case O::FGt: return ScalarValue (std::int64_t (a > b));
  case O::FGe: return ScalarValue (std::int64_t (a >= b));
  default: return invalid ("invalid floating operation");
  }
}
template <class T>
Result<ScalarValue> to_integer (const ScalarValue &v) {
  auto value = raw<T> (v);
  if (!std::isfinite (value) || static_cast<long double> (value) < -9223372036854775808.0L
      || static_cast<long double> (value) >= 9223372036854775808.0L)
    return invalid ("floating conversion outside integer range");
  return ScalarValue (static_cast<std::int64_t> (value));
}
}  // namespace
Result<std::optional<ScalarValue>> evaluate_scalar (O op, std::span<const ScalarValue> v,
                                                    OverflowFlags &flags) {
  auto result = [] (Result<ScalarValue> r) -> Result<std::optional<ScalarValue>> {
    if (!r) return r.error ();
    return std::optional<ScalarValue> (std::move (*r));
  };
  auto value = [] (ScalarValue r) -> Result<std::optional<ScalarValue>> {
    return std::optional<ScalarValue> (std::move (r));
  };
  std::uint64_t a = v.empty () ? 0 : raw<std::uint64_t> (v[0]),
                b = v.size () < 2 ? 0 : raw<std::uint64_t> (v[1]);
  O base = op;
  bool unsigned_multiply = false, narrow_unsigned = false;
  switch (op) {
  case O::Mov: return value (ScalarValue (a));
  case O::Neg: return value (ScalarValue (std::uint64_t (0) - a));
  case O::Negs:
    return value (ScalarValue (
      std::int64_t (std::bit_cast<std::int32_t> (std::uint32_t (0) - std::uint32_t (a)))));
  case O::Ext8:
    return value (ScalarValue (std::int64_t (std::bit_cast<std::int8_t> (std::uint8_t (a)))));
  case O::Ext16:
    return value (ScalarValue (std::int64_t (std::bit_cast<std::int16_t> (std::uint16_t (a)))));
  case O::Ext32:
    return value (ScalarValue (std::int64_t (std::bit_cast<std::int32_t> (std::uint32_t (a)))));
  case O::UExt8: return value (ScalarValue (std::uint64_t (std::uint8_t (a))));
  case O::UExt16: return value (ScalarValue (std::uint64_t (std::uint16_t (a))));
  case O::UExt32: return value (ScalarValue (std::uint64_t (std::uint32_t (a))));
#define MIRNEXT_INTERP_OVERFLOW(name, operation)                                            \
  case O::name:                                                                             \
    overflow<std::int64_t> (O::operation, a, b, flags);                                     \
    base = O::operation == O::Addo ? O::Add : O::operation == O::Subo ? O::Sub : O::Mul;    \
    break;                                                                                  \
  case O::name##s:                                                                          \
    overflow<std::int32_t> (O::operation, a, b, flags);                                     \
    base = O::operation == O::Addo ? O::Adds : O::operation == O::Subo ? O::Subs : O::Muls; \
    break;
    MIRNEXT_INTERP_OVERFLOW (Addo, Addo)
    MIRNEXT_INTERP_OVERFLOW (Subo, Subo)
    MIRNEXT_INTERP_OVERFLOW (Mulo, Mulo)
#undef MIRNEXT_INTERP_OVERFLOW
  case O::UMulo:
    overflow<std::int64_t> (O::UMulo, a, b, flags);
    base = O::Mul;
    unsigned_multiply = true;
    break;
  case O::UMulos:
    overflow<std::int32_t> (O::UMulo, a, b, flags);
    base = O::Muls;
    unsigned_multiply = narrow_unsigned = true;
    break;
#define FLOATING(name, common)                                         \
  case O::F##name: return result (floating<float> (O::F##common, v));  \
  case O::D##name: return result (floating<double> (O::F##common, v)); \
  case O::LD##name: return result (floating<long double> (O::F##common, v));
    FLOATING (Mov, Mov)
    FLOATING (Neg, Neg)
    FLOATING (Add, Add)
    FLOATING (Sub, Sub)
    FLOATING (Mul, Mul)
    FLOATING (Div, Div)
    FLOATING (Eq, Eq)
    FLOATING (Ne, Ne)
    FLOATING (Lt, Lt)
    FLOATING (Le, Le)
    FLOATING (Gt, Gt)
    FLOATING (Ge, Ge)
    FLOATING (Beq, Eq)
    FLOATING (Bne, Ne)
    FLOATING (Blt, Lt)
    FLOATING (Ble, Le)
    FLOATING (Bgt, Gt)
    FLOATING (Bge, Ge)
#undef FLOATING
#define CONVERT(opcode, to, from) \
  case O::opcode: return value (ScalarValue (static_cast<to> (raw<from> (v[0]))));
    CONVERT (I2F, float, std::int64_t)
    CONVERT (I2D, double, std::int64_t)
    CONVERT (I2LD, long double, std::int64_t)
    CONVERT (UI2F, float, std::uint64_t)
    CONVERT (UI2D, double, std::uint64_t)
    CONVERT (UI2LD, long double, std::uint64_t)
    CONVERT (F2D, double, float)
    CONVERT (F2LD, long double, float)
    CONVERT (D2F, float, double)
    CONVERT (D2LD, long double, double)
    CONVERT (LD2F, float, long double)
    CONVERT (LD2D, double, long double)
#undef CONVERT
  case O::F2I: return result (to_integer<float> (v[0]));
  case O::D2I: return result (to_integer<double> (v[0]));
  case O::LD2I: return result (to_integer<long double> (v[0]));
  default: break;
  }
  if (unsigned_multiply)
    return value (ScalarValue (
      narrow_unsigned
        ? std::uint64_t (std::uint32_t (a) * std::uint64_t (std::uint32_t (b))) & UINT32_MAX
        : a * b));
  auto folded
    = transforms::fold_integer (base, a, v.size () > 1 ? std::optional (b) : std::nullopt);
  if (folded) return value (ScalarValue (*folded));
  // Fold failure on these operations is an execution error, never a no-op.
  switch (base) {
  case O::Div:
  case O::Divs:
  case O::UDiv:
  case O::UDivs:
  case O::Mod:
  case O::Mods:
  case O::UMod:
  case O::UMods: return invalid ("integer division by zero or signed division overflow");
  case O::Lsh:
  case O::Lshs:
  case O::Rsh:
  case O::Rshs:
  case O::URsh:
  case O::URshs: return invalid ("shift count outside operand width");
  default: return std::optional<ScalarValue>{};
  }
}
}  // namespace mirnext::execution::detail
