/* Licensed under the MIT License. v128 semantics across all execution engines. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Execution/Session.hpp"
#include "mirnext/Serialization/Format.hpp"
#include "mirnext/CodeGen/Detail/NativeVector.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
Program make_program (Type type) {
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("simd");
  for (auto op : {Opcode::VAdd, Opcode::VSub, Opcode::VAnd, Opcode::VOr, Opcode::VXor, Opcode::VEq,
                  Opcode::VNe, Opcode::VLt, Opcode::VLe, Opcode::VGt, Opcode::VGe}) {
    auto result = op >= Opcode::VEq && op <= Opcode::VGe ? type.vector_mask_type () : type;
    b.create_function (module, instruction_descriptor (op).value ().name, {{result}, {type, type}});
    b.create_return (b.create_binary (op, b.argument (0), b.argument (1)));
  }
  b.create_function (module, "splat", {{type}, {type.vector_lane_type ()}});
  b.create_return (b.create_vector_splat (type, b.argument (0)));
  b.create_function (module, "not", {{type}, {type}});
  b.create_return (b.create_bit_not (b.argument (0)));
  b.create_function (module, "extract", {{type.vector_lane_type ()}, {type}});
  b.create_return (b.create_extract_lane (b.argument (0), type.vector_lanes () - 1));
  b.create_function (module, "insert", {{type}, {type, type.vector_lane_type ()}});
  b.create_return (b.create_insert_lane (b.argument (0), b.argument (1), type.vector_lanes () - 1));
  b.create_function (module, "shuffle", {{type}, {type, type}});
  std::array<unsigned, 16> mask{};
  for (unsigned n = 0; n < type.vector_lanes (); ++n)
    mask[n] = type.vector_lanes () - 1 - n + (n % 2 ? type.vector_lanes () : 0);
  b.create_return (b.create_shuffle (b.argument (0), b.argument (1),
                                     std::span (mask).first (type.vector_lanes ())));
  b.create_function (module, "memory", {{type}, {Type::pointer (), Type::pointer ()}});
  auto value = b.create_load (b.memory (type, b.argument (0), 1));
  b.create_store (b.memory (type, b.argument (1), 3), value);
  b.create_return (value);
  b.create_function (module, "constant", {{type}, {}});
  std::array<std::byte, 16> bytes{};
  for (unsigned n = 0; n < 16; ++n) bytes[n] = std::byte (n * 13);
  b.create_return (b.constant_vector (type, bytes));
  auto initialized
    = b.create_variable (module, "initial", type, Operand (VectorConstant{type, bytes}), false);
  auto zero = b.create_variable (module, "zero", type);
  b.create_function (module, "global", {{type}, {}});
  b.create_return (b.create_load (initialized));
  b.create_function (module, "global_zero", {{type}, {}});
  b.create_return (b.create_load (zero));
  b.create_function (module, "global_store", {{type}, {type}});
  b.create_store (zero, b.argument (0));
  b.create_return (b.create_load (zero));
  b.create_function (module, "overlap", {{type}, {Type::pointer (), type}});
  b.create_store (b.memory (type, b.argument (0)), b.argument (1));
  b.create_store (b.memory (Type::i64 (), b.argument (0)), b.constant_i64 (0x12345678));
  b.create_return (b.create_load (b.memory (type, b.argument (0))));
  auto finished = b.finish ();
  INFO ((finished.has_value () ? "" : finished.error ().message));
  REQUIRE (finished.has_value ());
  return program;
}
template <class T, std::size_t N>
ScalarValue vector_value (Type type, const std::array<T, N> &lanes) {
  auto result = ScalarValue::vector (type, std::as_bytes (std::span (lanes)));
  REQUIRE (result.has_value ());
  return result.value ();
}
std::vector<ScalarValue> invoke (const ExecutionSession &session, std::string_view name,
                                 std::initializer_list<ExecutionArgument> arguments = {}) {
  auto function = session.find_function ("simd", name);
  REQUIRE (function.has_value ());
  auto result = session.call (function.value (), std::span (arguments));
  INFO ((result.has_value () ? "" : result.error ().message));
  REQUIRE (result.has_value ());
  return std::move (result).value ();
}
void check_bytes (const ScalarValue &value, std::span<const std::byte> expected) {
  CHECK (std::ranges::equal (value.as_vector (), expected));
}
template <class T>
void exercise () {
  constexpr std::size_t lanes = 16 / sizeof (T);
  constexpr auto scalar = ir::detail::scalar_type<T> ();
  auto element = static_cast<Type::VectorElement> (static_cast<unsigned> (scalar.kind ()) - 1);
  auto type = Type::vector128 (element, lanes);
  std::array<T, lanes> a{}, b{};
  for (std::size_t n = 0; n < lanes; ++n) {
    if constexpr (std::is_floating_point_v<T>) {
      a[n] = n == 0 ? std::numeric_limits<T>::quiet_NaN () : n == 1 ? -T (0) : T (n) + T (0.5);
      b[n] = n == 0 ? T (1) : n == 1 ? T (0) : -T (n);
    } else {
      a[n] = n == 0   ? std::numeric_limits<T>::max ()
             : n == 1 ? std::numeric_limits<T>::min ()
                      : T (n * 117);
      b[n] = n == 0 ? T (1) : T (n * 71 + 3);
    }
  }
  auto first = vector_value (type, a), second = vector_value (type, b);
  const T scalar_value = std::is_floating_point_v<T> ? T (3.5) : T (-7);
  auto source = make_program (type);
  auto text = serialization::write_text (source);
  REQUIRE (text.has_value ());
  auto parsed = serialization::parse_text (text.value ());
  INFO ((parsed.has_value () ? "" : parsed.error ().message));
  REQUIRE (parsed.has_value ());
  auto text_again = serialization::write_text (parsed.value ());
  REQUIRE (text_again.has_value ());
  CHECK (text_again.value () == text.value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto bytes = serialization::write_binary (source, encoding);
    REQUIRE (bytes.has_value ());
    auto decoded = serialization::read_binary (bytes.value (), {encoding});
    INFO ((decoded.has_value () ? "" : decoded.error ().message));
    REQUIRE (decoded.has_value ());
    auto second_bytes = serialization::write_binary (decoded.value (), encoding);
    REQUIRE (second_bytes.has_value ());
    CHECK (bytes.value () == second_bytes.value ());
  }
  for (unsigned engine = 0; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      CAPTURE (engine);
      CAPTURE (level);
      CAPTURE (sizeof (T));
      CAPTURE (std::is_signed_v<T>);
      ExecutionOptions options;
      options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      auto loaded = ExecutionSession::create (source, options);
      INFO ((loaded.has_value () ? "" : loaded.error ().message));
      REQUIRE (loaded.has_value ());
      auto &session = loaded.value ();
      for (auto name : {"vadd", "vsub"}) {
        auto result = invoke (session, name, {first, second});
        REQUIRE (result.size () == 1);
        std::array<T, lanes> actual{};
        std::memcpy (actual.data (), result[0].storage ().data (), 16);
        for (unsigned n = 0; n < lanes; ++n) {
          if constexpr (std::is_floating_point_v<T>) {
            T expected = name == std::string_view ("vadd") ? a[n] + b[n] : a[n] - b[n];
            if (std::isnan (expected))
              CHECK (std::isnan (actual[n]));
            else
              CHECK (std::bit_cast<std::array<std::byte, sizeof (T)>> (actual[n])
                     == std::bit_cast<std::array<std::byte, sizeof (T)>> (expected));
          } else {
            using U = std::make_unsigned_t<T>;
            U expected = name == std::string_view ("vadd") ? U (U (a[n]) + U (b[n]))
                                                           : U (U (a[n]) - U (b[n]));
            CHECK (std::bit_cast<U> (actual[n]) == expected);
          }
        }
      }
      for (auto name : {"vand", "vor", "vxor"}) {
        auto result = invoke (session, name, {first, second});
        for (unsigned n = 0; n < 16; ++n) {
          auto x = first.storage ()[n], y = second.storage ()[n];
          CHECK (result[0].storage ()[n]
                 == (name == std::string_view ("vand")  ? x & y
                     : name == std::string_view ("vor") ? x | y
                                                        : x ^ y));
        }
      }
      for (auto name : {"veq", "vne", "vlt", "vle", "vgt", "vge"}) {
        auto result = invoke (session, name, {first, second});
        CHECK (result[0].type () == type.vector_mask_type ());
        for (unsigned n = 0; n < lanes; ++n) {
          auto test = std::string_view (name);
          bool yes = test == "veq"   ? a[n] == b[n]
                     : test == "vne" ? a[n] != b[n]
                     : test == "vlt" ? a[n] < b[n]
                     : test == "vle" ? a[n] <= b[n]
                     : test == "vgt" ? a[n] > b[n]
                                     : a[n] >= b[n];
          for (unsigned byte = 0; byte < sizeof (T); ++byte)
            CHECK (result[0].storage ()[n * sizeof (T) + byte]
                   == (yes ? std::byte (255) : std::byte (0)));
        }
      }
      auto splat = invoke (session, "splat", {ScalarValue (scalar_value)});
      auto inverted = invoke (session, "not", {first});
      for (unsigned n = 0; n < 16; ++n) CHECK (inverted[0].storage ()[n] == ~first.storage ()[n]);
      for (unsigned n = 0; n < lanes; ++n)
        CHECK (std::memcmp (splat[0].storage ().data () + n * sizeof (T), &scalar_value, sizeof (T))
               == 0);
      auto extracted = invoke (session, "extract", {first});
      if constexpr (std::is_floating_point_v<T>) {
        T actual;
        std::memcpy (&actual, extracted[0].storage ().data (), sizeof (T));
        CHECK (actual == a.back ());
      } else if constexpr (std::is_signed_v<T>)
        CHECK (extracted[0].as_i64 ().value () == std::int64_t (a.back ()));
      else
        CHECK (extracted[0].as_u64 ().value () == std::uint64_t (a.back ()));
      auto inserted = invoke (session, "insert", {first, ScalarValue (scalar_value)});
      auto expected = a;
      expected.back () = scalar_value;
      check_bytes (inserted[0], std::as_bytes (std::span (expected)));
      auto shuffled = invoke (session, "shuffle", {first, second});
      for (unsigned n = 0; n < lanes; ++n) expected[n] = (n % 2 ? b : a)[lanes - 1 - n];
      check_bytes (shuffled[0], std::as_bytes (std::span (expected)));
      std::array<std::byte, 24> input{}, output{};
      std::copy (first.storage ().begin (), first.storage ().end (), input.begin () + 1);
      auto memory
        = invoke (session, "memory",
                  {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (input.data ())),
                   ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (output.data ()))});
      check_bytes (memory[0], first.storage ());
      CHECK (std::ranges::equal (std::span (output).subspan (3, 16), first.storage ()));
      CHECK (output[2] == std::byte (0));
      CHECK (output[19] == std::byte (0));
      auto constant = invoke (session, "constant");
      for (unsigned n = 0; n < 16; ++n) CHECK (constant[0].storage ()[n] == std::byte (n * 13));
      auto global = invoke (session, "global");
      check_bytes (global[0], constant[0].storage ());
      auto zero = invoke (session, "global_zero");
      CHECK (
        std::ranges::all_of (zero[0].storage (), [] (auto byte) { return byte == std::byte (0); }));
      auto stored = invoke (session, "global_store", {first});
      check_bytes (stored[0], first.storage ());
      std::array<std::byte, 16> overlapped{};
      auto overlap
        = invoke (session, "overlap",
                  {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (overlapped.data ())),
                   first});
      auto expected_overlap = first.storage ();
      std::int64_t prefix = 0x12345678;
      std::memcpy (expected_overlap.data (), &prefix, sizeof prefix);
      check_bytes (overlap[0], expected_overlap);
      CHECK (overlapped == expected_overlap);
      // Reuse the same fixed seed across engines and optimization levels.
      std::mt19937 random (0x128);
      for (unsigned sample = 0; sample < 16; ++sample) {
        std::array<T, lanes> x{}, y{}, expected{};
        for (unsigned n = 0; n < lanes; ++n) {
          if constexpr (std::is_floating_point_v<T>) {
            x[n] = T (int (random () % 20001) - 10000) / T (8);
            y[n] = T (int (random () % 20001) - 10000) / T (8);
            expected[n] = x[n] + y[n];
          } else {
            using U = std::make_unsigned_t<T>;
            auto x_bits = U ((std::uint64_t (random ()) << 32) | random ());
            auto y_bits = U ((std::uint64_t (random ()) << 32) | random ());
            x[n] = std::bit_cast<T> (x_bits);
            y[n] = std::bit_cast<T> (y_bits);
            expected[n] = std::bit_cast<T> (U (x_bits + y_bits));
          }
        }
        auto sum = invoke (session, "vadd", {vector_value (type, x), vector_value (type, y)});
        check_bytes (sum[0], std::as_bytes (std::span (expected)));
        for (unsigned comparison = 0; comparison < 6; ++comparison) {
          std::array<std::byte, 16> mask{};
          for (unsigned n = 0; n < lanes; ++n) {
            bool yes = comparison == 0   ? x[n] == y[n]
                       : comparison == 1 ? x[n] != y[n]
                       : comparison == 2 ? x[n] < y[n]
                       : comparison == 3 ? x[n] <= y[n]
                       : comparison == 4 ? x[n] > y[n]
                                         : x[n] >= y[n];
            std::fill_n (mask.begin () + n * sizeof (T), sizeof (T), std::byte (yes ? 255 : 0));
          }
          auto result
            = invoke (session, std::array{"veq", "vne", "vlt", "vle", "vgt", "vge"}[comparison],
                      {vector_value (type, x), vector_value (type, y)});
          check_bytes (result[0], mask);
        }
      }
    }
}
}  // namespace
TEST_CASE_TEMPLATE ("v128 portable core preserves lane semantics in O0-O3 and all engines", T,
                    std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
                    std::uint32_t, std::int64_t, std::uint64_t, float, double) {
  exercise<T> ();
}

TEST_CASE ("v128 construction rejects invalid types, lanes, masks and owners without mutation") {
  auto type = Type::vector128 (Type::VectorElement::I32, 4);
  CHECK_FALSE (Type::vector128 (Type::VectorElement::I32, 8).valid ());
  CHECK_FALSE (Type::vector128 (Type::VectorElement::I8, 0).valid ());
  CHECK_FALSE (Type::vector128 (Type::VectorElement::I8, 0x20000010).valid ());
  CHECK_FALSE (Type::vector128 (static_cast<Type::VectorElement> (255), 4).valid ());
  for (unsigned scenario = 0; scenario < 16; ++scenario) {
    CAPTURE (scenario);
    Program program;
    MIRBuilder b (program);
    auto module = b.create_module ("errors");
    auto f = b.create_function (module, "f", {{type}, {type}});
    auto first = b.argument (0);
    switch (scenario) {
    case 0: b.create_extract_lane (first, 4); break;
    case 1: b.create_insert_lane (first, b.constant (1.0), 0); break;
    case 2: b.create_shuffle (first, first, {0, 1, 2}); break;
    case 3: b.create_shuffle (first, first, {0, 1, 2, 8}); break;
    case 4: b.create_vector_splat (type, b.constant (1.0f)); break;
    case 5: {
      std::array<std::byte, 15> short_bytes{};
      b.constant_vector (type, short_bytes);
      break;
    }
    case 6: {
      auto other
        = b.create_vector_splat (Type::vector128 (Type::VectorElement::I16, 8), b.constant_i64 (1));
      b.create_add (first, other);
      break;
    }
    case 7: {
      b.create_return (first);
      b.create_function (module, "other", {{type}, {type}});
      b.create_add (first, b.argument (0));
      break;
    }
    case 8: b.create_div (first, first); break;
    case 9: b.create_return (b.create_eq (first, first)); break;
    case 10: {
      auto imported = b.create_import (module, "variadic", Signature{{}, {}, true});
      b.create_call (imported, {first});
      break;
    }
    case 11: b.create_va_arg (type, b.constant (static_cast<void *> (nullptr))); break;
    case 12: {
      auto variable
        = b.create_variable (module, "readonly", type, Operand (VectorConstant{type}), false);
      b.create_store (variable, first);
      break;
    }
    case 13: b.constant (type, Operand (1)); break;
    case 14: b.constant (Type::i64 (), Operand (VectorConstant{type})); break;
    case 15: {
      b.create_variable (module, "mismatch", type,
                         Operand (VectorConstant{Type::vector128 (Type::VectorElement::U16, 8)}));
      break;
    }
    }
    CHECK_FALSE (b.status ().has_value ());
    auto error = b.status ().error ().message;
    auto snapshot = dump (*f);
    b.create_add (first, first);
    b.create_return (first);
    CHECK (dump (*f) == snapshot);
    CHECK (b.status ().error ().message == error);
    CHECK_FALSE (b.finish ().has_value ());
  }
}

TEST_CASE ("v128 calls, stack parameters, live vectors and SSA loop copies preserve all bits") {
  auto type = Type::vector128 (Type::VectorElement::U32, 4);
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("simd");
  Signature sum_signature{{type}, std::vector<Type> (10, type)};
  auto sum = b.create_function (module, "sum", sum_signature);
  auto value = b.argument (0);
  for (unsigned n = 1; n < 10; ++n) value = b.create_add (value, b.argument (n));
  b.create_return (value);
  Signature mixed_signature{{type}, std::vector<Type> (8, type)};
  mixed_signature.parameters.insert (mixed_signature.parameters.end (), 9, Type::i64 ());
  mixed_signature.parameters.push_back (type);
  b.create_function (module, "mixed", mixed_signature);
  b.create_return (b.create_add (b.argument (17), b.create_vector_splat (type, b.argument (16))));
  auto dispatch
    = b.create_variable (module, "dispatch", Type::pointer (), Operand (sum->symbol ()));
  b.create_function (module, "caller", {{type}, {type}});
  auto input = b.argument (0);
  std::vector<CallArgument> args (10, input);
  auto called = b.create_indirect_call (sum, b.create_load (dispatch), args).value ();
  b.create_return (b.create_add (called, input));

  b.create_function (module, "loop", {{type}, {type, Type::i64 ()}});
  auto initial = b.argument (0), count = b.argument (1);
  auto accumulator = b.create_register (type);
  auto index = b.create_register (Type::i64 ());
  b.create_assign (accumulator, initial);
  b.create_assign (index, b.constant_i64 (0));
  auto header = b.create_block ("header"), body = b.create_block ("body"),
       exit = b.create_block ("exit");
  b.create_jump (header);
  b.set_insert_point (header);
  b.create_cond_branch (b.create_ge (index, count), exit);
  b.set_insert_point (body);
  auto updated = b.create_add (accumulator, initial);
  b.create_assign (accumulator, b.create_shuffle (updated, updated, {0, 1, 2, 3}));
  b.create_assign (index, b.create_add (index, b.constant_i64 (1)));
  b.create_jump (header);
  b.set_insert_point (exit);
  b.create_return (accumulator);
  auto built = b.finish ();
  INFO ((built.has_value () ? "" : built.error ().message));
  REQUIRE (built.has_value ());
  std::array<std::uint32_t, 4> lanes{0xffffffffu, 0x7fffffff, 0x80000001, 9};
  auto argument = vector_value (type, lanes);
  std::vector<ExecutionArgument> arguments (10, argument);
  for (unsigned engine = 0; engine < 5; ++engine)
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
      if (engine == 4)
        options.select_engine = [] (const Function &function) -> Result<ExecutionEngine> {
          if (function.symbol ().name () == "sum") return ExecutionEngine::Interpreter;
          if (function.symbol ().name () == "loop") return ExecutionEngine::BlockVersioning;
          return ExecutionEngine::LazyJIT;
        };
      auto session = ExecutionSession::create (program, options);
      INFO ((session.has_value () ? "" : session.error ().message));
      REQUIRE (session.has_value ());
      auto function = session.value ().find_function ("simd", "sum");
      REQUIRE (function.has_value ());
      auto result = session.value ().call (function.value (), arguments);
      INFO ((result.has_value () ? "" : result.error ().message));
      REQUIRE (result.has_value ());
      auto expected = lanes;
      for (auto &lane : expected) lane *= 10;
      check_bytes (result.value ()[0], std::as_bytes (std::span (expected)));
      auto caller = invoke (session.value (), "caller", {argument});
      for (unsigned n = 0; n < 4; ++n) expected[n] = lanes[n] * 11;
      check_bytes (caller[0], std::as_bytes (std::span (expected)));
      std::vector<ExecutionArgument> mixed_arguments (8, argument);
      for (unsigned n = 0; n < 9; ++n) mixed_arguments.emplace_back (std::int64_t (77));
      mixed_arguments.emplace_back (argument);
      auto mixed = session.value ().find_function ("simd", "mixed");
      REQUIRE (mixed.has_value ());
      auto mixed_result = session.value ().call (mixed.value (), mixed_arguments);
      INFO ((mixed_result.has_value () ? "" : mixed_result.error ().message));
      REQUIRE (mixed_result.has_value ());
      for (unsigned n = 0; n < 4; ++n) expected[n] = lanes[n] + 77;
      check_bytes (mixed_result.value ()[0], std::as_bytes (std::span (expected)));
      for (std::int64_t count_value : {0, 1, 7}) {
        auto loop = invoke (session.value (), "loop", {argument, count_value});
        for (unsigned n = 0; n < 4; ++n) expected[n] = lanes[n] * (std::uint32_t (count_value) + 1);
        check_bytes (loop[0], std::as_bytes (std::span (expected)));
      }
    }
}

TEST_CASE ("v128 GVN keys preserve full types across differently shaped splats") {
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("simd");
  b.create_function<void (void *)> (module, "shapes");
  auto bytes = Type::vector128 (Type::VectorElement::U8, 16);
  auto words = Type::vector128 (Type::VectorElement::U16, 8);
  auto ones = b.constant_i64 (1);
  auto first = b.create_vector_splat (bytes, ones);
  auto second = b.create_vector_splat (words, ones);
  b.create_store (b.memory (bytes, b.argument (0)), first);
  b.create_store (b.memory (words, b.argument (0), 16), second);
  b.create_return ();
  REQUIRE (b.finish ().has_value ());
  for (auto mode : {ExecutionMode::Interpreter, ExecutionMode::JIT}) {
    std::array<std::byte, 32> output{};
    auto session = ExecutionSession::create (program, {.mode = mode});
    INFO ((session.has_value () ? "" : session.error ().message));
    REQUIRE (session.has_value ());
    invoke (session.value (), "shapes",
            {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (output.data ()))});
    for (unsigned n = 0; n < 32; ++n) CHECK (output[n] == std::byte (n < 16 || n % 2 == 0 ? 1 : 0));
  }
}

TEST_CASE (
  "native shuffles cover exhaustive two-lane masks, every lane transfer and arbitrary bit "
  "patterns") {
  for (auto type : {Type::vector128 (Type::VectorElement::U8, 16),
                    Type::vector128 (Type::VectorElement::U16, 8),
                    Type::vector128 (Type::VectorElement::U32, 4),
                    Type::vector128 (Type::VectorElement::U64, 2),
                    Type::vector128 (Type::VectorElement::F32, 4),
                    Type::vector128 (Type::VectorElement::F64, 2)}) {
    unsigned lanes = type.vector_lanes (), width = 16 / lanes;
    CAPTURE (lanes);
    CAPTURE (type.vector_element ());
    Program program;
    MIRBuilder b (program);
    auto module = b.create_module ("simd");
    b.create_function<void (void *, const void *, const void *)> (module, "matrix");
    auto a = b.create_load (b.memory (type, b.argument (1)));
    auto c = b.create_load (b.memory (type, b.argument (2)));
    std::vector<std::vector<unsigned>> masks;
    if (lanes == 2) {
      for (unsigned first = 0; first < 4; ++first)
        for (unsigned second = 0; second < 4; ++second) masks.push_back ({first, second});
    } else {
      std::mt19937 random (0x5128 + lanes);
      for (unsigned n = 0; n < 32; ++n) {
        std::vector<unsigned> mask (lanes);
        for (auto &i : mask) i = random () % (lanes * 2);
        masks.push_back (std::move (mask));
      }
    }
    for (unsigned pattern = 0; pattern < 7; ++pattern) {
      std::vector<unsigned> mask (lanes);
      for (unsigned n = 0; n < lanes; ++n)
        mask[n] = pattern == 0   ? n
                  : pattern == 1 ? lanes + n
                  : pattern == 2 ? lanes - n - 1
                  : pattern == 3 ? lanes + 1
                  : pattern == 4 ? n / 2 + (n % 2 ? lanes : 0)
                  : pattern == 5 ? n / 2 + lanes / 2 + (n % 2 ? lanes : 0)
                                 : (lanes - 1 + n) % (2 * lanes);
      masks.push_back (std::move (mask));
    }
    for (unsigned n = 0; n < masks.size (); ++n)
      b.create_store (b.memory (type, b.argument (0), n * 16), b.create_shuffle (a, c, masks[n]));
    for (unsigned lane = 0; lane < lanes; ++lane) {
      auto extracted = b.create_extract_lane (c, lane);
      auto replaced = b.create_insert_lane (a, extracted, (lane + 1) % lanes);
      b.create_store (b.memory (type, b.argument (0), (masks.size () + lane * 2) * 16), replaced);
      b.create_store (b.memory (type, b.argument (0), (masks.size () + lane * 2 + 1) * 16),
                      b.create_vector_splat (type, extracted));
    }
    b.create_return ();
    REQUIRE (b.finish ().has_value ());
    std::array<std::byte, 16> first{}, second{};
    for (unsigned n = 0; n < 16; ++n) {
      first[n] = std::byte (n * 17 + 1);
      second[n] = std::byte (255 - n * 9);
    }
    // Include signed zero and NaN payloads; lane operations must preserve bits.
    if (type.vector_lane_type ().is_float ()) {
      std::uint64_t bits = width == 4 ? UINT64_C (0x7fc12345) : UINT64_C (0x7ff8123456789abc);
      std::memcpy (second.data (), &bits, width);
      bits = width == 4 ? UINT64_C (0x7f812345) : UINT64_C (0x7ff0123456789abc);
      std::memcpy (second.data () + width, &bits, width);
      bits = width == 4 ? UINT64_C (0x80000000) : UINT64_C (0x8000000000000000);
      std::memcpy (first.data (), &bits, width);
    }
    std::vector<std::byte> expected ((masks.size () + 2 * lanes) * 16);
    for (unsigned n = 0; n < masks.size (); ++n)
      for (unsigned lane = 0; lane < lanes; ++lane) {
        auto index = masks[n][lane];
        auto source = index < lanes ? first.data () : second.data ();
        std::memcpy (expected.data () + n * 16 + lane * width, source + index % lanes * width,
                     width);
      }
    for (unsigned lane = 0; lane < lanes; ++lane) {
      auto output = expected.data () + (masks.size () + lane * 2) * 16;
      std::memcpy (output, first.data (), 16);
      std::memcpy (output + (lane + 1) % lanes * width, second.data () + lane * width, width);
      for (unsigned n = 0; n < lanes; ++n)
        std::memcpy (output + 16 + n * width, second.data () + lane * width, width);
    }
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
        bool prepared = false;
        options.link.compilation_observer
          = [&] (const codegen::CompilationSnapshot &snapshot) -> Result<void> {
          if (snapshot.stage == codegen::CompilationStage::PrepareMachineIR
              && snapshot.function.name () == "matrix") {
            prepared = true;
            for (auto &reg : snapshot.function.registers ()) CHECK_FALSE (reg.is_memory_backed ());
            for (auto &block : snapshot.function.blocks ())
              for (auto &instruction : block.instructions ()) {
                CHECK (instruction.opcode () != codegen::MachineOpcode (Opcode::Addr));
                CHECK (instruction.opcode () != codegen::MachineOpcode (Opcode::Alloca));
              }
          }
          return {};
        };
        auto session = ExecutionSession::create (program, options);
        INFO ((session.has_value () ? "" : session.error ().message));
        REQUIRE (session.has_value ());
        std::vector<std::byte> actual (expected.size ());
        invoke (session.value (), "matrix",
                {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (actual.data ())),
                 ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (first.data ())),
                 ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (second.data ()))});
        CHECK (actual == expected);
        if (engine != 0) CHECK (prepared);
      }
  }
}

TEST_CASE ("native vector comparisons and transfers survive actual register pressure") {
  auto type = Type::vector128 (Type::VectorElement::U64, 2);
  Program program;
  MIRBuilder b (program);
  auto module = b.create_module ("simd");
  b.create_function<void (void *, const void *)> (module, "pressure");
  std::vector<Value> values;
  for (unsigned n = 0; n < 32; ++n)
    values.push_back (b.create_load (b.memory (type, b.argument (1), n * 16)));
  auto accumulated = values[0];
  for (unsigned n = 1; n < values.size (); ++n) {
    auto compared = b.create_ge (values[n - 1], values[n]);
    auto reversed = b.create_shuffle (values[n], accumulated, {3, 0});
    accumulated = b.create_xor (b.create_add (accumulated, reversed), compared);
  }
  b.create_store (b.memory (type, b.argument (0)), accumulated);
  b.create_return ();
  REQUIRE (b.finish ().has_value ());
  std::array<std::uint64_t, 64> inputs{};
  for (unsigned n = 0; n < inputs.size (); ++n)
    inputs[n] = (UINT64_C (0xf123456789abcdef) * (n + 1)) ^ (UINT64_C (1) << (n % 64));
  std::array<std::byte, 16> expected{};
  for (unsigned engine = 0; engine < 4; ++engine)
    for (unsigned level = 0; level < 4; ++level) {
      ExecutionOptions options;
      options.mode = engine == 0 ? ExecutionMode::Interpreter : ExecutionMode::JIT;
      options.optimization = transforms::OptimizationOptions{
        .level = static_cast<transforms::OptimizationLevel> (level)};
      options.link.compilation = engine == 2   ? JITCompilation::Lazy
                                 : engine == 3 ? JITCompilation::BlockVersioning
                                               : JITCompilation::Eager;
      bool spilled = false;
      options.link.compilation_observer
        = [&] (const codegen::CompilationSnapshot &snapshot) -> Result<void> {
        if (snapshot.stage != codegen::CompilationStage::Rewrite
            || snapshot.function.name () != "pressure")
          return {};
        for (auto &block : snapshot.function.blocks ())
          for (auto &instruction : block.instructions ())
            if (instruction.opcode () == codegen::MachineOpcode (Opcode::VMov))
              for (auto &operand : instruction.operands ())
                if (auto memory = operand.get<codegen::MachineMemory> ();
                    memory.has_value () && memory.value ().base)
                  if (auto physical = memory.value ().base->get<target::PhysicalRegister> ();
                      physical.has_value ()) {
                    auto encoding = physical.value ().encoding ();
                    spilled |= snapshot.function.target ().architecture ()
                                   == target::Architecture::AArch64
                                 ? encoding == 29 || encoding == 31
                                 : encoding == 4 || encoding == 5;
                  }
        return {};
      };
      auto session = ExecutionSession::create (program, options);
      INFO ((session.has_value () ? "" : session.error ().message));
      REQUIRE (session.has_value ());
      std::array<std::byte, 16> actual{};
      invoke (session.value (), "pressure",
              {ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (actual.data ())),
               ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (inputs.data ()))});
      CAPTURE (engine);
      CAPTURE (level);
      if (engine != 0) CHECK (spilled);
      if (engine == 0) expected = actual;
      CHECK (actual == expected);
    }
}
