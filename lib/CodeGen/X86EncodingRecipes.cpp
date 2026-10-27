/* Rules derived from MIR, Copyright (C) 2018-2024 Vladimir Makarov. MIT License. */
#include "mirnext/CodeGen/CompiledFunction.hpp"
#include "mirnext/CodeGen/Detail/X86Encoding.hpp"
#include <array>
#include <climits>
namespace mirnext::codegen {
namespace {
struct Declaration {
  int code;
  std::string_view constraints, encoding;
};
#define MIRNEXT_X86_TABLE static constexpr Declaration declarations[]
#define MIRNEXT_X86_OPCODE(name) 0
#define MIRNEXT_X86_PRIVATE(name) 0
#include "mirnext/Target/X86Patterns.def"
#undef MIRNEXT_X86_PRIVATE
#undef MIRNEXT_X86_OPCODE
#undef MIRNEXT_X86_TABLE
constexpr int hex (char c) {
  return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
template <class Operation, class End>
constexpr bool parse (std::string_view text, Operation operation, End end) {
  using K = X86EncodingKind;
  unsigned opcodes = 0;
  bool prefix = false, rex = false, modrm = false, sib = false, disp8 = false, disp32 = false,
       imm8 = false, imm32 = false, imm64 = false, table = false;
  auto finish = [&] {
    end (opcodes + prefix + rex + modrm + sib + disp8 + 4 * disp32 + imm8 + 4 * imm32 + 8 * imm64
         + 4 * table);
    opcodes = 0;
    prefix = rex = modrm = sib = disp8 = disp32 = imm8 = imm32 = imm64 = table = false;
  };
  for (std::size_t p = 0; p < text.size ();) {
    auto c = text[p++];
    if (c == ' ' || c == '\t') continue;
    if (c == ';') {
      finish ();
      continue;
    }
    if (hex (c) >= 0) {
      if (p == text.size () || hex (text[p]) < 0 || opcodes == 3) return false;
      operation (K::Byte, unsigned (hex (c) * 16 + hex (text[p++])));
      ++opcodes;
      continue;
    }
    std::uint64_t n = 0;
    auto digit = [&] {
      if (p == text.size () || text[p] < '0' || text[p] > '7') return false;
      n = text[p++] - '0';
      return true;
    };
    auto number = [&] {
      const auto start = p;
      while (p < text.size () && hex (text[p]) >= 0) {
        if (n > (UINT64_MAX - unsigned (hex (text[p]))) / 16) return false;
        n = n * 16 + unsigned (hex (text[p++]));
      }
      return p != start;
    };
    K kind;
    switch (c) {
    case 'X':
    case 'Y':
    case 'Z':
      if (opcodes) {
        if (opcodes != 1 || prefix) return false;
        prefix = true;
        opcodes = 0;
      }
      rex = true;
      kind = K::Rex;
      n = c;
      break;
    case 'r':
    case 'R':
    case 'S':
      if (!digit () || n > 2) return false;
      modrm = true;
      kind = c == 'r' ? K::Register : c == 'R' ? K::RegisterRM : K::ByteRegisterRM;
      break;
    case 'm':
      if (p == text.size ()) return false;
      modrm = sib = true;
      if (text[p] == 't' || text[p] == 'T') {
        kind = text[p++] == 't' ? K::TemporaryMemory : K::SwitchMemory;
        disp8 = true;
      } else {
        if (!digit () || n > 2) return false;
        kind = K::Memory;
        disp32 = true;
      }
      break;
    case 'a':
      if (p == text.size ()) return false;
      c = text[p++];
      sib = true;
      if (c == 'p') {
        kind = K::AddressAdd;
        disp32 = true;
      } else if (c == 'm')
        kind = K::AddressScale;
      else if (c == 'd') {
        if (!number ()) return false;
        kind = K::AddressDisplacement;
        if (n <= 127 || n >= UINT64_MAX - 127)
          disp8 = true;
        else
          disp32 = true;
      } else
        return false;
      break;
    case 'i':
    case 'I':
    case 'J':
      if (!digit ()) return false;
      kind = c == 'i' ? K::Immediate8 : c == 'I' ? K::Immediate32 : K::Immediate64;
      if (c == 'i')
        imm8 = true;
      else if (c == 'I')
        imm32 = true;
      else
        imm64 = true;
      break;
    case 'T':
      kind = K::SwitchTable;
      table = modrm = true;
      break;
    case 'q':
      kind = K::RIPMemory;
      modrm = true;
      break;
    case 'l':
    case 'L':
      if (!digit () || n > 2) return false;
      kind = c == 'l' ? K::Label8 : K::Label32;
      if (c == 'l')
        disp8 = true;
      else
        disp32 = true;
      break;
    case 'P':
      if (!digit ()) return false;
      kind = K::CallPointer;
      modrm = disp32 = true;
      break;
    case '/':
      if (!digit ()) return false;
      kind = K::Extension;
      modrm = true;
      break;
    case '+':
      if (p == text.size ()) return false;
      if (text[p] == 'h') {
        ++p;
        if (p == text.size () || hex (text[p]) < 0) return false;
        n = hex (text[p++]);
        kind = K::OpcodePhysical;
      } else {
        if (!digit () || n > 2) return false;
        kind = K::OpcodeRegister;
      }
      if (!opcodes) opcodes = 1;
      break;
    case 'c':
      if (!number ()) return false;
      kind = K::PoolConstant;
      disp32 = true;
      break;
    case 'h':
    case 'H':
      if (!number () || n > 31) return false;
      kind = c == 'h' ? K::PhysicalRegister : K::PhysicalRM;
      modrm = true;
      break;
    case 'v':
    case 'V':
      if (!number () || n > (c == 'v' ? UINT8_MAX : UINT32_MAX)) return false;
      kind = c == 'v' ? K::Literal8 : K::Literal32;
      if (c == 'v')
        imm8 = true;
      else
        imm32 = true;
      break;
    default: return false;
    }
    operation (kind, n);
  }
  finish ();
  return true;
}
constexpr auto counts = [] {
  struct Counts {
    std::size_t operations = 0, words = 0;
    bool valid = true;
  } out;
  for (auto &d : declarations)
    out.valid
      &= parse (d.encoding, [&] (auto, auto) { ++out.operations; }, [&] (auto) { ++out.words; });
  return out;
}();
static_assert (counts.valid, "invalid x86 encoding declaration");
constexpr auto compiled = [] {
  struct RecipeIndex {
    std::size_t first_word = 0, word_count = 0, maximum = 0;
  };
  struct Table {
    std::array<X86EncodingOperation, counts.operations> operations{};
    std::array<X86EncodingWord, counts.words> words{};
    std::array<RecipeIndex, std::size (declarations)> recipes{};
  } out;
  std::size_t operation = 0, word = 0;
  for (std::size_t n = 0; n < std::size (declarations); ++n) {
    auto &r = out.recipes[n];
    r.first_word = word;
    auto start = operation;
    parse (
      declarations[n].encoding,
      [&] (auto kind, auto value) { out.operations[operation++] = {kind, value}; },
      [&] (auto maximum) {
        out.words[word++] = {start, operation - start, maximum};
        start = operation;
        ++r.word_count;
        r.maximum += maximum;
      });
  }
  return out;
}();
}  // namespace
std::optional<X86EncodingRecipe> x86_encoding_recipe (std::size_t index) {
  if (index >= compiled.recipes.size ()) return {};
  auto r = compiled.recipes[index];
  return X86EncodingRecipe{std::span (compiled.words).subspan (r.first_word, r.word_count),
                           compiled.operations, r.maximum};
}
}  // namespace mirnext::codegen
