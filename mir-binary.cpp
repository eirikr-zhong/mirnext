/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <array>
#include <climits>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mirnext {
namespace {

enum class BinaryType : std::uint8_t {
  I8,
  U8,
  I16,
  U16,
  I32,
  U32,
  I64,
  U64,
  F,
  D,
  LD,
  P,
};

enum class Tag : std::uint8_t {
  U0 = 0,
  U1 = 1,
  I1 = 9,
  F = 17,
  D = 18,
  LD = 19,
  REG1 = 20,
  NAME1 = 24,
  STR1 = 28,
  LAB1 = 32,
  MEM_DISP = 36,
  MEM_BASE = 37,
  MEM_INDEX = 38,
  MEM_DISP_BASE = 39,
  MEM_DISP_INDEX = 40,
  MEM_BASE_INDEX = 41,
  MEM_DISP_BASE_INDEX = 42,
  TI8 = 43,
  EOI = 61,
  EOFILE = 62,
};

enum class BinaryOpcode : std::uint64_t {
  Mov = 0,
  FMov = 1,
  DMov = 2,
  LDMov = 3,
  Ext8 = 4,
  Ext16 = 5,
  Ext32 = 6,
  UExt8 = 7,
  UExt16 = 8,
  UExt32 = 9,
  I2F = 10,
  I2D = 11,
  I2LD = 12,
  UI2F = 13,
  UI2D = 14,
  UI2LD = 15,
  F2I = 16,
  D2I = 17,
  LD2I = 18,
  F2D = 19,
  F2LD = 20,
  D2F = 21,
  D2LD = 22,
  LD2F = 23,
  LD2D = 24,
  Neg = 25,
  Negs = 26,
  FNeg = 27,
  DNeg = 28,
  LDNeg = 29,
  Addr = 30,
  Add = 34,
  Adds = 35,
  FAdd = 36,
  DAdd = 37,
  LDAdd = 38,
  Sub = 39,
  Subs = 40,
  FSub = 41,
  DSub = 42,
  LDSub = 43,
  Mul = 44,
  Muls = 45,
  FMul = 46,
  DMul = 47,
  LDMul = 48,
  Div = 49,
  Divs = 50,
  UDiv = 51,
  UDivs = 52,
  FDiv = 53,
  DDiv = 54,
  LDDiv = 55,
  Mod = 56,
  Mods = 57,
  UMod = 58,
  UMods = 59,
  And = 60,
  Ands = 61,
  Or = 62,
  Ors = 63,
  Xor = 64,
  Xors = 65,
  Lsh = 66,
  Lshs = 67,
  Rsh = 68,
  Rshs = 69,
  URsh = 70,
  URshs = 71,
  Eq = 72,
  Eqs = 73,
  FEq = 74,
  DEq = 75,
  LDEq = 76,
  Ne = 77,
  Nes = 78,
  FNe = 79,
  DNe = 80,
  LDNe = 81,
  Lt = 82,
  Lts = 83,
  ULt = 84,
  ULts = 85,
  FLt = 86,
  DLt = 87,
  LDLt = 88,
  Le = 89,
  Les = 90,
  ULe = 91,
  ULes = 92,
  FLe = 93,
  DLe = 94,
  LDLe = 95,
  Gt = 96,
  Gts = 97,
  UGt = 98,
  UGts = 99,
  FGt = 100,
  DGt = 101,
  LDGt = 102,
  Ge = 103,
  Ges = 104,
  UGe = 105,
  UGes = 106,
  FGe = 107,
  DGe = 108,
  LDGe = 109,
  Addo = 110,
  Addos = 111,
  Subo = 112,
  Subos = 113,
  Mulo = 114,
  Mulos = 115,
  UMulo = 116,
  UMulos = 117,
  Jmp = 118,
  Bt = 119,
  Bts = 120,
  Bf = 121,
  Bfs = 122,
  Beq = 123,
  Beqs = 124,
  FBeq = 125,
  DBeq = 126,
  LDBeq = 127,
  Bne = 128,
  Bnes = 129,
  FBne = 130,
  DBne = 131,
  LDBne = 132,
  Blt = 133,
  Blts = 134,
  UBlt = 135,
  UBlts = 136,
  FBlt = 137,
  DBlt = 138,
  LDBlt = 139,
  Ble = 140,
  Bles = 141,
  UBle = 142,
  UBles = 143,
  FBle = 144,
  DBle = 145,
  LDBle = 146,
  Bgt = 147,
  Bgts = 148,
  UBgt = 149,
  UBgts = 150,
  FBgt = 151,
  DBgt = 152,
  LDBgt = 153,
  Bge = 154,
  Bges = 155,
  UBge = 156,
  UBges = 157,
  FBge = 158,
  DBge = 159,
  LDBge = 160,
  Bo = 161,
  UBo = 162,
  Bno = 163,
  UBno = 164,
  Call = 167,
  Switch = 170,
  Ret = 171,
  Alloca = 173,
};

class StringTable {
public:
  void add_name(std::string_view name) {
    std::string bytes(name);
    bytes.push_back('\0');
    add(bytes);
  }

  void add_string(std::string_view value) {
    add(std::string(value));
  }

  Result<std::uint64_t> name_index(std::string_view name) const {
    std::string bytes(name);
    bytes.push_back('\0');
    const auto it = index_.find(bytes);
    if (it == index_.end()) return Error{ErrorCode::InvalidArgument, "missing binary string"};
    return it->second - 1;
  }

  Result<std::uint64_t> string_index(std::string_view value) const {
    std::string bytes(value);
    const auto it = index_.find(bytes);
    if (it == index_.end()) return Error{ErrorCode::InvalidArgument, "missing binary string"};
    return it->second - 1;
  }

  const std::vector<std::string> &strings() const noexcept { return strings_; }

private:
  void add(const std::string &bytes) {
    if (index_.find(bytes) != index_.end()) return;
    strings_.push_back(bytes);
    index_.emplace(strings_.back(), strings_.size());
  }

  std::vector<std::string> strings_;
  std::unordered_map<std::string, std::uint64_t> index_;
};

std::size_t uint_length(std::uint64_t value) {
  if (value <= 127) return 0;
  std::size_t length = 0;
  for (; value != 0; ++length) value >>= CHAR_BIT;
  return length;
}

std::size_t int_length(std::int64_t value) {
  std::uint64_t bits = static_cast<std::uint64_t>(value);
  std::size_t length = 0;
  for (; bits != 0; ++length) bits >>= CHAR_BIT;
  return length == 0 ? 1 : length;
}

void append_byte(std::vector<std::byte> &out, std::uint8_t byte) {
  out.push_back(static_cast<std::byte>(byte));
}

void append_little_uint(std::vector<std::byte> &out, std::uint64_t value, std::size_t bytes) {
  for (std::size_t i = 0; i < bytes; ++i) {
    append_byte(out, static_cast<std::uint8_t>(value & 0xffu));
    value >>= CHAR_BIT;
  }
}

void write_uint(std::vector<std::byte> &out, std::uint64_t value) {
  const std::size_t length = uint_length(value);
  if (length == 0) {
    append_byte(out, static_cast<std::uint8_t>(0x80u | value));
    return;
  }
  append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(Tag::U1) + length - 1));
  append_little_uint(out, value, length);
}

void write_int(std::vector<std::byte> &out, std::int64_t value) {
  const std::size_t length = int_length(value);
  append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(Tag::I1) + length - 1));
  append_little_uint(out, static_cast<std::uint64_t>(value), length);
}

void write_float(std::vector<std::byte> &out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  append_byte(out, static_cast<std::uint8_t>(Tag::F));
  append_little_uint(out, bits, sizeof(bits));
}

void write_double(std::vector<std::byte> &out, double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  append_byte(out, static_cast<std::uint8_t>(Tag::D));
  append_little_uint(out, bits, sizeof(bits));
}

void write_long_double(std::vector<std::byte> &out, long double value) {
  std::array<std::uint8_t, 16> bytes{};
  const std::size_t copy = sizeof(long double) < bytes.size() ? sizeof(long double) : bytes.size();
  std::memcpy(bytes.data(), &value, copy);
  append_byte(out, static_cast<std::uint8_t>(Tag::LD));
  for (std::uint8_t byte : bytes) append_byte(out, byte);
}

Result<void *> write_name(std::vector<std::byte> &out, const StringTable &strings,
                          std::string_view name, Tag base_tag) {
  auto index_result = strings.name_index(name);
  MIRNEXT_RESULT_RET(index_result);
  auto index = *index_result;
  std::size_t length = uint_length(index);
  if (length == 0) length = 1;
  if (length > 4) {
    return Error{ErrorCode::InvalidArgument, "binary string table is too large"};
  }
  append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(base_tag) + length - 1));
  append_little_uint(out, index, length);
  return static_cast<void *>(nullptr);
}

Result<void *> write_string_ref(std::vector<std::byte> &out, const StringTable &strings,
                                std::string_view value, Tag base_tag) {
  auto index_result = strings.string_index(value);
  MIRNEXT_RESULT_RET(index_result);
  auto index = *index_result;
  std::size_t length = uint_length(index);
  if (length == 0) length = 1;
  if (length > 4) {
    return Error{ErrorCode::InvalidArgument, "binary string table is too large"};
  }
  append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(base_tag) + length - 1));
  append_little_uint(out, index, length);
  return static_cast<void *>(nullptr);
}

Result<void *> write_type(std::vector<std::byte> &out, BinaryType type) {
  append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(Tag::TI8)
                                            + static_cast<std::uint8_t>(type)));
  return static_cast<void *>(nullptr);
}

Result<BinaryType> lower_type(Type type) {
  switch (type.kind()) {
  case Type::Kind::B: return BinaryType::I64;
  case Type::Kind::I8: return BinaryType::I8;
  case Type::Kind::U8: return BinaryType::U8;
  case Type::Kind::I16: return BinaryType::I16;
  case Type::Kind::U16: return BinaryType::U16;
  case Type::Kind::I32: return BinaryType::I32;
  case Type::Kind::U32: return BinaryType::U32;
  case Type::Kind::I64: return BinaryType::I64;
  case Type::Kind::U64: return BinaryType::U64;
  case Type::Kind::F: return BinaryType::F;
  case Type::Kind::D: return BinaryType::D;
  case Type::Kind::LD:
#if defined(_WIN32) || (defined(__SIZEOF_LONG_DOUBLE__) && __SIZEOF_LONG_DOUBLE__ == 8)
    return BinaryType::D;
#else
    return BinaryType::LD;
#endif
  case Type::Kind::P: return BinaryType::I64;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext type"};
}

Result<BinaryType> lower_register_type(Type type) {
  switch (type.kind()) {
  case Type::Kind::B:
  case Type::Kind::I8:
  case Type::Kind::U8:
  case Type::Kind::I16:
  case Type::Kind::U16:
  case Type::Kind::I32:
  case Type::Kind::U32:
  case Type::Kind::I64:
  case Type::Kind::U64:
  case Type::Kind::P:
    return BinaryType::I64;
  case Type::Kind::F:
  case Type::Kind::D:
  case Type::Kind::LD:
    return lower_type(type);
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext register type"};
}

Result<BinaryType> lower_data_type(Type type) {
  switch (type.kind()) {
  case Type::Kind::B:
    return Error{ErrorCode::InvalidArgument, "unsupported MIRNext data type"};
  case Type::Kind::I8: return BinaryType::I8;
  case Type::Kind::U8: return BinaryType::U8;
  case Type::Kind::I16: return BinaryType::I16;
  case Type::Kind::U16: return BinaryType::U16;
  case Type::Kind::I32: return BinaryType::I32;
  case Type::Kind::U32: return BinaryType::U32;
  case Type::Kind::I64: return BinaryType::I64;
  case Type::Kind::U64: return BinaryType::U64;
  case Type::Kind::F: return BinaryType::F;
  case Type::Kind::D: return BinaryType::D;
  case Type::Kind::LD: return BinaryType::LD;
  case Type::Kind::P: return BinaryType::U64;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext data type"};
}

Result<BinaryOpcode> lower_opcode(Opcode opcode) {
  switch (opcode) {
  case Opcode::Mov: return BinaryOpcode::Mov;
  case Opcode::FMov: return BinaryOpcode::FMov;
  case Opcode::DMov: return BinaryOpcode::DMov;
  case Opcode::LDMov: return BinaryOpcode::LDMov;
  case Opcode::Ext8: return BinaryOpcode::Ext8;
  case Opcode::Ext16: return BinaryOpcode::Ext16;
  case Opcode::Ext32: return BinaryOpcode::Ext32;
  case Opcode::UExt8: return BinaryOpcode::UExt8;
  case Opcode::UExt16: return BinaryOpcode::UExt16;
  case Opcode::UExt32: return BinaryOpcode::UExt32;
  case Opcode::I2F: return BinaryOpcode::I2F;
  case Opcode::I2D: return BinaryOpcode::I2D;
  case Opcode::I2LD: return BinaryOpcode::I2LD;
  case Opcode::UI2F: return BinaryOpcode::UI2F;
  case Opcode::UI2D: return BinaryOpcode::UI2D;
  case Opcode::UI2LD: return BinaryOpcode::UI2LD;
  case Opcode::F2I: return BinaryOpcode::F2I;
  case Opcode::D2I: return BinaryOpcode::D2I;
  case Opcode::LD2I: return BinaryOpcode::LD2I;
  case Opcode::F2D: return BinaryOpcode::F2D;
  case Opcode::F2LD: return BinaryOpcode::F2LD;
  case Opcode::D2F: return BinaryOpcode::D2F;
  case Opcode::D2LD: return BinaryOpcode::D2LD;
  case Opcode::LD2F: return BinaryOpcode::LD2F;
  case Opcode::LD2D: return BinaryOpcode::LD2D;
  case Opcode::Addr: return BinaryOpcode::Addr;
  case Opcode::Alloca: return BinaryOpcode::Alloca;
  case Opcode::Neg: return BinaryOpcode::Neg;
  case Opcode::Negs: return BinaryOpcode::Negs;
  case Opcode::FNeg: return BinaryOpcode::FNeg;
  case Opcode::DNeg: return BinaryOpcode::DNeg;
  case Opcode::LDNeg: return BinaryOpcode::LDNeg;
  case Opcode::Add: return BinaryOpcode::Add;
  case Opcode::Adds: return BinaryOpcode::Adds;
  case Opcode::FAdd: return BinaryOpcode::FAdd;
  case Opcode::DAdd: return BinaryOpcode::DAdd;
  case Opcode::LDAdd: return BinaryOpcode::LDAdd;
  case Opcode::Sub: return BinaryOpcode::Sub;
  case Opcode::Subs: return BinaryOpcode::Subs;
  case Opcode::FSub: return BinaryOpcode::FSub;
  case Opcode::DSub: return BinaryOpcode::DSub;
  case Opcode::LDSub: return BinaryOpcode::LDSub;
  case Opcode::Mul: return BinaryOpcode::Mul;
  case Opcode::Muls: return BinaryOpcode::Muls;
  case Opcode::FMul: return BinaryOpcode::FMul;
  case Opcode::DMul: return BinaryOpcode::DMul;
  case Opcode::LDMul: return BinaryOpcode::LDMul;
  case Opcode::Div: return BinaryOpcode::Div;
  case Opcode::Divs: return BinaryOpcode::Divs;
  case Opcode::UDiv: return BinaryOpcode::UDiv;
  case Opcode::UDivs: return BinaryOpcode::UDivs;
  case Opcode::FDiv: return BinaryOpcode::FDiv;
  case Opcode::DDiv: return BinaryOpcode::DDiv;
  case Opcode::LDDiv: return BinaryOpcode::LDDiv;
  case Opcode::Mod: return BinaryOpcode::Mod;
  case Opcode::Mods: return BinaryOpcode::Mods;
  case Opcode::UMod: return BinaryOpcode::UMod;
  case Opcode::UMods: return BinaryOpcode::UMods;
  case Opcode::And: return BinaryOpcode::And;
  case Opcode::Ands: return BinaryOpcode::Ands;
  case Opcode::Or: return BinaryOpcode::Or;
  case Opcode::Ors: return BinaryOpcode::Ors;
  case Opcode::Xor: return BinaryOpcode::Xor;
  case Opcode::Xors: return BinaryOpcode::Xors;
  case Opcode::Lsh: return BinaryOpcode::Lsh;
  case Opcode::Lshs: return BinaryOpcode::Lshs;
  case Opcode::Rsh: return BinaryOpcode::Rsh;
  case Opcode::Rshs: return BinaryOpcode::Rshs;
  case Opcode::URsh: return BinaryOpcode::URsh;
  case Opcode::URshs: return BinaryOpcode::URshs;
  case Opcode::Eq: return BinaryOpcode::Eq;
  case Opcode::Eqs: return BinaryOpcode::Eqs;
  case Opcode::FEq: return BinaryOpcode::FEq;
  case Opcode::DEq: return BinaryOpcode::DEq;
  case Opcode::LDEq: return BinaryOpcode::LDEq;
  case Opcode::Ne: return BinaryOpcode::Ne;
  case Opcode::Nes: return BinaryOpcode::Nes;
  case Opcode::FNe: return BinaryOpcode::FNe;
  case Opcode::DNe: return BinaryOpcode::DNe;
  case Opcode::LDNe: return BinaryOpcode::LDNe;
  case Opcode::Lt: return BinaryOpcode::Lt;
  case Opcode::Lts: return BinaryOpcode::Lts;
  case Opcode::ULt: return BinaryOpcode::ULt;
  case Opcode::ULts: return BinaryOpcode::ULts;
  case Opcode::FLt: return BinaryOpcode::FLt;
  case Opcode::DLt: return BinaryOpcode::DLt;
  case Opcode::LDLt: return BinaryOpcode::LDLt;
  case Opcode::Le: return BinaryOpcode::Le;
  case Opcode::Les: return BinaryOpcode::Les;
  case Opcode::ULe: return BinaryOpcode::ULe;
  case Opcode::ULes: return BinaryOpcode::ULes;
  case Opcode::FLe: return BinaryOpcode::FLe;
  case Opcode::DLe: return BinaryOpcode::DLe;
  case Opcode::LDLe: return BinaryOpcode::LDLe;
  case Opcode::Gt: return BinaryOpcode::Gt;
  case Opcode::Gts: return BinaryOpcode::Gts;
  case Opcode::UGt: return BinaryOpcode::UGt;
  case Opcode::UGts: return BinaryOpcode::UGts;
  case Opcode::FGt: return BinaryOpcode::FGt;
  case Opcode::DGt: return BinaryOpcode::DGt;
  case Opcode::LDGt: return BinaryOpcode::LDGt;
  case Opcode::Ge: return BinaryOpcode::Ge;
  case Opcode::Ges: return BinaryOpcode::Ges;
  case Opcode::UGe: return BinaryOpcode::UGe;
  case Opcode::UGes: return BinaryOpcode::UGes;
  case Opcode::FGe: return BinaryOpcode::FGe;
  case Opcode::DGe: return BinaryOpcode::DGe;
  case Opcode::LDGe: return BinaryOpcode::LDGe;
  case Opcode::Addo: return BinaryOpcode::Addo;
  case Opcode::Addos: return BinaryOpcode::Addos;
  case Opcode::Subo: return BinaryOpcode::Subo;
  case Opcode::Subos: return BinaryOpcode::Subos;
  case Opcode::Mulo: return BinaryOpcode::Mulo;
  case Opcode::Mulos: return BinaryOpcode::Mulos;
  case Opcode::UMulo: return BinaryOpcode::UMulo;
  case Opcode::UMulos: return BinaryOpcode::UMulos;
  case Opcode::Jmp: return BinaryOpcode::Jmp;
  case Opcode::Bt: return BinaryOpcode::Bt;
  case Opcode::Bts: return BinaryOpcode::Bts;
  case Opcode::Bf: return BinaryOpcode::Bf;
  case Opcode::Bfs: return BinaryOpcode::Bfs;
  case Opcode::Beq: return BinaryOpcode::Beq;
  case Opcode::Beqs: return BinaryOpcode::Beqs;
  case Opcode::FBeq: return BinaryOpcode::FBeq;
  case Opcode::DBeq: return BinaryOpcode::DBeq;
  case Opcode::LDBeq: return BinaryOpcode::LDBeq;
  case Opcode::Bne: return BinaryOpcode::Bne;
  case Opcode::Bnes: return BinaryOpcode::Bnes;
  case Opcode::FBne: return BinaryOpcode::FBne;
  case Opcode::DBne: return BinaryOpcode::DBne;
  case Opcode::LDBne: return BinaryOpcode::LDBne;
  case Opcode::Blt: return BinaryOpcode::Blt;
  case Opcode::Blts: return BinaryOpcode::Blts;
  case Opcode::UBlt: return BinaryOpcode::UBlt;
  case Opcode::UBlts: return BinaryOpcode::UBlts;
  case Opcode::FBlt: return BinaryOpcode::FBlt;
  case Opcode::DBlt: return BinaryOpcode::DBlt;
  case Opcode::LDBlt: return BinaryOpcode::LDBlt;
  case Opcode::Ble: return BinaryOpcode::Ble;
  case Opcode::Bles: return BinaryOpcode::Bles;
  case Opcode::UBle: return BinaryOpcode::UBle;
  case Opcode::UBles: return BinaryOpcode::UBles;
  case Opcode::FBle: return BinaryOpcode::FBle;
  case Opcode::DBle: return BinaryOpcode::DBle;
  case Opcode::LDBle: return BinaryOpcode::LDBle;
  case Opcode::Bgt: return BinaryOpcode::Bgt;
  case Opcode::Bgts: return BinaryOpcode::Bgts;
  case Opcode::UBgt: return BinaryOpcode::UBgt;
  case Opcode::UBgts: return BinaryOpcode::UBgts;
  case Opcode::FBgt: return BinaryOpcode::FBgt;
  case Opcode::DBgt: return BinaryOpcode::DBgt;
  case Opcode::LDBgt: return BinaryOpcode::LDBgt;
  case Opcode::Bge: return BinaryOpcode::Bge;
  case Opcode::Bges: return BinaryOpcode::Bges;
  case Opcode::UBge: return BinaryOpcode::UBge;
  case Opcode::UBges: return BinaryOpcode::UBges;
  case Opcode::FBge: return BinaryOpcode::FBge;
  case Opcode::DBge: return BinaryOpcode::DBge;
  case Opcode::LDBge: return BinaryOpcode::LDBge;
  case Opcode::Bo: return BinaryOpcode::Bo;
  case Opcode::UBo: return BinaryOpcode::UBo;
  case Opcode::Bno: return BinaryOpcode::Bno;
  case Opcode::UBno: return BinaryOpcode::UBno;
  case Opcode::Ret: return BinaryOpcode::Ret;
  case Opcode::Call: return BinaryOpcode::Call;
  case Opcode::Switch: return BinaryOpcode::Switch;
  case Opcode::Nop:
  case Opcode::Label:
    break;
  }
  return Error{ErrorCode::InvalidArgument, "unsupported MIRNext instruction"};
}

struct FunctionTables {
  std::unordered_set<std::size_t> labels;
};

class ModuleWriter {
public:
  explicit ModuleWriter(const Module &module) : module_(module) {
    for (const auto &prototype : module_.prototypes()) {
      refs_.emplace(prototype.get(), std::string(prototype->name()));
    }
    for (const auto &import : module_.imports()) {
      refs_.emplace(import.get(), std::string(import->name()));
    }
    for (const auto &function : module_.functions()) {
      refs_.emplace(function.get(), std::string(function->name()));
    }
    for (const auto &data : module_.data_items()) {
      if (!data->name().empty()) refs_.emplace(data.get(), std::string(data->name()));
    }
  }

  Result<std::vector<std::byte>> write_raw() {
    auto collect_result = collect_strings();
    MIRNEXT_RESULT_RET(collect_result);

    std::vector<std::byte> out;
    write_uint(out, 1);
    write_uint(out, strings_.strings().size());
    for (const std::string &string : strings_.strings()) {
      write_uint(out, string.size());
      for (unsigned char ch : string) append_byte(out, ch);
    }
    auto module_result = write_module(out);
    MIRNEXT_RESULT_RET(module_result);
    append_byte(out, static_cast<std::uint8_t>(Tag::EOFILE));
    return out;
  }

private:
  Result<void *> collect_strings() {
    std::vector<std::byte> sink;
    return write_module(sink, true);
  }

  Result<void *> write_name_token(std::vector<std::byte> &out, std::string_view name,
                                  Tag base_tag = Tag::NAME1, bool collect_only = false) {
    if (collect_only) {
      strings_.add_name(name);
      return static_cast<void *>(nullptr);
    }
    return write_name(out, strings_, name, base_tag);
  }

  Result<void *> write_string_token(std::vector<std::byte> &out, std::string_view value,
                                    Tag base_tag = Tag::STR1, bool collect_only = false) {
    if (collect_only) {
      strings_.add_string(value);
      return static_cast<void *>(nullptr);
    }
    return write_string_ref(out, strings_, value, base_tag);
  }

  Result<void *> write_module(std::vector<std::byte> &out, bool collect_only = false) {
    auto result = write_name_token(out, "module", Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    result = write_name_token(out, module_.name(), Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    for (const auto &prototype : module_.prototypes()) {
      result = write_prototype(out, *prototype, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    for (const auto &import : module_.imports()) {
      result = write_name_token(out, "import", Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
      result = write_name_token(out, import->name(), Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    for (const auto &function : module_.functions()) {
      result = write_name_token(out, "forward", Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
      result = write_name_token(out, function->name(), Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    for (const auto &data : module_.data_items()) {
      result = write_data(out, *data, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    for (const auto &function : module_.functions()) {
      result = write_function(out, *function, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    result = write_name_token(out, "endmodule", Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_signature(std::vector<std::byte> &out,
                                 const std::vector<Type> &return_types,
                                 const std::vector<Prototype::Parameter> &parameters,
                                 bool collect_only) {
    if (!collect_only) {
      write_uint(out, 0);
      write_uint(out, return_types.size());
      for (Type type : return_types) {
        auto lowered_result = lower_type(type);
        MIRNEXT_RESULT_RET(lowered_result);
        auto write_result = write_type(out, *lowered_result);
        MIRNEXT_RESULT_RET(write_result);
      }
    } else {
      for (Type type : return_types) {
        auto lowered_result = lower_type(type);
        MIRNEXT_RESULT_RET(lowered_result);
      }
    }
    for (const Prototype::Parameter &parameter : parameters) {
      auto lowered_result = lower_type(parameter.type);
      MIRNEXT_RESULT_RET(lowered_result);
      if (!collect_only) {
        auto write_result = write_type(out, *lowered_result);
        MIRNEXT_RESULT_RET(write_result);
      }
      auto name_result = write_name_token(out, parameter.name, Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(name_result);
    }
    if (!collect_only) append_byte(out, static_cast<std::uint8_t>(Tag::EOI));
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_function_signature(std::vector<std::byte> &out,
                                          const Function &function, bool collect_only) {
    std::vector<Prototype::Parameter> parameters;
    parameters.reserve(function.arguments().size());
    for (const Register &argument : function.arguments()) {
      parameters.push_back(Prototype::Parameter{argument.type(), std::string(argument.name())});
    }
    return write_signature(out, function.return_types(), parameters, collect_only);
  }

  Result<void *> write_prototype(std::vector<std::byte> &out, const Prototype &prototype,
                                 bool collect_only) {
    auto result = write_name_token(out, "proto", Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    result = write_name_token(out, prototype.name(), Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    result = write_signature(out, prototype.return_types(), prototype.parameters(), collect_only);
    MIRNEXT_RESULT_RET(result);
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_named_item_header(std::vector<std::byte> &out, std::string_view item,
                                         std::string_view named_item,
                                         std::string_view name, bool collect_only) {
    auto result = write_name_token(out, name.empty() ? item : named_item,
                                   Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    if (!name.empty()) {
      result = write_name_token(out, name, Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_data(std::vector<std::byte> &out, const Data &data, bool collect_only) {
    switch (data.kind()) {
    case Data::Kind::Bss: {
      auto result = write_named_item_header(out, "bss", "nbss", data.name(), collect_only);
      MIRNEXT_RESULT_RET(result);
      if (!collect_only) write_uint(out, data.size());
      return static_cast<void *>(nullptr);
    }
    case Data::Kind::Typed:
      return write_typed_data(out, data.name(), data.element_type(), data.values(),
                              collect_only);
    case Data::Kind::String:
      return write_string_data(out, data.name(), data.string_value(), collect_only);
    case Data::Kind::Ref:
      return write_ref_data(out, data, collect_only);
    }
    return Error{ErrorCode::InvalidArgument, "unsupported MIRNext data item"};
  }

  Result<void *> write_typed_data(std::vector<std::byte> &out, std::string_view name,
                                  Type element_type, const std::vector<Operand> &values,
                                  bool collect_only) {
    auto result = write_named_item_header(out, "data", "ndata", name, collect_only);
    MIRNEXT_RESULT_RET(result);
    auto type_result = lower_data_type(element_type);
    MIRNEXT_RESULT_RET(type_result);
    if (!collect_only) {
      result = write_type(out, *type_result);
      MIRNEXT_RESULT_RET(result);
    }
    for (const Operand &value : values) {
      result = write_data_value(out, value, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    if (!collect_only) append_byte(out, static_cast<std::uint8_t>(Tag::EOI));
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_string_data(std::vector<std::byte> &out, std::string_view name,
                                   std::string_view value, bool collect_only) {
    auto result = write_named_item_header(out, "data", "ndata", name, collect_only);
    MIRNEXT_RESULT_RET(result);
    if (!collect_only) {
      result = write_type(out, BinaryType::U8);
      MIRNEXT_RESULT_RET(result);
    }
    for (unsigned char ch : value) {
      if (!collect_only) write_uint(out, ch);
    }
    if (!collect_only) append_byte(out, static_cast<std::uint8_t>(Tag::EOI));
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_data_value(std::vector<std::byte> &out, const Operand &value,
                                  bool collect_only) {
    switch (value.kind()) {
    case Operand::Kind::Int64:
      if (!collect_only) write_int(out, value.int64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::UInt64:
      if (!collect_only) write_uint(out, value.uint64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::Float32:
      if (!collect_only) write_float(out, value.float32_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::Float64:
      if (!collect_only) write_double(out, value.float64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::LongDouble:
      if (!collect_only) write_long_double(out, value.long_double_value());
      return static_cast<void *>(nullptr);
    default:
      return Error{ErrorCode::InvalidOperand, "unsupported data value operand"};
    }
  }

  Result<void *> write_ref_data(std::vector<std::byte> &out, const Data &data,
                                bool collect_only) {
    auto result = write_named_item_header(out, "ref", "nref", data.name(), collect_only);
    MIRNEXT_RESULT_RET(result);
    const auto &target = data.ref_target();
    if (target.pointer == nullptr) {
      return Error{ErrorCode::InvalidOperand, "ref data target is null"};
    }
    const auto it = refs_.find(target.pointer);
    if (it == refs_.end()) return Error{ErrorCode::InvalidOperand, "unknown ref data target"};
    result = write_name_token(out, it->second, Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    if (!collect_only) write_int(out, target.displacement);
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_function(std::vector<std::byte> &out, const Function &function,
                                bool collect_only) {
    FunctionTables tables;
    for (const auto &instruction : function.instructions()) {
      if (instruction->opcode() == Opcode::Label) tables.labels.insert(instruction->label_id());
    }

    auto result = write_name_token(out, "func", Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    result = write_name_token(out, function.name(), Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    result = write_function_signature(out, function, collect_only);
    MIRNEXT_RESULT_RET(result);

    if (!function.local_registers().empty()) {
      result = write_name_token(out, "local", Tag::NAME1, collect_only);
      MIRNEXT_RESULT_RET(result);
      for (const Register &local : function.local_registers()) {
        auto type_result = lower_register_type(local.type());
        MIRNEXT_RESULT_RET(type_result);
        auto type = *type_result;
        if (!collect_only) {
          result = write_type(out, type);
          MIRNEXT_RESULT_RET(result);
        }
        result = write_name_token(out, local.name(), Tag::NAME1, collect_only);
        MIRNEXT_RESULT_RET(result);
      }
      if (!collect_only) append_byte(out, static_cast<std::uint8_t>(Tag::EOI));
    }

    for (const auto &instruction : function.instructions()) {
      result = write_instruction(out, function, tables, *instruction, collect_only);
      MIRNEXT_RESULT_RET(result);
    }
    result = write_name_token(out, "endfunc", Tag::NAME1, collect_only);
    MIRNEXT_RESULT_RET(result);
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_label(std::vector<std::byte> &out, std::size_t label_id,
                             bool collect_only) {
    if (collect_only) return static_cast<void *>(nullptr);
    std::size_t length = uint_length(label_id);
    if (length == 0) length = 1;
    if (length > 4) return Error{ErrorCode::InvalidOperand, "label id is too large"};
    append_byte(out, static_cast<std::uint8_t>(static_cast<std::uint8_t>(Tag::LAB1) + length - 1));
    append_little_uint(out, label_id, length);
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_operand(std::vector<std::byte> &out, const Function &function,
                               const FunctionTables &tables, const Operand &operand,
                               bool collect_only) {
    switch (operand.kind()) {
    case Operand::Kind::Poison:
      return Error{ErrorCode::InvalidOperand, "poison operand cannot be encoded"};
    case Operand::Kind::Int64:
      if (!collect_only) write_int(out, operand.int64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::UInt64:
      if (!collect_only) write_uint(out, operand.uint64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::Float32:
      if (!collect_only) write_float(out, operand.float32_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::Float64:
      if (!collect_only) write_double(out, operand.float64_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::LongDouble:
      if (!collect_only) write_long_double(out, operand.long_double_value());
      return static_cast<void *>(nullptr);
    case Operand::Kind::Register: {
      const Register *reg = function.find_register(operand.register_id());
      if (reg == nullptr) return Error{ErrorCode::InvalidOperand, "unknown register operand"};
      return write_name_token(out, reg->name(), Tag::REG1, collect_only);
    }
    case Operand::Kind::LabelRef:
      if (tables.labels.find(operand.label_id()) == tables.labels.end()) {
        return Error{ErrorCode::InvalidOperand, "unknown label operand"};
      }
      return write_label(out, operand.label_id(), collect_only);
    case Operand::Kind::Memory:
      return write_memory(out, function, operand, collect_only);
    case Operand::Kind::ModuleSlot:
      return Error{ErrorCode::InvalidOperand, "module binding operand is not supported by binary encoding"};
    case Operand::Kind::Reference: {
      const auto it = refs_.find(operand.reference_pointer());
      if (it == refs_.end()) return Error{ErrorCode::InvalidOperand, "unknown reference operand"};
      return write_name_token(out, it->second, Tag::NAME1, collect_only);
    }
    }
    return Error{ErrorCode::InvalidOperand, "unsupported MIRNext operand"};
  }

  Result<void *> write_memory(std::vector<std::byte> &out, const Function &function,
                              const Operand &operand, bool collect_only) {
    const bool has_base = operand.memory_base_register_id() != 0;
    const bool has_index = operand.memory_index_register_id() != 0;
    const Register *base
        = has_base ? function.find_register(operand.memory_base_register_id()) : nullptr;
    const Register *index
        = has_index ? function.find_register(operand.memory_index_register_id()) : nullptr;
    if ((has_base && base == nullptr) || (has_index && index == nullptr)) {
      return Error{ErrorCode::InvalidOperand, "unknown memory register operand"};
    }
    auto type_result = lower_type(operand.memory_type());
    MIRNEXT_RESULT_RET(type_result);
    auto type = *type_result;
    if (!collect_only) {
      const bool has_disp = operand.memory_displacement() != 0;
      Tag tag = Tag::MEM_DISP;
      if (has_disp) {
        if (has_base) {
          tag = has_index ? Tag::MEM_DISP_BASE_INDEX : Tag::MEM_DISP_BASE;
        } else {
          tag = has_index ? Tag::MEM_DISP_INDEX : Tag::MEM_DISP;
        }
      } else if (has_base) {
        tag = has_index ? Tag::MEM_BASE_INDEX : Tag::MEM_BASE;
      } else if (has_index) {
        tag = Tag::MEM_INDEX;
      }
      append_byte(out, static_cast<std::uint8_t>(tag));
      auto result = write_type(out, type);
      MIRNEXT_RESULT_RET(result);
      if (has_disp || (!has_base && !has_index)) write_int(out, operand.memory_displacement());
      if (has_base) {
        result = write_name_token(out, base->name(), Tag::REG1, false);
        MIRNEXT_RESULT_RET(result);
      }
      if (has_index) {
        result = write_name_token(out, index->name(), Tag::REG1, false);
        MIRNEXT_RESULT_RET(result);
        write_uint(out, static_cast<std::uint64_t>(operand.memory_scale()));
      }
    } else {
      if (has_base) strings_.add_name(base->name());
      if (has_index) strings_.add_name(index->name());
    }
    return static_cast<void *>(nullptr);
  }

  Result<void *> write_instruction(std::vector<std::byte> &out, const Function &function,
                                   const FunctionTables &tables, const Instruction &instruction,
                                   bool collect_only) {
    if (instruction.opcode() == Opcode::Label) {
      return write_label(out, instruction.label_id(), collect_only);
    }
    auto opcode_result = instruction.opcode() == Opcode::Addr
                             ? Result<BinaryOpcode>(BinaryOpcode::Mov)
                             : lower_opcode(instruction.opcode());
    MIRNEXT_RESULT_RET(opcode_result);
    auto opcode = *opcode_result;
    if (!collect_only) write_uint(out, static_cast<std::uint64_t>(opcode));
    for (const Operand &operand : instruction.operands()) {
      auto operand_result = write_operand(out, function, tables, operand, collect_only);
      MIRNEXT_RESULT_RET(operand_result);
    }
    if (instruction.opcode() == Opcode::Ret || instruction.opcode() == Opcode::Call
        || instruction.opcode() == Opcode::Switch) {
      if (!collect_only) append_byte(out, static_cast<std::uint8_t>(Tag::EOI));
    }
    return static_cast<void *>(nullptr);
  }

  const Module &module_;
  StringTable strings_;
  std::unordered_map<const void *, std::string> refs_;
};

std::uint64_t hash_mum(std::uint64_t value, std::uint64_t constant) {
  const std::uint64_t value_hi = value >> 32;
  const std::uint64_t value_lo = static_cast<std::uint32_t>(value);
  const std::uint64_t constant_hi = constant >> 32;
  const std::uint64_t constant_lo = static_cast<std::uint32_t>(constant);
  const std::uint64_t middle = value_lo * constant_hi + value_hi * constant_lo;
  return value_hi * constant_hi + (middle >> 32) + value_lo * constant_lo + (middle << 32);
}

std::uint64_t hash_key_part(const std::byte *data, std::size_t length) {
  std::uint64_t tail = 0;
  for (std::size_t i = 0; i < length; ++i) {
    tail = (tail >> 8) | (std::uint64_t(std::to_integer<std::uint8_t>(data[i])) << 56);
  }
  return tail;
}

std::uint64_t hash_round(std::uint64_t state, std::uint64_t value) {
  constexpr std::uint64_t p1 = 0x65862b62bdf5ef4dull;
  constexpr std::uint64_t p2 = 0x288eea216831e6a7ull;
  state ^= hash_mum(value, p1);
  return state ^ hash_mum(state, p2);
}

std::uint64_t hash_strict(const std::byte *data, std::size_t length, std::uint64_t seed) {
  constexpr std::uint64_t p1 = 0x65862b62bdf5ef4dull;
  constexpr std::uint64_t p2 = 0x288eea216831e6a7ull;
  std::uint64_t result = seed + length;
  for (; length >= 16; length -= 16, data += 16) {
    result ^= hash_mum(hash_key_part(data, 8), p1);
    result ^= hash_mum(hash_key_part(data + 8, 8), p2);
    result ^= hash_mum(result, p1);
  }
  if (length >= 8) {
    result ^= hash_mum(hash_key_part(data, 8), p1);
    length -= 8;
    data += 8;
  }
  if (length != 0) result ^= hash_mum(hash_key_part(data, length), p2);
  return hash_round(result, result);
}

class BinaryReducer {
public:
  std::vector<std::byte> encode(const std::vector<std::byte> &input) {
    out_.clear();
    append_byte(out_, 'M');
    append_byte(out_, 'I');
    append_byte(out_, 'R');
    check_hash_ = 42;
    for (std::byte byte : input) put(byte);
    encode_buffer();
    write_hash(check_hash_);
    return out_;
  }

private:
  struct Entry {
    std::uint32_t pos = 0;
    std::uint32_t num = 0;
    std::uint32_t next = UINT32_MAX;
    std::uint32_t head = UINT32_MAX;
  };

  static constexpr std::uint32_t start_len = 4;
  static constexpr std::uint32_t buffer_len = 1u << 18;
  static constexpr std::uint32_t table_size = buffer_len / 4;
  static constexpr std::uint32_t max_symbol_len = 2047;
  static constexpr std::uint32_t symbol_tag_len = 3;
  static constexpr std::uint32_t ref_tag_len = 8 - symbol_tag_len;
  static constexpr std::uint32_t symbol_tag_long = (1u << symbol_tag_len) - 1;
  static constexpr std::uint32_t ref_tag_long = (1u << ref_tag_len) - 1;

  void put(std::byte byte) {
    if (buffer_.size() >= buffer_len) {
      encode_buffer();
      buffer_.clear();
    }
    buffer_.push_back(byte);
  }

  void write_reduce_uint(std::uint32_t value) {
    int bytes = 1;
    for (; bytes <= 4 && value >= (1u << (7 * bytes)); ++bytes) {}
    append_byte(out_, static_cast<std::uint8_t>((1u << (8 - bytes))
                                                | ((value >> ((bytes - 1) * 8)) & 0xffu)));
    for (int i = 2; i <= bytes; ++i) {
      append_byte(out_, static_cast<std::uint8_t>((value >> ((bytes - i) * 8)) & 0xffu));
    }
  }

  void write_hash(std::uint64_t value) {
    append_byte(out_, 0);
    for (std::size_t i = 0; i < sizeof(value); ++i) {
      append_byte(out_, static_cast<std::uint8_t>((value >> (i * 8)) & 0xffu));
    }
  }

  std::uint32_t ref_offset_size(std::uint32_t offset) const {
    if (offset < (1u << 7)) return 1;
    if (offset < (1u << 14)) return 2;
    if (offset < (1u << 21)) return 3;
    return 4;
  }

  std::uint32_t ref_size(std::uint32_t length, std::uint32_t offset) const {
    length -= start_len - 1;
    return (length < ref_tag_long ? 0 : ref_offset_size(length)) + ref_offset_size(offset);
  }

  void reset_table() {
    for (std::uint32_t i = 0; i < table_.size(); ++i) {
      table_[i].next = i + 1;
      table_[i].head = UINT32_MAX;
    }
    table_.back().next = UINT32_MAX;
    free_entry_ = 0;
  }

  std::uint32_t get_new_entry() {
    const std::uint32_t result = free_entry_;
    if (result != UINT32_MAX) free_entry_ = table_[result].next;
    return result;
  }

  void flush_symbol(std::uint32_t ref_tag) {
    const std::uint32_t length = static_cast<std::uint32_t>(symbol_.size());
    if (length == 0 && ref_tag == 0) return;
    append_byte(out_, static_cast<std::uint8_t>(
                          ((length < symbol_tag_long ? length : symbol_tag_long) << ref_tag_len)
                          | ref_tag));
    if (length >= symbol_tag_long) write_reduce_uint(length);
    out_.insert(out_.end(), symbol_.begin(), symbol_.end());
    symbol_.clear();
  }

  void output_byte(std::uint32_t pos) {
    if (symbol_.size() + 1 > max_symbol_len) flush_symbol(0);
    symbol_.push_back(buffer_[pos]);
  }

  void output_ref(std::uint32_t offset, std::uint32_t length) {
    length -= start_len - 1;
    const std::uint32_t ref_tag = length < ref_tag_long ? length : ref_tag_long;
    flush_symbol(ref_tag);
    if (length >= ref_tag_long) write_reduce_uint(length);
    write_reduce_uint(offset);
  }

  std::uint32_t find_longest(std::uint32_t pos, std::uint32_t *dict_pos) {
    if (pos + start_len > buffer_.size()) return 0;
    const std::uint64_t hash = hash_strict(&buffer_[pos], start_len, 24) % table_size;
    Entry *best = nullptr;
    std::uint32_t best_length = 0;
    std::uint32_t best_ref_size = 0;
    for (std::uint32_t current = table_[hash].head; current != UINT32_MAX;) {
      Entry &entry = table_[current];
      const std::uint32_t next = entry.next;
      const std::uint32_t bound
          = std::min<std::uint32_t>(static_cast<std::uint32_t>(buffer_.size()) - pos,
                                    pos - entry.pos);
      if (bound >= start_len) {
        std::uint32_t length = 0;
        for (; length < bound; ++length) {
          if (buffer_[entry.pos + length] != buffer_[pos + length]) break;
        }
        if (length >= start_len) {
          const std::uint32_t offset = current_num_ - entry.num;
          if (best == nullptr) {
            best = &entry;
            best_length = length;
            best_ref_size = ref_size(length, offset);
          } else {
            const std::uint32_t candidate_ref_size = ref_size(length, offset);
            if (best_length + candidate_ref_size < length + best_ref_size) {
              best = &entry;
              best_length = length;
              best_ref_size = candidate_ref_size;
            }
          }
        }
      }
      current = next;
    }
    if (best == nullptr) return 0;
    *dict_pos = best->num;
    return best_length;
  }

  void add_dict(std::uint32_t pos) {
    if (pos + start_len > buffer_.size()) return;
    const std::uint64_t hash = hash_strict(&buffer_[pos], start_len, 24) % table_size;
    std::uint32_t current = get_new_entry();
    if (current == UINT32_MAX) {
      std::uint32_t previous = UINT32_MAX;
      current = table_[hash].head;
      while (current != UINT32_MAX && table_[current].next != UINT32_MAX) {
        previous = current;
        current = table_[current].next;
      }
      if (current == UINT32_MAX) return;
      if (previous != UINT32_MAX) {
        table_[previous].next = table_[current].next;
      } else {
        table_[hash].head = table_[current].next;
      }
    }
    table_[current].pos = pos;
    table_[current].num = current_num_++;
    table_[current].next = table_[hash].head;
    table_[hash].head = current;
  }

  void encode_buffer() {
    if (buffer_.empty()) return;
    check_hash_ = hash_strict(buffer_.data(), buffer_.size(), check_hash_);
    current_num_ = 0;
    symbol_.clear();
    reset_table();
    for (std::uint32_t pos = 0; pos < buffer_.size();) {
      std::uint32_t dict_pos = 0;
      const std::uint32_t dict_length = find_longest(pos, &dict_pos);
      const std::uint32_t base = current_num_;
      if (dict_length == 0) {
        output_byte(pos);
        add_dict(pos);
        ++pos;
        continue;
      }
      output_ref(base - dict_pos, dict_length);
      add_dict(pos);
      pos += dict_length;
    }
    flush_symbol(0);
  }

  std::vector<std::byte> out_;
  std::vector<std::byte> buffer_;
  std::vector<std::byte> symbol_;
  std::vector<Entry> table_ = std::vector<Entry>(table_size);
  std::uint64_t check_hash_ = 42;
  std::uint32_t free_entry_ = 0;
  std::uint32_t current_num_ = 0;
};

} // namespace

Result<std::vector<std::byte>> encode_binary(const Module &module) {
  if (module.error()) return *module.error();
  ModuleWriter writer(module);
  auto raw_result = writer.write_raw();
  MIRNEXT_RESULT_RET(raw_result);
  auto raw = *raw_result;
#ifdef MIR_NO_BIN_COMPRESSION
  return raw;
#else
  BinaryReducer reducer;
  return reducer.encode(raw);
#endif
}

Result<std::vector<std::byte>> Module::encode_binary() const {
  return mirnext::encode_binary(*this);
}

} // namespace mirnext

