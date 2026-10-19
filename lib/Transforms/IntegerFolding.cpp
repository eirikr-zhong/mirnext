/* Licensed under the MIT License. Derived from MIR GVN integer macros. */
#include "mirnext/Transforms/IntegerFolding.hpp"
#include <bit>
#include <limits>
namespace mirnext::transforms {
std::optional<std::uint64_t> fold_integer (ir::Opcode op, std::optional<std::uint64_t> a,
                                           std::optional<std::uint64_t> b) {
  using ir::Opcode;
  if (!a) return {};
  auto signed64 = [] (std::uint64_t x) { return std::bit_cast<std::int64_t> (x); };
  auto signed32
    = [] (std::uint64_t x) { return std::bit_cast<std::int32_t> (static_cast<std::uint32_t> (x)); };
  auto result32 = [&] (std::uint64_t x) {
    return static_cast<std::uint64_t> (static_cast<std::int64_t> (signed32 (x)));
  };
  switch (op) {
  case Opcode::Ext8:
    return static_cast<std::uint64_t> (
      static_cast<std::int64_t> (std::bit_cast<std::int8_t> (static_cast<std::uint8_t> (*a))));
  case Opcode::Ext16:
    return static_cast<std::uint64_t> (
      static_cast<std::int64_t> (std::bit_cast<std::int16_t> (static_cast<std::uint16_t> (*a))));
  case Opcode::Ext32: return result32 (*a);
  case Opcode::UExt8: return static_cast<std::uint8_t> (*a);
  case Opcode::UExt16: return static_cast<std::uint16_t> (*a);
  case Opcode::UExt32: return static_cast<std::uint32_t> (*a);
  case Opcode::Neg: return 0 - *a;
  case Opcode::Negs: return result32 (0 - *a);
  case Opcode::Bt: return *a;
  case Opcode::Bts: return result32 (*a);
  case Opcode::Bf: return !*a;
  case Opcode::Bfs: return !static_cast<std::uint32_t> (*a);
  default: break;
  }
  if (!b) return {};
  switch (op) {
  case Opcode::Add:
  case Opcode::Addo: return *a + *b;
  case Opcode::Adds:
  case Opcode::Addos: return result32 (*a + *b);
  case Opcode::Sub:
  case Opcode::Subo: return *a - *b;
  case Opcode::Subs:
  case Opcode::Subos: return result32 (*a - *b);
  case Opcode::Mul:
  case Opcode::Mulo:
  case Opcode::UMulo: return *a * *b;
  case Opcode::Muls:
  case Opcode::Mulos: return result32 (*a * *b);
  case Opcode::UMulos: return static_cast<std::uint32_t> (*a * *b);
  case Opcode::And: return *a & *b;
  case Opcode::Ands: return result32 (*a & *b);
  case Opcode::Or: return *a | *b;
  case Opcode::Ors: return result32 (*a | *b);
  case Opcode::Xor: return *a ^ *b;
  case Opcode::Xors: return result32 (*a ^ *b);
  case Opcode::Div:
  case Opcode::Mod: {
    auto x = signed64 (*a), y = signed64 (*b);
    if (!y || (x == std::numeric_limits<std::int64_t>::min () && y == -1)) return {};
    return static_cast<std::uint64_t> (op == Opcode::Div ? x / y : x % y);
  }
  case Opcode::Divs:
  case Opcode::Mods: {
    auto x = signed32 (*a), y = signed32 (*b);
    if (!y || (x == std::numeric_limits<std::int32_t>::min () && y == -1)) return {};
    return static_cast<std::uint64_t> (
      static_cast<std::int64_t> (op == Opcode::Divs ? x / y : x % y));
  }
  case Opcode::UDiv:
  case Opcode::UMod:
    if (!*b) return {};
    return op == Opcode::UDiv ? *a / *b : *a % *b;
  case Opcode::UDivs:
  case Opcode::UMods: {
    auto x = static_cast<std::uint32_t> (*a), y = static_cast<std::uint32_t> (*b);
    if (!y) return {};
    return op == Opcode::UDivs ? x / y : x % y;
  }
  case Opcode::Lsh:
    if (*b >= 64) return {};
    return *a << *b;
  case Opcode::Lshs:
    if (static_cast<std::uint32_t> (*b) >= 32) return {};
    return result32 (static_cast<std::uint32_t> (*a) << static_cast<std::uint32_t> (*b));
  case Opcode::Rsh:
    if (*b >= 64) return {};
    return static_cast<std::uint64_t> (signed64 (*a) >> *b);
  case Opcode::Rshs:
    if (static_cast<std::uint32_t> (*b) >= 32) return {};
    return static_cast<std::uint64_t> (
      static_cast<std::int64_t> (signed32 (*a) >> static_cast<std::uint32_t> (*b)));
  case Opcode::URsh:
    if (*b >= 64) return {};
    return *a >> *b;
  case Opcode::URshs:
    if (static_cast<std::uint32_t> (*b) >= 32) return {};
    return static_cast<std::uint32_t> (*a) >> static_cast<std::uint32_t> (*b);
#define CMP(normal, narrow, branch, narrow_branch, comparison)        \
  case Opcode::normal:                                                \
  case Opcode::branch: return signed64 (*a) comparison signed64 (*b); \
  case Opcode::narrow:                                                \
  case Opcode::narrow_branch: return signed32 (*a) comparison signed32 (*b);
    CMP (Eq, Eqs, Beq, Beqs, ==)
    CMP (Ne, Nes, Bne, Bnes, !=)
    CMP (Lt, Lts, Blt, Blts, <)
    CMP (Le, Les, Ble, Bles, <=)
    CMP (Gt, Gts, Bgt, Bgts, >)
    CMP (Ge, Ges, Bge, Bges, >=)
#undef CMP
#define CMP(normal, narrow, branch, narrow_branch, comparison) \
  case Opcode::normal:                                         \
  case Opcode::branch: return *a comparison * b;               \
  case Opcode::narrow:                                         \
  case Opcode::narrow_branch:                                  \
    return static_cast<std::uint32_t> (*a) comparison static_cast<std::uint32_t> (*b);
    CMP (ULt, ULts, UBlt, UBlts, <)
    CMP (ULe, ULes, UBle, UBles, <=)
    CMP (UGt, UGts, UBgt, UBgts, >)
    CMP (UGe, UGes, UBge, UBges, >=)
#undef CMP
  default: return {};
  }
}
}  // namespace mirnext::transforms
