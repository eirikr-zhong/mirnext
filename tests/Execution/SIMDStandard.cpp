/* Licensed under the MIT License. Standard SIMD128 operation differential tests. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/IR/Vector.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
struct StandardCase {
  Opcode opcode;
  Type source, destination;
  std::string name;
  unsigned count;
};
Value standard_operation (MIRBuilder &b, Opcode opcode, Type destination, Value a, Value c,
                          Value d) {
  switch (opcode) {
  case Opcode::VMul: return b.create_mul (a, c);
  case Opcode::VDiv: return b.create_div (a, c);
  case Opcode::VNeg: return b.create_neg (a);
  case Opcode::VAbs: return b.create_abs (a);
  case Opcode::VShl: return b.create_shl (a, c);
  case Opcode::VShr: return b.create_shr (a, c);
  case Opcode::VMin: return b.create_min (a, c);
  case Opcode::VMax: return b.create_max (a, c);
  case Opcode::VAddSat: return b.create_add_sat (a, c);
  case Opcode::VSubSat: return b.create_sub_sat (a, c);
  case Opcode::VAvgr: return b.create_avgr (a, c);
  case Opcode::VPopcnt: return b.create_popcount (a);
  case Opcode::VAndNot: return b.create_andnot (a, c);
  case Opcode::VBitcast: return b.create_bitcast (a, destination);
  case Opcode::VBitselect: return b.create_bitselect (a, c, d);
  case Opcode::VAnyTrue: return b.create_any_true (a);
  case Opcode::VAllTrue: return b.create_all_true (a);
  case Opcode::VBitmask: return b.create_bitmask (a);
  case Opcode::VSwizzle: return b.create_swizzle (a, c);
  case Opcode::VExtendLow: return b.create_extend (a, VectorHalf::Low);
  case Opcode::VExtendHigh: return b.create_extend (a, VectorHalf::High);
  case Opcode::VNarrowSat: return b.create_narrow_sat (a, c, destination);
  case Opcode::VExtmulLow: return b.create_extmul (a, c, VectorHalf::Low);
  case Opcode::VExtmulHigh: return b.create_extmul (a, c, VectorHalf::High);
  case Opcode::VExtaddPairwise: return b.create_extadd_pairwise (a);
  case Opcode::VDot: return b.create_dot (a, c);
  case Opcode::VQ15MulrSat: return b.create_q15mulr_sat (a, c);
  case Opcode::VSqrt: return b.create_sqrt (a);
  case Opcode::VPMin: return b.create_pmin (a, c);
  case Opcode::VPMax: return b.create_pmax (a, c);
  case Opcode::VCeil: return b.create_ceil (a);
  case Opcode::VFloor: return b.create_floor (a);
  case Opcode::VTrunc: return b.create_trunc (a);
  case Opcode::VNearest: return b.create_nearest (a);
  case Opcode::VConvert: return b.create_convert (a, destination);
  case Opcode::VConvertLow: return b.create_convert_low (a, destination);
  case Opcode::VConvertZero: return b.create_convert_zero (a, destination);
  case Opcode::VTruncSat: return b.create_trunc_sat (a, destination);
  case Opcode::VTruncSatZero: return b.create_trunc_sat_zero (a, destination);
  default: return {};
  }
}
std::vector<StandardCase> standard_cases () {
  std::vector<StandardCase> cases;
  for (unsigned op = static_cast<unsigned> (Opcode::VMul);
       op <= static_cast<unsigned> (Opcode::VTruncSatZero); ++op) {
    auto code = static_cast<Opcode> (op);
    auto descriptor = instruction_descriptor (code).value ();
    for (unsigned element = 0; element < 10; ++element) {
      unsigned width = element < 8 ? 1u << (element / 2) : element == 8 ? 4 : 8;
      auto source = Type::vector128 (static_cast<Type::VectorElement> (element), 16 / width);
      for (unsigned output = 0; output < 11; ++output) {
        unsigned w = output < 8 ? 1u << (output / 2) : output == 8 ? 4 : 8;
        auto dest
          = output == 10
              ? (code == Opcode::VBitmask ? Type (Type::Kind::U32) : Type (Type::Kind::Bool))
              : Type::vector128 (static_cast<Type::VectorElement> (output), 16 / w);
        unsigned count = descriptor.roles.size () - 1;
        std::array<Type, 3> inputs{source, source, source};
        if (code == Opcode::VShl || code == Opcode::VShr) inputs[1] = Type::i64 ();
        if (!ir::detail::vector_types (code, dest, std::span (inputs).first (count))) continue;
        cases.push_back ({code, source, dest,
                          std::string (descriptor.name) + "_" + std::to_string (element) + "_"
                            + std::to_string (output),
                          count});
      }
    }
  }
  return cases;
}
ScalarValue standard_input (Type type, unsigned sample, unsigned operand) {
  std::array<std::byte, 16> data{};
  std::mt19937_64 rng (0x128000 + sample * 11 + operand * 777);
  for (auto &v : data) v = std::byte (rng () & 255);
  unsigned width = 16 / type.vector_lanes ();
  for (unsigned lane = 0; lane < type.vector_lanes (); ++lane) {
    std::uint64_t x = rng ();
    if (type.vector_lane_type ().is_float ()) {
      const double values[]{0.,          -0.,         1.5,          -2.5,     0.5,       -0.5,
                            2147483648., 4294967296., -2147483649., INFINITY, -INFINITY, NAN,
                            1e-38,       -1e-38,      1.999,        -1.999};
      auto value = values[(sample + lane + operand * 3) % std::size (values)];
      if (width == 4)
        x = std::bit_cast<std::uint32_t> (static_cast<float> (value));
      else
        x = std::bit_cast<std::uint64_t> (value);
      if (sample == 15 && lane == 0)
        x = width == 4 ? UINT64_C (0x7fa12345) : UINT64_C (0x7ff0123456789abc);
    } else if (sample < 8) {
      const auto sign = UINT64_C (1) << (width * 8 - 1);
      const std::uint64_t values[]{0, 1, UINT64_MAX, sign, sign - 1, sign + 1, 0x7fff, 0x8000};
      x = values[(sample + lane + operand) % 8];
    }
    std::memcpy (data.data () + lane * width, &x, width);
  }
  return ScalarValue::vector (type, data).value ();
}
void standard_check (const ScalarValue &actual, const ScalarValue &expected) {
  REQUIRE (actual.type () == expected.type ());
  if (!actual.type ().is_vector ()) {
    CHECK (actual.as_u64 ().value () == expected.as_u64 ().value ());
    return;
  }
  auto type = actual.type ();
  unsigned width = 16 / type.vector_lanes ();
  for (unsigned lane = 0; lane < type.vector_lanes (); ++lane) {
    auto a = actual.as_vector ().subspan (lane * width, width),
         e = expected.as_vector ().subspan (lane * width, width);
    // Arithmetic permits quiet NaN payload variation; dedicated semantic tests check exact bit
    // operations.
    if (type.vector_lane_type ().is_float ()) {
      if (width == 4) {
        float x, y;
        std::memcpy (&x, a.data (), 4);
        std::memcpy (&y, e.data (), 4);
        if (std::isnan (x) && std::isnan (y)) continue;
      } else {
        double x, y;
        std::memcpy (&x, a.data (), 8);
        std::memcpy (&y, e.data (), 8);
        if (std::isnan (x) && std::isnan (y)) continue;
      }
    }
    CHECK (std::ranges::equal (a, e));
  }
}
}  // namespace
TEST_CASE ("standard SIMD128 matrix builds, roundtrips and executes every new opcode") {
  auto cases = standard_cases ();
  REQUIRE (cases.size () > 150);
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("standard");
  for (auto &item : cases) {
    Signature signature{{item.destination}, {item.source}};
    for (unsigned n = 1; n < item.count; ++n)
      signature.parameters.push_back (
        item.opcode == Opcode::VShl || item.opcode == Opcode::VShr ? Type::i64 () : item.source);
    b.create_function (module, item.name, signature);
    b.create_return (standard_operation (b, item.opcode, item.destination, b.argument (0),
                                         item.count > 1 ? b.argument (1) : Value{},
                                         item.count > 2 ? b.argument (2) : Value{}));
  }
  auto finished = b.finish ();
  INFO ((finished.has_value () ? "" : finished.error ().message));
  REQUIRE (finished.has_value ());
  auto text = serialization::write_text (program);
  REQUIRE (text.has_value ());
  auto parsed = serialization::parse_text (text.value ());
  INFO ((parsed.has_value () ? "" : parsed.error ().message));
  REQUIRE (parsed.has_value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto bytes = serialization::write_binary (program, encoding);
    REQUIRE (bytes.has_value ());
    auto read = serialization::read_binary (bytes.value (), {encoding});
    INFO ((read.has_value () ? "" : read.error ().message));
    REQUIRE (read.has_value ());
    REQUIRE (serialization::write_binary (read.value (), encoding).value () == bytes.value ());
  }
  auto baseline = ExecutionSession::create (program, {.mode = ExecutionMode::Interpreter});
  REQUIRE (baseline.has_value ());
  for (unsigned engine = 1; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (engine);
      CAPTURE (level);
      ExecutionOptions options;
      options.mode = ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      auto loaded = ExecutionSession::create (program, options);
      INFO ((loaded.has_value () ? "" : loaded.error ().message));
      REQUIRE (loaded.has_value ());
      for (auto &item : cases) {
        CAPTURE (item.name);
        auto reference = baseline.value ().find_function ("standard", item.name);
        REQUIRE (reference.has_value ());
        auto compiled = loaded.value ().find_function ("standard", item.name);
        REQUIRE (compiled.has_value ());
        for (unsigned sample = 0; sample < 16; ++sample) {
          CAPTURE (sample);
          std::vector<ExecutionArgument> inputs{standard_input (item.source, sample, 0)};
          for (unsigned n = 1; n < item.count; ++n)
            inputs.emplace_back (
              item.opcode == Opcode::VShl || item.opcode == Opcode::VShr
                ? ScalarValue (std::uint64_t (sample == 15 ? UINT64_MAX : sample * 17))
                : standard_input (item.source, sample, n));
          auto expected = baseline.value ().call (reference.value (), inputs);
          INFO ((expected.has_value () ? "" : expected.error ().message));
          REQUIRE (expected.has_value ());
          auto actual = loaded.value ().call (compiled.value (), inputs);
          INFO ((actual.has_value () ? "" : actual.error ().message));
          REQUIRE (actual.has_value ());
          standard_check (actual.value ()[0], expected.value ()[0]);
        }
      }
    }
}

#include <filesystem>
#include <fstream>
#include <sstream>
namespace {
Type wasm_shape (std::string_view text, bool sign = true) {
  auto end = text.find ('x');
  unsigned bits = std::stoul (std::string (text.substr (1, end - 1)));
  if (text.front () == 'f')
    return Type::vector128 (bits == 32 ? Type::VectorElement::F32 : Type::VectorElement::F64,
                            128 / bits);
  return ir::detail::vector_integer (bits, sign);
}
struct OfficialInstruction {
  std::string name, operation;
  Opcode opcode = Opcode::VMov;
  Type source = ir::detail::vector_integer (8, true), destination = source;
  unsigned count = 1, width = 0;
  bool memory = false;
};
OfficialInstruction official_instruction (std::string_view name) {
  OfficialInstruction i;
  i.name = name;
  auto dot = name.find ('.');
  i.operation = name.substr (dot + 1);
  auto &op = i.operation;
  bool sign = op.find ("_u") == std::string::npos;
  if (name.substr (0, dot) != "v128") i.source = wasm_shape (name.substr (0, dot), sign);
  i.destination = i.source;
  auto binary = [&] (Opcode code) {
    i.opcode = code;
    i.count = 2;
  };
  if (op == "const") return i;
  if (op.starts_with ("load") || op.starts_with ("store")) {
    i.memory = true;
    i.width = op == "load" || op == "store"
                ? 16
                : std::stoul (op.substr (op.starts_with ("load") ? 4 : 5)) / 8;
    if (op.find ('x') != std::string::npos) {
      auto width = std::stoul (op.substr (4));
      i.destination = ir::detail::vector_integer (width * 2, sign);
    } else
      i.destination = ir::detail::vector_integer (i.width == 16 ? 8 : i.width * 8, false);
    return i;
  }
  if (op.starts_with ("extract_lane")) {
    i.opcode = Opcode::VExtract;
    i.destination = i.source.vector_lane_type ();
    return i;
  }
  if (op == "replace_lane") {
    binary (Opcode::VInsert);
    return i;
  }
  if (op == "splat") {
    i.opcode = Opcode::VSplat;
    return i;
  }
  if (op == "shuffle") {
    binary (Opcode::VShuffle);
    return i;
  }
  if (op == "swizzle") {
    binary (Opcode::VSwizzle);
    return i;
  }
  if (op == "not") {
    i.opcode = Opcode::VXor;
    return i;
  }
  if (op == "bitselect") {
    i.opcode = Opcode::VBitselect;
    i.count = 3;
    return i;
  }
  if (op == "any_true" || op == "all_true" || op == "bitmask") {
    i.opcode = op == "any_true"   ? Opcode::VAnyTrue
               : op == "all_true" ? Opcode::VAllTrue
                                  : Opcode::VBitmask;
    i.destination = op == "bitmask" ? Type (Type::Kind::U32) : Type (Type::Kind::Bool);
    return i;
  }
  if (op.starts_with ("extend_") || op.starts_with ("extmul_")
      || op.starts_with ("extadd_pairwise_") || op.starts_with ("dot_")) {
    auto source = op.find ("_i");
    i.source = wasm_shape (op.substr (source + 1, op.find ('_', source + 2) - source - 1), sign);
    i.opcode
      = op.starts_with ("extend_")
          ? (op.find ("_low_") != std::string::npos ? Opcode::VExtendLow : Opcode::VExtendHigh)
        : op.starts_with ("extmul_")
          ? (op.find ("_low_") != std::string::npos ? Opcode::VExtmulLow : Opcode::VExtmulHigh)
        : op.starts_with ("dot_") ? Opcode::VDot
                                  : Opcode::VExtaddPairwise;
    if (op.starts_with ("extmul_") || op.starts_with ("dot_")) i.count = 2;
    return i;
  }
  if (op.starts_with ("narrow_")) {
    i.source = ir::detail::vector_widened (
      ir::detail::vector_integer (128 / i.destination.vector_lanes (), true));
    binary (Opcode::VNarrowSat);
    return i;
  }
  if (op.starts_with ("trunc_sat_")) {
    i.source = wasm_shape (op.substr (10, 5));
    i.opcode = op.ends_with ("_zero") ? Opcode::VTruncSatZero : Opcode::VTruncSat;
    return i;
  }
  if (op.starts_with ("convert_") || op.starts_with ("promote_") || op.starts_with ("demote_")) {
    auto position = op.find ("i32x4");
    if (position == std::string::npos) position = op.find ("f32x4");
    if (position == std::string::npos) position = op.find ("f64x2");
    i.source = wasm_shape (op.substr (position, 5), sign);
    i.opcode = op.starts_with ("demote_")               ? Opcode::VConvertZero
               : op.find ("_low_") != std::string::npos ? Opcode::VConvertLow
                                                        : Opcode::VConvert;
    return i;
  }
  if (op == "abs")
    i.opcode = Opcode::VAbs;
  else if (op == "neg")
    i.opcode = Opcode::VNeg;
  else if (op == "popcnt")
    i.opcode = Opcode::VPopcnt;
  else if (op == "sqrt")
    i.opcode = Opcode::VSqrt;
  else if (op == "ceil")
    i.opcode = Opcode::VCeil;
  else if (op == "floor")
    i.opcode = Opcode::VFloor;
  else if (op == "trunc")
    i.opcode = Opcode::VTrunc;
  else if (op == "nearest")
    i.opcode = Opcode::VNearest;
  else if (op == "and")
    binary (Opcode::VAnd);
  else if (op == "or")
    binary (Opcode::VOr);
  else if (op == "xor")
    binary (Opcode::VXor);
  else if (op == "andnot")
    binary (Opcode::VAndNot);
  else if (op == "add")
    binary (Opcode::VAdd);
  else if (op == "sub")
    binary (Opcode::VSub);
  else if (op == "mul")
    binary (Opcode::VMul);
  else if (op == "div")
    binary (Opcode::VDiv);
  else if (op.starts_with ("min"))
    binary (Opcode::VMin);
  else if (op.starts_with ("max"))
    binary (Opcode::VMax);
  else if (op == "pmin")
    binary (Opcode::VPMin);
  else if (op == "pmax")
    binary (Opcode::VPMax);
  else if (op == "shl")
    binary (Opcode::VShl);
  else if (op.starts_with ("shr"))
    binary (Opcode::VShr);
  else if (op.starts_with ("add_sat"))
    binary (Opcode::VAddSat);
  else if (op.starts_with ("sub_sat"))
    binary (Opcode::VSubSat);
  else if (op == "avgr_u")
    binary (Opcode::VAvgr);
  else if (op == "q15mulr_sat_s")
    binary (Opcode::VQ15MulrSat);
  else if (op == "eq" || op == "ne" || op == "lt" || op == "le" || op == "gt" || op == "ge"
           || op == "lt_s" || op == "lt_u" || op == "le_s" || op == "le_u" || op == "gt_s"
           || op == "gt_u" || op == "ge_s" || op == "ge_u") {
    binary (op == "eq"              ? Opcode::VEq
            : op == "ne"            ? Opcode::VNe
            : op.starts_with ("lt") ? Opcode::VLt
            : op.starts_with ("le") ? Opcode::VLe
            : op.starts_with ("gt") ? Opcode::VGt
                                    : Opcode::VGe);
    i.destination = i.source.vector_mask_type ();
  } else {
    INFO (name);
    REQUIRE_MESSAGE (false, "standard instruction has no explicit MIR mapping");
  }
  return i;
}
Value official_body (MIRBuilder &b, const OfficialInstruction &i) {
  if (i.operation == "const") return b.constant_vector (i.destination, std::array<std::byte, 16>{});
  auto a = b.argument (0), c = i.count > 1 ? b.argument (1) : Value{},
       d = i.count > 2 ? b.argument (2) : Value{};
  auto op = i.opcode;
  if (i.memory) {
    a = b.argument (0);
    auto type = i.destination;
    auto scalar = type.vector_lane_type ();
    if (i.operation.starts_with ("store")) {
      c = b.argument (1);
      if (i.operation == "store")
        b.create_store (b.memory (type, a), c);
      else
        b.create_store_lane (b.memory (scalar, a), c, type.vector_lanes () - 1);
      return c;
    }
    if (i.operation == "load") return b.create_load (b.memory (type, a));
    if (i.operation.find ('x') != std::string::npos) return b.create_load_extend (type, a);
    if (i.operation.ends_with ("_splat")) return b.create_load_splat (type, b.memory (scalar, a));
    if (i.operation.ends_with ("_zero")) return b.create_load_zero (type, b.memory (scalar, a));
    return b.create_load_lane (b.argument (1), b.memory (scalar, a), type.vector_lanes () - 1);
  }
  if (i.operation == "const") return b.constant_vector (i.destination, std::array<std::byte, 16>{});
  if (i.operation == "not") return b.create_bit_not (a);
  if (op == Opcode::VExtract) return b.create_extract_lane (a, i.source.vector_lanes () - 1);
  if (op == Opcode::VInsert) return b.create_insert_lane (a, c, i.source.vector_lanes () - 1);
  if (op == Opcode::VSplat) return b.create_vector_splat (i.destination, a);
  if (op == Opcode::VShuffle) {
    std::array<unsigned, 16> mask{};
    for (unsigned n = 0; n < 16; ++n) mask[n] = 31 - n;
    return b.create_shuffle (a, c, mask);
  }
  if (op == Opcode::VBitselect) return b.create_bitselect (d, a, c);
  switch (op) {
  case Opcode::VAdd: return b.create_add (a, c);
  case Opcode::VSub: return b.create_sub (a, c);
  case Opcode::VAnd: return b.create_and (a, c);
  case Opcode::VOr: return b.create_or (a, c);
  case Opcode::VXor: return b.create_xor (a, c);
  case Opcode::VEq: return b.create_eq (a, c);
  case Opcode::VNe: return b.create_ne (a, c);
  case Opcode::VLt: return b.create_lt (a, c);
  case Opcode::VLe: return b.create_le (a, c);
  case Opcode::VGt: return b.create_gt (a, c);
  case Opcode::VGe: return b.create_ge (a, c);
  default: return standard_operation (b, op, i.destination, a, c, d);
  }
}
std::vector<OfficialInstruction> official_instructions () {
  std::vector<OfficialInstruction> result;
#define MIRNEXT_STANDARD_SIMD(number, name) result.push_back (official_instruction (name));
#include "../SIMD/Instructions.def"
#undef MIRNEXT_STANDARD_SIMD
  return result;
}
Program official_program (const std::vector<OfficialInstruction> &list) {
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("official");
  for (auto &i : list) {
    Signature signature{{i.destination}, {}};
    if (i.memory) {
      signature.parameters.push_back (Type::pointer ());
      if (i.operation.starts_with ("store") || i.operation.ends_with ("_lane"))
        signature.parameters.push_back (i.destination);
    } else if (i.operation != "const") {
      signature.parameters.push_back (i.opcode == Opcode::VSplat ? i.source.vector_lane_type ()
                                                                 : i.source);
      for (unsigned n = 1; n < i.count; ++n)
        signature.parameters.push_back (
          i.opcode == Opcode::VShl || i.opcode == Opcode::VShr ? Type::i64 ()
          : i.opcode == Opcode::VInsert                        ? i.source.vector_lane_type ()
          : i.opcode == Opcode::VSwizzle ? ir::detail::vector_integer (8, false)
                                         : i.source);
    }
    b.create_function (module, i.name, signature);
    b.create_return (official_body (b, i));
  }
  auto done = b.finish ();
  INFO ((done.has_value () ? "" : done.error ().message));
  REQUIRE (done.has_value ());
  return program;
}
std::vector<std::string> split (std::string_view s, char separator) {
  std::vector<std::string> result;
  std::istringstream stream{std::string (s)};
  std::string part;
  while (std::getline (stream, part, separator)) result.push_back (part);
  return result;
}
std::vector<std::byte> unhex (std::string_view hex) {
  std::vector<std::byte> data;
  for (unsigned n = 0; n < hex.size (); n += 2)
    data.push_back (std::byte (std::stoul (std::string (hex.substr (n, 2)), nullptr, 16)));
  return data;
}
ScalarValue fixture_value (std::string_view text, Type type) {
  auto data = unhex (text.substr (text.find (':') + 1));
  if (!type.is_vector ()) {
    auto bits = std::uint64_t (0);
    std::memcpy (&bits, data.data (), data.size ());
    if (type.is_integer ()) return ScalarValue (bits);
  }
  data.resize (16);
  auto value = ScalarValue::from_storage (type, data);
  REQUIRE (value.has_value ());
  return value.value ();
}
}  // namespace
TEST_CASE ("Core 2.0 has 236 mapped instructions and offline official semantic fixtures") {
  auto list = official_instructions ();
  REQUIRE (list.size () == 236);
  auto program = official_program (list);
  auto text = serialization::write_text (program);
  REQUIRE (text.has_value ());
  REQUIRE (serialization::parse_text (text.value ()).has_value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto bytes = serialization::write_binary (program, encoding);
    REQUIRE (bytes.has_value ());
    REQUIRE (serialization::read_binary (bytes.value (), {encoding}).has_value ());
  }
  struct Fixture {
    std::string name;
    std::vector<ExecutionArgument> arguments;
    ScalarValue expected;
    unsigned nan_mask;
    std::vector<std::byte> memory;
  };
  std::vector<Fixture> fixtures;
  std::ifstream file (std::filesystem::path (__FILE__).parent_path () / "../SIMD/Cases.tsv");
  REQUIRE (file.good ());
  std::string line;
  while (std::getline (file, line)) {
    if (line.empty () || line[0] == '#') continue;
    auto fields = split (line, ';');
    REQUIRE (fields.size () == 5);
    auto item = std::ranges::find (list, fields[0], &OfficialInstruction::name);
    REQUIRE (item != list.end ());
    Fixture f;
    f.name = fields[0];
    f.nan_mask = std::stoul (fields[3]);
    auto values = split (fields[1], ',');
    REQUIRE (values.size () == item->count);
    for (unsigned n = 0; n < values.size (); ++n) {
      if (item->memory) {
        REQUIRE (values[n].starts_with ("memory:"));
        f.memory = unhex (values[n].substr (7));
        f.arguments.emplace_back (
          ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (f.memory.data ())));
        continue;
      }
      auto type = item->source;
      if (item->opcode == Opcode::VSplat)
        type = type.vector_lane_type ();
      else if (item->opcode == Opcode::VShl || item->opcode == Opcode::VShr) {
        if (n == 1) type = Type::i64 ();
      } else if (item->opcode == Opcode::VSwizzle && n == 1)
        type = ir::detail::vector_integer (8, false);
      f.arguments.emplace_back (fixture_value (values[n], type));
    }
    f.expected
      = fixture_value (fields[2],
                       fields[2].substr (0, fields[2].find (':')).find ('x') != std::string::npos
                         ? wasm_shape (fields[2].substr (0, fields[2].find (':')))
                         : item->destination);
    fixtures.push_back (std::move (f));
  }
  REQUIRE (fixtures.size () > 20000);
  for (unsigned engine = 0; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (engine);
      CAPTURE (level);
      ExecutionOptions options;
      options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      options.link.compilation_observer
        = [] (const codegen::CompilationSnapshot &snapshot) -> Result<void> {
        if (snapshot.stage == codegen::CompilationStage::PrepareMachineIR
            && snapshot.function.module_name () == "official") {
          for (auto &reg : snapshot.function.registers ()) CHECK_FALSE (reg.is_memory_backed ());
          for (auto &block : snapshot.function.blocks ())
            for (auto &instruction : block.instructions ()) {
              CHECK (instruction.opcode () != codegen::MachineOpcode (Opcode::Addr));
              CHECK (instruction.opcode () != codegen::MachineOpcode (Opcode::Alloca));
              auto opcode = instruction.opcode ();
              if (auto code = std::get_if<Opcode> (&opcode);
                  code && ir::detail::vector_opcode (*code))
                CHECK ((*code == Opcode::VMov || *code == Opcode::VAdd || *code == Opcode::VSub
                        || *code == Opcode::VAnd || *code == Opcode::VOr || *code == Opcode::VXor));
            }
        }
        return {};
      };
      auto session = ExecutionSession::create (program, options);
      INFO ((session.has_value () ? "" : session.error ().message));
      REQUIRE (session.has_value ());
      for (auto &f : fixtures) {
        CAPTURE (f.name);
        auto function = session.value ().find_function ("official", f.name);
        REQUIRE (function.has_value ());
        auto result = session.value ().call (function.value (), f.arguments);
        INFO ((result.has_value () ? "" : result.error ().message));
        REQUIRE (result.has_value ());
        auto &actual = result.value ()[0];
        if (actual.type ().is_vector ()) {
          unsigned width = 16 / f.expected.type ().vector_lanes ();
          for (unsigned lane = 0; lane < f.expected.type ().vector_lanes (); ++lane) {
            auto a = actual.as_vector ().subspan (lane * width, width),
                 e = f.expected.as_vector ().subspan (lane * width, width);
            if (f.nan_mask & (1u << lane)) {
              if (width == 4) {
                float v;
                std::memcpy (&v, a.data (), 4);
                CHECK (std::isnan (v));
              } else {
                double v;
                std::memcpy (&v, a.data (), 8);
                CHECK (std::isnan (v));
              }
            } else
              CHECK (std::ranges::equal (a, e));
          }
        } else
          CHECK (actual.as_u64 ().value () == f.expected.as_u64 ().value ());
      }
    }
}

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <cfenv>
namespace {
struct GuardedMemory {
  std::byte *data = nullptr;
  std::size_t page = 0;
  GuardedMemory () {
#if defined(_WIN32)
    SYSTEM_INFO info;
    GetSystemInfo (&info);
    page = info.dwPageSize;
    data = static_cast<std::byte *> (
      VirtualAlloc (nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    REQUIRE (data);
    DWORD old = 0;
    REQUIRE (VirtualProtect (data + page, page, PAGE_NOACCESS, &old));
#else
    page = static_cast<std::size_t> (sysconf (_SC_PAGESIZE));
    auto address = mmap (nullptr, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    REQUIRE (address != MAP_FAILED);
    data = static_cast<std::byte *> (address);
    REQUIRE (mprotect (data + page, page, PROT_NONE) == 0);
#endif
  }
  ~GuardedMemory () {
#if defined(_WIN32)
    if (data) VirtualFree (data, 0, MEM_RELEASE);
#else
    if (data) munmap (data, page * 2);
#endif
  }
};
}  // namespace
TEST_CASE ("standard narrow memory operations touch exactly their defined bytes") {
  auto list = official_instructions ();
  std::erase_if (list, [] (auto &i) { return !i.memory; });
  REQUIRE (list.size () == 22);
  auto program = official_program (list);
  GuardedMemory memory;
  for (unsigned engine = 0; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (engine);
      CAPTURE (level);
      ExecutionOptions options;
      options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      auto session = ExecutionSession::create (program, options);
      INFO ((session.has_value () ? "" : session.error ().message));
      REQUIRE (session.has_value ());
      for (auto &i : list) {
        CAPTURE (i.name);
        auto type = i.destination;
        unsigned lane_width = 16 / type.vector_lanes ();
        bool extend = i.operation.find ('x') != std::string::npos;
        unsigned accessed = extend ? 8 : i.width;
        auto address = memory.data + memory.page - accessed;
        std::fill (memory.data, memory.data + memory.page, std::byte (0xcc));
        for (unsigned n = 0; n < accessed; ++n) address[n] = std::byte (n * 37 + 0x81);
        auto argument = standard_input (type, 10, 0);
        auto expected = argument.storage ();
        std::vector<ExecutionArgument> arguments{
          ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (address))};
        if (i.operation.starts_with ("store") || i.operation.ends_with ("_lane"))
          arguments.emplace_back (argument);
        if (i.operation.starts_with ("load")) {
          expected.fill (std::byte (0));
          if (extend)
            for (unsigned n = 0; n < type.vector_lanes (); ++n) {
              unsigned width = lane_width / 2;
              std::uint64_t raw = 0;
              std::memcpy (&raw, address + n * width, width);
              if (ir::detail::vector_signed (type)) {
                auto sign = UINT64_C (1) << (width * 8 - 1);
                raw = (raw ^ sign) - sign;
              }
              std::memcpy (expected.data () + n * lane_width, &raw, lane_width);
            }
          else if (i.operation.ends_with ("_splat")) {
            for (unsigned n = 0; n < type.vector_lanes (); ++n)
              std::memcpy (expected.data () + n * lane_width, address, lane_width);
          } else if (i.operation.ends_with ("_lane")) {
            expected = argument.storage ();
            std::memcpy (expected.data () + 16 - lane_width, address, lane_width);
          } else
            std::memcpy (expected.data (), address, accessed);
        }
        auto function = session.value ().find_function ("official", i.name);
        REQUIRE (function.has_value ());
        auto result = session.value ().call (function.value (), arguments);
        INFO ((result.has_value () ? "" : result.error ().message));
        REQUIRE (result.has_value ());
        CHECK (std::ranges::equal (result.value ()[0].as_vector (), expected));
        if (i.operation.starts_with ("store")) {
          auto source = argument.as_vector ().subspan (i.operation == "store" ? 0 : 16 - lane_width,
                                                       accessed);
          CHECK (std::ranges::equal (std::span (address, accessed), source));
          CHECK (std::ranges::all_of (std::span (address - 32, 32),
                                      [] (auto v) { return v == std::byte (0xcc); }));
        }
      }
    }
}
TEST_CASE ("float masks preserve payload bits and min max canonicalize only as specified") {
  auto list = official_instructions ();
  std::erase_if (list, [] (auto &i) {
    return !i.source.vector_lane_type ().is_float ()
           || !(i.operation == "abs" || i.operation == "neg" || i.operation == "min"
                || i.operation == "max" || i.operation == "pmin" || i.operation == "pmax"
                || i.operation == "nearest");
  });
  auto program = official_program (list);
  for (unsigned engine = 0; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (engine);
      CAPTURE (level);
      ExecutionOptions options;
      options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      auto session = ExecutionSession::create (program, options);
      REQUIRE (session.has_value ());
      for (unsigned width : {4u, 8u}) {
        auto type
          = Type::vector128 (width == 4 ? Type::VectorElement::F32 : Type::VectorElement::F64,
                             16 / width);
        const auto sign = UINT64_C (1) << (width * 8 - 1);
        const auto canonical = width == 4 ? UINT64_C (0x7fc00000) : UINT64_C (0x7ff8000000000000);
        const auto one = width == 4 ? UINT64_C (0x3f800000) : UINT64_C (0x3ff0000000000000);
        const auto three_halves
          = width == 4 ? UINT64_C (0x3fc00000) : UINT64_C (0x3ff8000000000000);
        const auto five_halves = width == 4 ? UINT64_C (0x40200000) : UINT64_C (0x4004000000000000);
        const std::array<std::uint64_t, 4> first{width == 4 ? UINT64_C (0xffa12345)
                                                            : UINT64_C (0xfff0123456789abc),
                                                 sign, 0, three_halves},
          second{one, 0, sign, five_halves};
        auto value = [&] (const auto &bits, unsigned offset) {
          std::array<std::byte, 16> bytes{};
          for (unsigned n = 0; n < type.vector_lanes (); ++n)
            std::memcpy (bytes.data () + width * n, &bits[offset + n], width);
          return ScalarValue::vector (type, bytes).value ();
        };
        for (unsigned offset = 0; offset < 4; offset += type.vector_lanes ()) {
          for (auto &i : list) {
            if (i.source != type || i.operation == "nearest") continue;
            CAPTURE (i.name);
            auto expected = first;
            if (i.operation == "abs")
              for (auto &v : expected) v &= sign - 1;
            if (i.operation == "neg")
              for (auto &v : expected) v ^= sign;
            if (i.operation == "min") expected = {canonical, sign, sign, three_halves};
            if (i.operation == "max") expected = {canonical, 0, 0, five_halves};
            if (i.operation == "pmax") expected[3] = second[3];
            std::vector<ExecutionArgument> inputs{value (first, offset)};
            if (i.count == 2) inputs.emplace_back (value (second, offset));
            auto f = session.value ().find_function ("official", i.name);
            REQUIRE (f.has_value ());
            auto result = session.value ().call (f.value (), inputs);
            REQUIRE (result.has_value ());
            CHECK (std::ranges::equal (result.value ()[0].as_vector (),
                                       value (expected, offset).as_vector ()));
          }
        }
        const auto half = width == 4 ? UINT64_C (0x3f000000) : UINT64_C (0x3fe0000000000000);
        const auto two = width == 4 ? UINT64_C (0x40000000) : UINT64_C (0x4000000000000000);
        const std::array<std::uint64_t, 4> inputs{half, three_halves, half | sign,
                                                  five_halves | sign},
          expected{0, two, sign, two | sign};
        auto f = session.value ().find_function ("official",
                                                 width == 4 ? "f32x4.nearest" : "f64x2.nearest");
        REQUIRE (f.has_value ());
        auto previous = std::fegetround ();
        for (unsigned offset = 0; offset < 4; offset += type.vector_lanes ())
          for (auto rounding : {FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO, FE_TONEAREST}) {
            const std::array<ExecutionArgument, 1> args{value (inputs, offset)};
            REQUIRE (std::fesetround (rounding) == 0);
            auto result = session.value ().call (f.value (), args);
            std::fesetround (previous);
            REQUIRE (result.has_value ());
            CHECK (std::ranges::equal (result.value ()[0].as_vector (),
                                       value (expected, offset).as_vector ()));
          }
      }
    }
}
TEST_CASE (
  "standard SIMD rejects unsupported lanes and conversions before appending instructions") {
  for (unsigned scenario = 0; scenario < 21; ++scenario) {
    CAPTURE (scenario);
    Program program;
    MIRBuilder b (program);
    auto m = b.create_module ("errors");
    auto i8 = ir::detail::vector_integer (8, true), i16 = ir::detail::vector_integer (16, true),
         i32 = ir::detail::vector_integer (32, true), i64 = ir::detail::vector_integer (64, true),
         u16 = ir::detail::vector_integer (16, false), f32 = wasm_shape ("f32x4"),
         f64 = wasm_shape ("f64x2");
    auto f = b.create_function (m, "invalid", {{}, {i8, i16, i32, i64, u16, f32, f64}});
    auto a = b.argument (0), c = b.argument (1), d = b.argument (2), e = b.argument (3),
         u = b.argument (4), x = b.argument (5), y = b.argument (6);
    auto before = dump (*f);
    switch (scenario) {
    case 0: b.create_mul (a, a); break;
    case 1: b.create_div (d, d); break;
    case 2: b.create_abs (u); break;
    case 3: b.create_min (e, e); break;
    case 4: b.create_add_sat (d, d); break;
    case 5: b.create_avgr (c, c); break;
    case 6: b.create_popcount (c); break;
    case 7: b.create_swizzle (a, a); break;
    case 8: b.create_bitselect (a, c, d); break;
    case 9: b.create_extend (e, VectorHalf::Low); break;
    case 10: b.create_extmul (x, x, VectorHalf::Low); break;
    case 11: b.create_narrow_sat (u, u, i8); break;
    case 12: b.create_dot (u, u); break;
    case 13: b.create_q15mulr_sat (d, d); break;
    case 14: b.create_convert (x, d.type ()); break;
    case 15: b.create_convert_low (e, f64); break;
    case 16: b.create_trunc_sat (y, i32); break;
    case 17: b.create_trunc_sat_zero (x, i32); break;
    case 18: b.create_extend (c, static_cast<VectorHalf> (17)); break;
    case 19: b.create_load_lane (d, b.memory (Type::i64 (), b.constant_i64 (0)), 0); break;
    case 20:
      b.create_store_lane (b.memory (Type (Type::Kind::I32), b.constant_i64 (0)), d, 4);
      break;
    }
    CHECK_FALSE (b.status ().has_value ());
    CHECK (dump (*f) == before);
    auto error = b.status ().error ().message;
    b.create_return ();
    CHECK (dump (*f) == before);
    CHECK (b.status ().error ().message == error);
    CHECK_FALSE (b.finish ().has_value ());
  }
}
TEST_CASE ("new native vector definitions survive calls and allocation spills") {
  for (auto type : {wasm_shape ("i16x8"), wasm_shape ("i64x2")}) {
    const bool narrow = type.vector_lanes () == 8;
    Program program;
    MIRBuilder b (program);
    auto m = b.create_module ("pressure");
    b.create_function (m, "combine", {{type}, {type, type}});
    auto product = [&] (Value a, Value c) {
      return narrow ? b.create_q15mulr_sat (a, c) : b.create_mul (a, c);
    };
    auto first = product (b.argument (0), b.argument (1));
    b.create_return (narrow ? b.create_add_sat (b.argument (0), first)
                            : b.create_add (b.argument (0), first));
    auto callee = m->find_symbol ("combine");
    REQUIRE (callee.has_value ());
    Signature signature{{type}, std::vector<Type> (24, type)};
    b.create_function (m, "caller", signature);
    std::vector<Value> live;
    for (unsigned n = 0; n < 24; ++n) live.push_back (b.create_neg (b.argument (n)));
    auto result = b.create_call (callee.get (), {live[0], live[1]}).result (0);
    for (unsigned n = 2; n < 24; ++n)
      result = b.create_add (result, product (live[n], live[n - 1]));
    b.create_return (result);
    REQUIRE (b.finish ().has_value ());
    std::vector<ExecutionArgument> arguments;
    for (unsigned n = 0; n < 24; ++n) arguments.emplace_back (standard_input (type, 8 + n, 0));
    auto reference = ExecutionSession::create (program, {.mode = ExecutionMode::Interpreter});
    REQUIRE (reference.has_value ());
    auto rf = reference.value ().find_function ("pressure", "caller");
    REQUIRE (rf.has_value ());
    auto expected = reference.value ().call (rf.value (), arguments);
    REQUIRE (expected.has_value ());
    for (unsigned mode = 0; mode < 4; ++mode)
      for (unsigned level = 0; level < 4; ++level) {
        ExecutionOptions options;
        options.optimization = transforms::OptimizationOptions{
          .level = static_cast<transforms::OptimizationLevel> (level)};
        options.link.compilation = mode == 1   ? JITCompilation::Lazy
                                   : mode == 2 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
        if (mode == 3)
          options.select_engine = [] (const Function &f) -> Result<ExecutionEngine> {
            return f.symbol ().name () == "combine" ? ExecutionEngine::Interpreter
                                                    : ExecutionEngine::EagerJIT;
          };
        options.link.compilation_observer
          = [] (const codegen::CompilationSnapshot &snapshot) -> Result<void> {
          if (snapshot.stage == codegen::CompilationStage::PrepareMachineIR)
            for (auto &r : snapshot.function.registers ()) CHECK_FALSE (r.is_memory_backed ());
          return {};
        };
        auto session = ExecutionSession::create (program, options);
        INFO ((session.has_value () ? "" : session.error ().message));
        REQUIRE (session.has_value ());
        auto f = session.value ().find_function ("pressure", "caller");
        REQUIRE (f.has_value ());
        auto actual = session.value ().call (f.value (), arguments);
        INFO ((actual.has_value () ? "" : actual.error ().message));
        REQUIRE (actual.has_value ());
        standard_check (actual.value ()[0], expected.value ()[0]);
      }
  }
}
TEST_CASE ("dynamic byte swizzle handles every unsigned index with overlapping outputs") {
  auto type = ir::detail::vector_integer (8, false);
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("swizzle");
  b.create_function (module, "overlap", {{type}, {type, type}});
  auto data = b.argument (0), indices = b.argument (1);
  b.create_assign (data, b.create_swizzle (data, indices));
  b.create_return (data);
  REQUIRE (b.finish ().has_value ());
  std::array<std::byte, 16> source{};
  for (unsigned n = 0; n < 16; ++n) source[n] = std::byte (n * 13 + 1);
  for (unsigned engine = 0; engine < 4; ++engine) {
    ExecutionOptions options;
    options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
    options.link.compilation = engine == 2   ? JITCompilation::Lazy
                               : engine == 3 ? JITCompilation::BlockVersioning
                                             : JITCompilation::Eager;
    auto session = ExecutionSession::create (program, options);
    REQUIRE (session.has_value ());
    auto function = session.value ().find_function ("swizzle", "overlap");
    REQUIRE (function.has_value ());
    for (unsigned base = 0; base < 256; base += 16) {
      std::array<std::byte, 16> mask{}, expected{};
      for (unsigned n = 0; n < 16; ++n) {
        mask[n] = std::byte (base + n);
        expected[n] = base + n < 16 ? source[n] : std::byte (0);
      }
      std::array<ExecutionArgument, 2> inputs{ScalarValue::vector (type, source).value (),
                                              ScalarValue::vector (type, mask).value ()};
      auto result = session.value ().call (function.value (), inputs);
      REQUIRE (result.has_value ());
      CHECK (std::ranges::equal (result.value ()[0].as_vector (), expected));
    }
  }
}
TEST_CASE ("bitselect accepts arbitrary mask layouts and bitcasts preserve signaling NaNs") {
  auto bytes = ir::detail::vector_integer (8, false);
  auto payload = ir::detail::vector_integer (32, true);
  auto mask_type = wasm_shape ("f64x2");
  Program program;
  MIRBuilder b (program);
  auto m = b.create_module ("bits");
  b.create_function (m, "select", {{payload}, {mask_type, payload, payload}});
  b.create_return (b.create_bitselect (b.argument (0), b.argument (1), b.argument (2)));
  b.create_function (m, "roundtrip", {{mask_type}, {mask_type}});
  b.create_return (b.create_bitcast (b.create_bitcast (b.argument (0), bytes), mask_type));
  b.create_function (m, "splat", {{mask_type}, {Type::pointer ()}});
  b.create_return (
    b.create_load_splat (mask_type, b.memory (Type (Type::Kind::F64), b.argument (0))));
  REQUIRE (b.finish ().has_value ());
  const std::array<std::uint64_t, 2> raw{UINT64_C (0xfff0123456789abc),
                                         UINT64_C (0x55aa1234f00fc33c)};
  auto mask = ScalarValue::vector (mask_type, std::as_bytes (std::span (raw))).value ();
  auto yes = standard_input (payload, 10, 0), no = standard_input (payload, 11, 1);
  auto expected = yes.storage ();
  for (unsigned n = 0; n < 16; ++n)
    expected[n]
      = (mask.storage ()[n] & yes.storage ()[n]) | (~mask.storage ()[n] & no.storage ()[n]);
  for (unsigned engine = 0; engine < 4; ++engine) {
    ExecutionOptions options;
    options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
    options.link.compilation = engine == 2   ? JITCompilation::Lazy
                               : engine == 3 ? JITCompilation::BlockVersioning
                                             : JITCompilation::Eager;
    auto session = ExecutionSession::create (program, options);
    REQUIRE (session.has_value ());
    auto invoke = [&] (std::string_view name, std::initializer_list<ExecutionArgument> args) {
      auto f = session.value ().find_function ("bits", name);
      REQUIRE (f.has_value ());
      auto result = session.value ().call (f.value (), std::span (args));
      REQUIRE (result.has_value ());
      return result.value ()[0];
    };
    CHECK (std::ranges::equal (invoke ("select", {mask, yes, no}).as_vector (), expected));
    CHECK (std::ranges::equal (invoke ("roundtrip", {mask}).as_vector (), mask.as_vector ()));
    const std::array<std::uint64_t, 2> duplicated{raw[0], raw[0]};
    auto result
      = invoke ("splat", {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (raw.data ()))});
    CHECK (std::ranges::equal (result.as_vector (), std::as_bytes (std::span (duplicated))));
  }
}
