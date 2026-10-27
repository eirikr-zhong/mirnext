/* Rules derived from MIR, Copyright (C) 2020-2024 Vladimir Makarov.
   Licensed under the MIT License. No runtime addresses are folded here. */
#include "mirnext/CodeGen/Lowering.hpp"
#include "mirnext/CodeGen/Detail/AArch64Patterns.hpp"
#include <array>
#include <limits>

namespace mirnext::codegen {
namespace {
using ir::Opcode;
using K = ir::Type::Kind;
enum class PatternCode {
#define MIRNEXT_OPCODE(name, wire, spelling, roles, count, flow) name,
#include "mirnext/IR/Opcodes.def"
#undef MIRNEXT_OPCODE
  SubUnsignedOverflow,
  SubUnsignedNoOverflow,
  MultiplyOverflow,
  MultiplyNoOverflow,
  Count
};
struct pattern {
  PatternCode code;
  const char *pattern, *replacement;
};
#define MIRNEXT_A64_OPCODE(name) PatternCode::name
#define MIRNEXT_A64_TABLE static constexpr struct pattern patterns[]
#include "mirnext/Target/AArch64Patterns.def"
#undef MIRNEXT_A64_TABLE
#undef MIRNEXT_A64_OPCODE
enum class ConstraintKind {
  Invalid,
  Any,
  Register,
  Physical,
  IndexedMemory,
  DisplacedMemory,
  WideImmediate,
  InvertedImmediate,
  AddImmediate,
  AlignedAddImmediate,
  Shift,
  Label,
  LabelOrSymbol,
  FloatZero,
  DoubleZero
};
struct Constraint {
  ConstraintKind kind = ConstraintKind::Invalid;
  unsigned parameter = 0, type_mask = 0;
};
constexpr unsigned type_bit (K type) { return 1u << static_cast<unsigned> (type); }
constexpr Constraint decode (std::string_view token) {
  using C = ConstraintKind;
  if (token == "X") return {C::Any};
  if (token == "r") return {C::Register};
  if (token.front () == 'h') {
    unsigned n = 0;
    for (char c : token.substr (1)) n = n * 10 + unsigned (c - '0');
    return {C::Physical, n};
  }
  if (token.front () == 'm' || token.front () == 'M') {
    auto tag = token.substr (1);
    unsigned mask, scale;
    if (tag == "f") {
      mask = type_bit (K::F32);
      scale = 4;
    } else if (tag == "d") {
      mask = type_bit (K::F64);
      scale = 8;
    } else if (tag == "ld") {
      mask = type_bit (K::LongDouble);
      scale = 16;
    } else {
      bool signed_ok = tag.front () != 'u', unsigned_ok = tag.front () != 's';
      if (!signed_ok || !unsigned_ok) tag.remove_prefix (1);
      unsigned n = unsigned (tag.front () - '0');
      if (n > 3) return {};
      constexpr std::array<K, 4> signed_types{K::I8, K::I16, K::I32, K::I64};
      constexpr std::array<K, 4> unsigned_types{K::U8, K::U16, K::U32, K::U64};
      mask = (signed_ok ? type_bit (signed_types[n]) : 0)
             | (unsigned_ok ? type_bit (unsigned_types[n]) : 0);
      if (n == 3) mask |= type_bit (K::Pointer);
      scale = 1u << n;
    }
    return {token.front () == 'm' ? C::IndexedMemory : C::DisplacedMemory, scale, mask};
  }
  if (token == "Zf") return {C::FloatZero};
  if (token == "Zd") return {C::DoubleZero};
  if (token.front () == 'Z' || token.front () == 'N')
    return {token.front () == 'Z' ? C::WideImmediate : C::InvertedImmediate,
            unsigned (token[1] - '0') + 1};
  if (token == "I" || token == "Iu")
    return {token == "I" ? C::AddImmediate : C::AlignedAddImmediate};
  if (token.front () == 'S') return {C::Shift, token[1] == 'r' || token[1] == 'l' ? 32u : 64u};
  if (token == "l") return {C::Label};
  if (token == "L") return {C::LabelOrSymbol};
  return {};
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
  for (unsigned n = 0; n < pattern_count; ++n) {
    table.indexes[next[static_cast<unsigned> (patterns[n].code)]++] = n;
    auto &out = table.compiled[n];
    std::string_view spec (patterns[n].pattern);
    while (!spec.empty ()) {
      auto end = spec.find_first_of (" \t");
      auto token = spec.substr (0, end);
      if (token == "$") {
        out.tail = true;
        break;
      }
      if (out.count == out.operands.size ()) {
        table.valid = false;
        break;
      }
      auto c = decode (token);
      if (c.kind == ConstraintKind::Invalid) table.valid = false;
      out.operands[out.count++] = c;
      if (end == spec.npos) break;
      auto start = spec.find_first_not_of (" \t", end);
      if (start == spec.npos) break;
      spec.remove_prefix (start);
    }
  }
  return table;
}
constexpr auto table = compile_patterns ();
static_assert (table.valid, "unsupported AArch64 instruction constraint");
constexpr std::string_view trim (std::string_view s) {
  auto first = s.find_first_not_of (" \t");
  if (first == s.npos) return {};
  return s.substr (first, s.find_last_not_of (" \t") - first + 1);
}
constexpr unsigned hex (char c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : 16;
}
constexpr std::uint32_t number (std::string_view s, bool &valid) {
  std::uint32_t value = 0;
  if (s.empty ()) valid = false;
  for (auto c : s) {
    if (hex (c) == 16 || value > UINT32_MAX / 16) valid = false;
    value = value * 16 + hex (c);
  }
  return value;
}
struct RecipeSizes {
  std::size_t words = 0, fields = 0;
};
constexpr auto recipe_sizes = [] {
  RecipeSizes out;
  for (auto &p : patterns) {
    std::string_view source (p.replacement);
    for (;;) {
      auto end = source.find (';');
      auto word = trim (source.substr (0, end));
      ++out.words;
      if (!word.empty ()) {
        auto rest = word.find_first_of (" \t");
        if (rest != word.npos) {
          auto fields = trim (word.substr (rest));
          while (!fields.empty ()) {
            ++out.fields;
            auto space = fields.find_first_of (" \t");
            if (space == fields.npos) break;
            fields = trim (fields.substr (space));
          }
        }
      }
      if (end == source.npos) break;
      source.remove_prefix (end + 1);
    }
  }
  return out;
}();
struct EncodingTable {
  std::array<AArch64Word, recipe_sizes.words> words{};
  std::array<AArch64Field, recipe_sizes.fields> fields{};
  std::array<std::size_t, pattern_count + 1> offsets{};
  bool valid = true;
};
consteval EncodingTable compile_encoding () {
  EncodingTable out;
  std::size_t w = 0, f = 0, row = 0;
  for (auto &p : patterns) {
    out.offsets[row++] = w;
    std::string_view source (p.replacement);
    for (;;) {
      auto end = source.find (';');
      auto text = trim (source.substr (0, end));
      auto &word = out.words[w++];
      word.first_field = f;
      if (!text.empty ()) {
        auto space = text.find_first_of (" \t");
        auto header = text.substr (0, space);
        auto colon = header.find (':');
        word.opcode = number (header.substr (0, colon), out.valid);
        if (colon != header.npos) word.mask = number (header.substr (colon + 1), out.valid);
        if (word.opcode & ~word.mask) out.valid = false;
        auto fields = space == text.npos ? std::string_view{} : trim (text.substr (space));
        while (!fields.empty ()) {
          auto limit = fields.find_first_of (" \t");
          auto token = fields.substr (0, limit);
          auto &field = out.fields[f++];
          using K = AArch64FieldKind;
          if ((token.front () == 'r' || token.front () == 'v' || token.front () == 'h')
              && token.size () >= 3) {
            field.kind = K::Register;
            field.flags = token.front () == 'h' ? 1 : token.front () == 'v' ? 2 : 0;
            field.shift = token[1] == 'd'   ? 0
                          : token[1] == 'n' ? 5
                          : token[1] == 'm' ? 16
                          : token[1] == 'a' ? 10
                                            : 255;
            field.parameter = number (token.substr (2), out.valid);
            if (field.shift == 255 || field.parameter > (field.flags == 1 ? 31 : 2))
              out.valid = false;
          } else if (token == "m" || token == "M")
            field.kind = token == "m" ? K::IndexedMemory : K::DisplacedMemory;
          else if (token == "S")
            field.kind = K::RightShift;
          else if (token == "SL" || token == "Sl") {
            field.kind = K::LeftShift;
            field.parameter = token == "Sl" ? 32 : 64;
          } else if (token.front () == 'Z' || token.front () == 'N') {
            field.kind = K::WideImmediate;
            field.flags = token.front () == 'N';
            field.parameter = number (token.substr (1), out.valid);
            if (field.parameter > 3) out.valid = false;
          } else if (token == "I" || token == "Iu") {
            field.kind = K::ArithmeticImmediate;
            field.flags = token == "Iu";
          } else if (token.front () == 'I' || token.front () == 'i') {
            field.kind = token.front () == 'I' ? K::ImmediateR : K::ImmediateS;
            field.parameter = number (token.substr (1), out.valid);
            if (field.parameter > 63) out.valid = false;
          } else if (token == "l" || token == "L")
            field.kind = token == "l" ? K::ShortLabel : K::Label;
          else if (token == "T")
            field.kind = K::SwitchTable;
          else
            out.valid = false;
          if (limit == fields.npos) break;
          fields = trim (fields.substr (limit));
        }
      }
      word.field_count = f - word.first_field;
      if (end == source.npos) break;
      source.remove_prefix (end + 1);
    }
  }
  out.offsets[row] = w;
  if (w != out.words.size () || f != out.fields.size ()) out.valid = false;
  return out;
}
constexpr auto encoding = compile_encoding ();
static_assert (encoding.valid, "unsupported AArch64 encoding recipe");
bool is_register (const MachineOperand &o) {
  return o.get<const MachineVirtualRegister *> () || o.get<target::PhysicalRegister> ();
}
std::optional<std::uint64_t> integer (const MachineOperand &o) {
  if (auto v = o.get<std::uint64_t> ()) return *v;
  if (auto v = o.get<std::int64_t> ()) return static_cast<std::uint64_t> (*v);
  return {};
}
bool add_immediate (std::uint64_t v) { return v < 4096 || (!(v & 4095) && (v >> 12) < 4096); }
bool token_match (Constraint token, const MachineOperand &o) {
  using C = ConstraintKind;
  switch (token.kind) {
  case C::Any: return true;
  case C::Register: return is_register (o);
  case C::Physical: {
    auto p = o.get<target::PhysicalRegister> ();
    return p && p->encoding () == token.parameter;
  }
  case C::IndexedMemory:
  case C::DisplacedMemory: {
    auto memory = o.get<MachineMemory> ();
    if (!memory) return false;
    if (!(token.type_mask & type_bit (memory->type.kind ()))) return false;
    auto scale = token.parameter;
    if (token.kind == C::IndexedMemory)
      return memory->displacement == 0
             && (!memory->index || memory->scale == 1 || memory->scale == scale);
    return !memory->index && memory->displacement >= 0 && memory->displacement % scale == 0
           && memory->displacement / scale < 4096;
  }
  case C::FloatZero: {
    auto v = o.get<float> ();
    return v && *v == 0.0f;
  }
  case C::DoubleZero: {
    auto v = o.get<double> ();
    return v && *v == 0.0;
  }
  case C::WideImmediate:
  case C::InvertedImmediate: {
    auto v = integer (o);
    // An unresolved relocation must reserve the complete four-chunk move.
    if (!v)
      return token.kind == C::WideImmediate && token.parameter == 4
             && (o.get<MachineSymbolReference> () || o.get<BuiltinReference> ());
    auto bits = token.kind == C::InvertedImmediate ? ~*v : *v;
    unsigned chunks = 0;
    for (unsigned n = 0; n < 4; ++n) chunks += ((bits >> (16 * n)) & 65535) != 0;
    return chunks <= token.parameter;
  }
  case C::AddImmediate:
  case C::AlignedAddImmediate: {
    auto v = integer (o);
    return v && add_immediate (token.kind == C::AlignedAddImmediate ? (*v + 15) / 16 * 16 : *v);
  }
  case C::Shift: {
    auto v = integer (o);
    return v && *v < token.parameter;
  }
  case C::Label: return o.get<const MachineLabel *> ();
  case C::LabelOrSymbol:
    return o.get<const MachineLabel *> () || o.get<MachineSymbolReference> ()
           || o.get<BuiltinReference> ();
  case C::Invalid: break;
  }
  return false;
}
bool match (const CompiledPattern &p, std::span<const MachineOperand> operands) {
  if (p.count > operands.size () || (!p.tail && p.count != operands.size ())) return false;
  for (unsigned n = 0; n < p.count; ++n)
    if (!token_match (p.operands[n], operands[n])) return false;
  return true;
}
}  // namespace
bool aarch64_instruction_ok (MachineOpcode code, std::span<const MachineOperand> operands) {
  auto op = std::get_if<Opcode> (&code);
  if (!op) return false;
  auto wanted = static_cast<unsigned> (*op);
  if (wanted >= code_count) return false;
  // Overflow encodings select a different condition after sub/mul. Their
  // constraints are identical (one label), so legalization needs no def scan.
  for (unsigned n = table.offsets[wanted]; n < table.offsets[wanted + 1]; ++n)
    if (match (table.compiled[table.indexes[n]], operands)) return true;
  return false;
}
std::optional<AArch64Recipe> aarch64_encoding_recipe (MachineOpcode code,
                                                      std::span<const MachineOperand> operands,
                                                      AArch64Overflow overflow) {
  auto op = std::get_if<Opcode> (&code);
  if (!op) return {};
  auto wanted = static_cast<unsigned> (*op);
  if (*op == Opcode::Bo || *op == Opcode::Bno || *op == Opcode::UBo || *op == Opcode::UBno) {
    if (overflow == AArch64Overflow::Subtract && (*op == Opcode::UBo || *op == Opcode::UBno))
      wanted = static_cast<unsigned> (*op == Opcode::UBo ? PatternCode::SubUnsignedOverflow
                                                         : PatternCode::SubUnsignedNoOverflow);
    if (overflow == AArch64Overflow::Multiply)
      wanted = static_cast<unsigned> (*op == Opcode::Bo || *op == Opcode::UBo
                                        ? PatternCode::MultiplyOverflow
                                        : PatternCode::MultiplyNoOverflow);
  }
  if (wanted >= code_count) return {};
  for (unsigned n = table.offsets[wanted]; n < table.offsets[wanted + 1]; ++n) {
    auto row = table.indexes[n];
    if (match (table.compiled[row], operands))
      return AArch64Recipe{std::span (encoding.words)
                             .subspan (encoding.offsets[row],
                                       encoding.offsets[row + 1] - encoding.offsets[row]),
                           encoding.fields};
  }
  return {};
}
}  // namespace mirnext::codegen
