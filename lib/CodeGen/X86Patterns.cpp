#include "mirnext/CodeGen/Access.hpp"
/* Rules derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov.
   Licensed under the MIT License. Symbol addresses remain unresolved. */
#include "mirnext/CodeGen/Detail/X86Patterns.hpp"
#include "mirnext/CodeGen/Detail/X86Encoding.hpp"
#include <array>
#include <bit>
#include <climits>
#include <limits>

namespace mirnext::codegen {
namespace {
using ir::Opcode;
using K = ir::Type::Kind;
enum class PatternCode {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) name,
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
  AlignedMove,
  UnalignedMove,
  Count
};
struct Pattern {
  PatternCode code;
  std::string_view constraints, encoding;
};
#define MIRNEXT_X86_OPCODE(name) PatternCode::name
#define MIRNEXT_X86_PRIVATE(name) PatternCode::name
#define MIRNEXT_X86_TABLE static constexpr Pattern patterns[]
#include "mirnext/Target/X86Patterns.def"
#undef MIRNEXT_X86_TABLE
#undef MIRNEXT_X86_PRIVATE
#undef MIRNEXT_X86_OPCODE

enum class ConstraintKind {
  Invalid,
  Any,
  Register,
  LowRegister,
  Physical,
  Zero,
  Immediate,
  Scale,
  Constant,
  Memory,
  LongLabel,
  ShortLabel,
  Same
};
struct Constraint {
  ConstraintKind kind = ConstraintKind::Invalid;
  unsigned parameter = 0;
};
constexpr unsigned type_bit (K k) {
  auto n = static_cast<unsigned> (k);
  return n < std::numeric_limits<unsigned>::digits ? 1u << n : 0;
}
constexpr Constraint decode (std::string_view token) {
  using C = ConstraintKind;
  if (token == "X") return {C::Any};
  if (token == "r") return {C::Register};
  if (token == "t") return {C::LowRegister};
  if (token == "z") return {C::Zero};
  if (token == "s") return {C::Scale};
  if (token == "L") return {C::LongLabel};
  if (token == "l") return {C::ShortLabel};
  if (token.size () == 1 && token.front () >= '0' && token.front () <= '9')
    return {C::Same, unsigned (token.front () - '0')};
  if (token.size () == 2 && token.front () == 'i' && token[1] >= '0' && token[1] <= '3')
    return {C::Immediate, 8u << (token[1] - '0')};
  if (token.front () == 'h' || token.front () == 'c') {
    unsigned number = 0;
    if (token.size () == 1) return {};
    for (char c : token.substr (1)) {
      if (c < '0' || c > '9' || number > (UINT_MAX - unsigned (c - '0')) / 10) return {};
      number = number * 10 + unsigned (c - '0');
    }
    if (token.front () == 'h' && number > 33) return {};
    return {token.front () == 'h' ? C::Physical : C::Constant, number};
  }
  if (token.front () != 'm') return {};
  auto tag = token.substr (1);
  if (tag == "f") return {C::Memory, type_bit (K::F32)};
  if (tag == "d") return {C::Memory, type_bit (K::F64)};
  if (tag == "ld") return {C::Memory, type_bit (K::LongDouble)};
  bool signed_ok = true, unsigned_ok = true;
  if (!tag.empty () && (tag.front () == 'u' || tag.front () == 's')) {
    signed_ok = tag.front () != 'u';
    unsigned_ok = tag.front () != 's';
    tag.remove_prefix (1);
  }
  if (tag.size () != 1 || tag.front () < '0' || tag.front () > '3') return {};
  auto n = unsigned (tag.front () - '0');
  constexpr std::array<K, 4> signed_types{K::I8, K::I16, K::I32, K::I64};
  constexpr std::array<K, 4> unsigned_types{K::U8, K::U16, K::U32, K::U64};
  return {C::Memory, (signed_ok ? type_bit (signed_types[n]) : 0)
                       | (unsigned_ok ? type_bit (unsigned_types[n]) : 0)
                       | (n == 3 ? type_bit (K::Pointer) : 0)};
}
struct CompiledPattern {
  std::array<Constraint, 4> operands{};
  unsigned count = 0;
  bool tail = false;
};
constexpr auto pattern_count = std::size (patterns);
constexpr auto code_count = static_cast<unsigned> (PatternCode::Count);
static_assert (pattern_count < UINT16_MAX);
struct PatternTable {
  std::array<CompiledPattern, pattern_count> compiled{};
  std::array<std::uint16_t, pattern_count> indexes{};
  std::array<std::uint16_t, code_count + 1> offsets{};
  bool valid = true;
};
consteval PatternTable compile_patterns () {
  PatternTable table;
  for (auto &p : patterns) ++table.offsets[static_cast<unsigned> (p.code) + 1];
  for (unsigned n = 1; n <= code_count; ++n) table.offsets[n] += table.offsets[n - 1];
  auto next = table.offsets;
  for (unsigned row = 0; row < pattern_count; ++row) {
    auto &p = patterns[row];
    table.indexes[next[static_cast<unsigned> (p.code)]++] = row;
    auto &out = table.compiled[row];
    auto source = p.constraints;
    while (!source.empty ()) {
      auto end = source.find_first_of (" \t");
      auto token = source.substr (0, end);
      if (token == "$") {
        out.tail = true;
        if (end != source.npos) table.valid = false;
        break;
      }
      if (out.count == out.operands.size ()) {
        table.valid = false;
        break;
      }
      auto c = decode (token);
      if (c.kind == ConstraintKind::Invalid
          || (c.kind == ConstraintKind::Same && c.parameter >= out.count))
        table.valid = false;
      out.operands[out.count++] = c;
      if (end == source.npos) break;
      auto start = source.find_first_not_of (" \t", end);
      if (start == source.npos) break;
      source.remove_prefix (start);
    }
  }
  return table;
}
constexpr auto table = compile_patterns ();
static_assert (table.valid, "unsupported x86 instruction constraint");
constexpr auto descriptions = [] {
  std::array<X86InstructionPattern, pattern_count> out{};
  for (std::size_t n = 0; n < pattern_count; ++n) {
    auto &p = patterns[n];
    auto code = p.code == PatternCode::AlignedMove ? MachineOpcode (MachinePseudoOpcode::X86Mov128)
                : p.code == PatternCode::UnalignedMove
                  ? MachineOpcode (MachinePseudoOpcode::X86Mov128Unaligned)
                  : MachineOpcode (static_cast<Opcode> (p.code));
    out[n] = {n, p.constraints, p.encoding, code};
  }
  return out;
}();
std::optional<std::uint64_t> integer (const MachineOperand &o) {
  if (auto p = detail::operand_access (o).get<std::int64_t> ())
    return std::bit_cast<std::uint64_t> (*p);
  if (auto p = detail::operand_access (o).get<std::uint64_t> ()) return *p;
  return {};
}
bool is_register (const MachineOperand &o) {
  if (auto reg = detail::operand_access (o).get<const MachineVirtualRegister *> ())
    return *reg != nullptr;
  auto physical = detail::operand_access (o).get<target::PhysicalRegister> ();
  return physical && physical->architecture () == target::Architecture::X86_64;
}
bool short_branch (X86PatternOptions options) {
  if (!options.allow_short_branches) return false;
  if (!options.branch_range) return true;  // Legality, without an encoding layout.
  auto [distance, maximum] = *options.branch_range;
  // Apply the conservative backward adjustment without overflowing
  // on malformed distances or maximum instruction sizes.
  return distance < 0 ? maximum <= 123 && distance >= -124 + static_cast<std::int64_t> (maximum)
                      : distance <= 129;
}
bool match (const CompiledPattern &p, std::span<const MachineOperand> ops,
            X86PatternOptions options) {
  if (ops.size () < p.count || (!p.tail && ops.size () != p.count)) return false;
  using C = ConstraintKind;
  for (unsigned n = 0; n < p.count; ++n) {
    auto c = p.operands[n];
    auto &o = ops[n];
    switch (c.kind) {
    case C::Any: break;
    case C::Register:
      if (!is_register (o)) return false;
      break;
    case C::LowRegister:
    case C::Physical: {
      auto reg = detail::operand_access (o).get<target::PhysicalRegister> ();
      if (!reg || reg->architecture () != target::Architecture::X86_64
          || (c.kind == C::LowRegister ? reg->encoding () > 3 : reg->encoding () != c.parameter))
        return false;
      break;
    }
    case C::Zero:
    case C::Scale:
    case C::Constant: {
      auto value = integer (o);
      if (!value) return false;
      if (c.kind == C::Scale ? (*value != 1 && *value != 2 && *value != 4 && *value != 8)
                             : *value != (c.kind == C::Zero ? 0 : c.parameter))
        return false;
      break;
    }
    case C::Immediate: {
      auto value = integer (o);
      if (!value) {
        if (c.parameter != 64
            || (!detail::operand_access (o).get<MachineSymbolReference> ()
                && !detail::operand_access (o).get<BuiltinReference> ()))
          return false;
      } else if (c.parameter < 64) {
        const auto signed_value = std::bit_cast<std::int64_t> (*value);
        auto bound = std::int64_t (1) << (c.parameter - 1);
        if (signed_value < -bound || signed_value >= bound) return false;
      }
      break;
    }
    case C::Memory: {
      // x87 values remain pseudos until spill rewriting assigns their storage.
      if (auto reg = detail::operand_access (o).get<const MachineVirtualRegister *> ();
          reg && *reg && c.parameter == type_bit (K::LongDouble)
          && (*reg)->type ().kind () == K::LongDouble)
        break;
      auto m = detail::operand_access (o).get<MachineMemory> ();
      if (!m || !(c.parameter & type_bit (m->type.kind ())) || m->displacement < INT32_MIN
          || m->displacement > INT32_MAX
          || (m->index && m->scale != 1 && m->scale != 2 && m->scale != 4 && m->scale != 8))
        return false;
      break;
    }
    case C::ShortLabel:
      if (!detail::operand_access (o).get<const MachineLabel *> () || !short_branch (options))
        return false;
      break;
    case C::LongLabel:
      if (!detail::operand_access (o).get<const MachineLabel *> ()
          && !detail::operand_access (o).get<MachineSymbolReference> ()
          && !detail::operand_access (o).get<BuiltinReference> ())
        return false;
      break;
    case C::Same:
      if (!x86_operands_equal (o, ops[c.parameter])) return false;
      break;
    case C::Invalid: return false;
    }
  }
  return true;
}
}  // namespace
bool x86_operands_equal (const MachineOperand &a, const MachineOperand &b) {
  if (auto value = integer (a)) {
    auto other = integer (b);
    return other && *value == *other;
  }
  if (auto memory = detail::operand_access (a).get<MachineMemory> ()) {
    auto other = detail::operand_access (b).get<MachineMemory> ();
    return other && memory->type == other->type && memory->base == other->base
           && memory->index == other->index && memory->displacement == other->displacement
           && (!memory->index || memory->scale == other->scale);
  }
  return a == b;
}
std::optional<X86InstructionPattern> x86_instruction_pattern (
  MachineOpcode code, std::span<const MachineOperand> operands, X86PatternOptions options) {
  unsigned wanted;
  if (auto op = std::get_if<Opcode> (&code)) {
    wanted = static_cast<unsigned> (*op);
    if (wanted >= static_cast<unsigned> (PatternCode::AlignedMove)) return {};
  } else if (std::get<MachinePseudoOpcode> (code) == MachinePseudoOpcode::X86Mov128)
    wanted = static_cast<unsigned> (PatternCode::AlignedMove);
  else if (std::get<MachinePseudoOpcode> (code) == MachinePseudoOpcode::X86Mov128Unaligned)
    wanted = static_cast<unsigned> (PatternCode::UnalignedMove);
  else
    return {};
  if (wanted >= code_count) return {};
  for (unsigned n = table.offsets[wanted]; n < table.offsets[wanted + 1]; ++n) {
    auto row = table.indexes[n];
    auto candidate = options;
    if (candidate.encoding_layout && candidate.branch_range)
      candidate.branch_range->maximum_instruction_size = x86_encoding_recipe (row)->maximum_size;
    if (match (table.compiled[row], operands, candidate)) return descriptions[row];
  }
  return {};
}
std::span<const X86InstructionPattern> x86_instruction_patterns () { return descriptions; }
bool x86_instruction_ok (MachineOpcode code, std::span<const MachineOperand> operands) {
  return x86_instruction_pattern (code, operands).has_value ();
}
bool x86_inout_opcode (Opcode code) {
  switch (code) {
  case Opcode::Add:
  case Opcode::Adds:
  case Opcode::FAdd:
  case Opcode::DAdd:
  case Opcode::LDAdd:
  case Opcode::Sub:
  case Opcode::Subs:
  case Opcode::FSub:
  case Opcode::DSub:
  case Opcode::LDSub:
  case Opcode::Mul:
  case Opcode::Muls:
  case Opcode::FMul:
  case Opcode::DMul:
  case Opcode::LDMul:
  case Opcode::FDiv:
  case Opcode::DDiv:
  case Opcode::LDDiv:
  case Opcode::And:
  case Opcode::Ands:
  case Opcode::Or:
  case Opcode::Ors:
  case Opcode::Xor:
  case Opcode::Xors:
  case Opcode::Lsh:
  case Opcode::Lshs:
  case Opcode::Rsh:
  case Opcode::Rshs:
  case Opcode::URsh:
  case Opcode::URshs:
  case Opcode::Neg:
  case Opcode::Negs:
  case Opcode::FNeg:
  case Opcode::DNeg:
  case Opcode::LDNeg:
  case Opcode::Addo:
  case Opcode::Addos:
  case Opcode::Subo:
  case Opcode::Subos:
  case Opcode::Mulo:
  case Opcode::Mulos:
  case Opcode::UMulo:
  case Opcode::UMulos: return true;
  default: return false;
  }
}
}  // namespace mirnext::codegen
