/* Licensed under the MIT License. Shared SIMD128 type rules. */
#pragma once
#include "mirnext/IR/IR.hpp"
namespace mirnext::ir::detail {
enum class VectorRule {
  Move,
  Arithmetic,
  Bits,
  Compare,
  Splat,
  Extract,
  Insert,
  Shuffle,
  Multiply,
  FloatBinary,
  Negate,
  Absolute,
  Shift,
  MinMax,
  Saturate,
  Average,
  Popcount,
  Bitcast,
  Bitselect,
  AnyTrue,
  AllTrue,
  Bitmask,
  Swizzle,
  Extend,
  Narrow,
  Extmul,
  Pairwise,
  Dot,
  Q15,
  FloatUnary,
  Convert,
  ConvertLow,
  ConvertZero,
  TruncSat,
  TruncSatZero
};
constexpr std::optional<VectorRule> vector_rule (Opcode op) {
  switch (op) {
#define MIRNEXT_VECTOR_OPCODE(name, wire, spelling, roles, rule) \
  case Opcode::name: return VectorRule::rule;
#include "mirnext/IR/VectorOps.def"
#undef MIRNEXT_VECTOR_OPCODE
  default: return {};
  }
}
constexpr bool vector_opcode (Opcode op) { return vector_rule (op).has_value (); }
constexpr bool vector_comparison (Opcode op) { return vector_rule (op) == VectorRule::Compare; }
inline bool vector_signed (Type t) {
  return t.is_vector () && t.vector_lane_type ().is_integer ()
         && static_cast<unsigned> (t.vector_element ()) % 2 == 0;
}
inline Type vector_integer (unsigned bits, bool sign) {
  unsigned element = bits == 8 ? 0 : bits == 16 ? 2 : bits == 32 ? 4 : 6;
  return Type::vector128 (static_cast<Type::VectorElement> (element + !sign), 128 / bits);
}
inline Type vector_widened (Type t) {
  return t.valid () && t.is_vector () && t.vector_lane_type ().is_integer ()
             && t.vector_lanes () >= 4
           ? vector_integer (256 / t.vector_lanes (), vector_signed (t))
           : Type (Type::Kind::Undef);
}
inline bool vector_types (Opcode op, Type destination, std::span<const Type> inputs) {
  auto rule = vector_rule (op);
  if (!rule || inputs.empty () || !destination.valid ()) return false;
  auto a = inputs[0];
  if (*rule == VectorRule::Splat)
    return destination.is_vector () && a.is_scalar () && a.kind () != Type::Kind::Pointer
           && (destination.vector_lane_type ().is_integer ()
                 ? a.is_integer ()
                 : a == destination.vector_lane_type ());
  if (!a.valid () || !a.is_vector ()) return false;
  auto lane = a.vector_lane_type ();
  auto bits = 128 / a.vector_lanes ();
  bool integer = lane.is_integer (), signed_input = vector_signed (a);
  if (*rule == VectorRule::Extract)
    return lane.is_integer ()
             ? destination.is_integer () && destination.kind () != Type::Kind::Pointer
             : destination == lane;
  if (*rule == VectorRule::AnyTrue || *rule == VectorRule::AllTrue || *rule == VectorRule::Bitmask)
    return destination.is_integer () && destination.kind () != Type::Kind::Pointer
           && (*rule == VectorRule::AnyTrue || integer);
  if (!destination.is_vector ()) return false;
  if (*rule == VectorRule::Bitcast) return true;
  if (*rule == VectorRule::Bitselect)
    return inputs.size () == 3 && inputs[1] == destination && inputs[2] == destination;
  if (*rule == VectorRule::Extend || *rule == VectorRule::Extmul || *rule == VectorRule::Pairwise)
    return integer && destination == vector_widened (a)
           && (*rule != VectorRule::Pairwise || bits <= 16)
           && (*rule != VectorRule::Extmul || (inputs.size () == 2 && inputs[1] == a));
  if (*rule == VectorRule::Narrow)
    return inputs.size () == 2 && inputs[1] == a && integer && signed_input
           && (bits == 16 || bits == 32) && destination.vector_lane_type ().is_integer ()
           && destination.vector_lanes () == a.vector_lanes () * 2;
  if (*rule == VectorRule::Dot)
    return a == vector_integer (16, true) && inputs.size () == 2 && inputs[1] == a
           && destination == vector_integer (32, true);
  auto f32 = Type::vector128 (Type::VectorElement::F32, 4);
  auto f64 = Type::vector128 (Type::VectorElement::F64, 2);
  if (*rule == VectorRule::Convert) return integer && bits == 32 && destination == f32;
  if (*rule == VectorRule::ConvertLow)
    return ((integer && bits == 32) || a == f32) && destination == f64;
  if (*rule == VectorRule::ConvertZero) return a == f64 && destination == f32;
  if (*rule == VectorRule::TruncSat || *rule == VectorRule::TruncSatZero)
    return a == (*rule == VectorRule::TruncSat ? f32 : f64)
           && destination.vector_lane_type ().is_integer () && destination.vector_lanes () == 4;
  if (destination != (vector_comparison (op) ? a.vector_mask_type () : a)) return false;
  if (*rule == VectorRule::Insert)
    return inputs.size () == 3
           && (integer ? inputs[1].is_integer () && inputs[1].kind () != Type::Kind::Pointer
                       : inputs[1] == lane);
  if (*rule == VectorRule::Shift)
    return inputs.size () == 2 && integer && inputs[1].is_integer ()
           && inputs[1].kind () != Type::Kind::Pointer;
  if (*rule == VectorRule::Shuffle) return inputs.size () == 3 && inputs[1] == a;
  if (*rule == VectorRule::Swizzle)
    return bits == 8 && integer && inputs.size () == 2 && inputs[1] == vector_integer (8, false);
  if (inputs.size () == 2 && inputs[1] != a) return false;
  switch (*rule) {
  case VectorRule::Multiply: return !integer || bits != 8;
  case VectorRule::FloatBinary:
  case VectorRule::FloatUnary: return !integer;
  case VectorRule::Absolute: return !integer || signed_input;
  case VectorRule::MinMax: return !integer || bits <= 32;
  case VectorRule::Saturate: return integer && bits <= 16;
  case VectorRule::Average: return integer && !signed_input && bits <= 16;
  case VectorRule::Popcount: return integer && bits == 8;
  case VectorRule::Q15: return a == vector_integer (16, true);
  default: return true;
  }
}
}  // namespace mirnext::ir::detail
