/* Licensed under the MIT License. SIMD packing, memory ordering and execution parity. */
#define DOCTEST_CONFIG_NO_EXCEPTIONS_BUT_WITH_ALL_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/Transforms/Vectorization.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Transforms/PrepareCFG.hpp"
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/DeadCode.hpp"
#include "mirnext/Target/VectorCost.hpp"
#include "mirnext/Execution/Session.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <cmath>
#include <limits>
#include <random>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
using namespace mirnext::execution;
namespace {
void require (const Result<void> &r) {
  INFO ((r.has_value () ? "" : r.error ().message));
  REQUIRE (r.has_value ());
}
Program kernel (Type type, Opcode op = Opcode::Add, bool interleaved = false,
                bool alias_classes = false, unsigned lanes = 0, bool external = false) {
  Program p;
  MIRBuilder b (p);
  auto m = b.create_module ("m");
  b.create_function (m, "f",
                     {{Type::i64 ()}, {Type::pointer (), Type::pointer (), Type::pointer ()}});
  auto output = b.argument (0), x = b.argument (1), y = b.argument (2);
  auto bytes = type.kind () == Type::Kind::F64 ? 8u
               : type.kind () == Type::Kind::F32
                 ? 4u
                 : (1u << ((static_cast<unsigned> (type.kind ()) - 1) / 2));
  if (!lanes) lanes = 16 / bytes;
  std::vector<Value> results;
  for (unsigned n = 0; n < lanes; ++n) {
    auto a = b.memory (type, x, n * bytes), c = b.memory (type, y, n * bytes);
    if (alias_classes) {
      a.alias = "x";
      c.alias = "y";
    }
    auto left = b.create_load (a), right = b.create_load (c);
    Value value;
    if (op == Opcode::Add)
      value = b.create_add (left, right);
    else if (op == Opcode::Sub)
      value = b.create_sub (left, right);
    else if (op == Opcode::Mul)
      value = b.create_mul (left, right);
    else if (op == Opcode::FDiv)
      value = b.create_div (left, right);
    else if (op == Opcode::Neg)
      value = b.create_neg (left);
    else if (op == Opcode::And)
      value = b.create_and (left, right);
    else if (op == Opcode::Or)
      value = b.create_or (left, right);
    else
      value = b.create_xor (left, right);
    results.push_back (value);
    if (interleaved) {
      auto target = b.memory (type, output, n * bytes);
      if (alias_classes) target.alias = "out";
      b.create_store (target, value);
    }
  }
  if (!interleaved)
    for (unsigned n = 0; n < lanes; ++n) {
      auto target = b.memory (type, output, n * bytes);
      if (alias_classes) target.alias = "out";
      b.create_store (target, results[n]);
    }
  b.create_return (external ? b.create_convert (results[0], Type::i64 ()) : b.constant_i64 (0));
  require (b.finish ());
  return p;
}
Function &function (Program &p) { return *p.modules ().begin ()->functions ().begin (); }
std::size_t count (const Function &f, Opcode op) {
  std::size_t n = 0;
  for (auto &b : f.blocks ())
    for (auto &i : b.instructions ()) n += i.opcode () == op;
  return n;
}
void prepare (Function &f, FunctionAnalysisManager &a) {
  FunctionPassManager pm;
  pm.add<SimplifyPass> ();
  pm.add<PrepareCFGPass> ();
  pm.add<BuildSSAPass> ();
  require (pm.run (f, a));
}
std::size_t slp (Program &p, target::Architecture target = target::Architecture::Native,
                 VectorizationOptions options = {}) {
  auto &f = function (p);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto r = transforms::detail::run_pass (SLPVectorizePass (target, options), f, a);
  INFO ((r.has_value () ? "" : r.error ().message));
  REQUIRE (r.has_value ());
  require (f.ssa_form ().value ().verify ());
  return r.value ().transformations;
}
OptimizationOptions vector_options (OptimizationLevel level = OptimizationLevel::O2) {
  OptimizationOptions options;
  options.level = level;
  options.vectorization.slp = true;
  return options;
}
void roundtrip (const Program &p) {
  auto text = serialization::write_text (p);
  REQUIRE (text.has_value ());
  auto parsed = serialization::parse_text (text.value ());
  REQUIRE (parsed.has_value ());
  for (auto encoding :
       {serialization::BinaryEncoding::Raw, serialization::BinaryEncoding::Compressed}) {
    auto binary = serialization::write_binary (p, encoding);
    REQUIRE (binary.has_value ());
    auto decoded = serialization::read_binary (binary.value (), {encoding});
    REQUIRE (decoded.has_value ());
    CHECK (serialization::write_text (decoded.value ()).value () == text.value ());
  }
}
std::array<std::byte, 16> invoke (const ExecutionSession &session,
                                  const std::array<std::byte, 16> &a,
                                  const std::array<std::byte, 16> &b) {
  std::array<std::byte, 16> result{};
  std::array<ExecutionArgument, 3>
    arguments{ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (result.data ())),
              ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (a.data ())),
              ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (b.data ()))};
  auto f = session.find_function ("m", "f");
  REQUIRE (f.has_value ());
  auto called = session.call (f.value (), arguments);
  INFO ((called.has_value () ? "" : called.error ().message));
  REQUIRE (called.has_value ());
  return result;
}
}  // namespace
TEST_CASE ("SLP packs ten scalar lane types without changing public ownership or protocols") {
  for (unsigned k = unsigned (Type::Kind::I8); k <= unsigned (Type::Kind::F64); ++k) {
    Type type (static_cast<Type::Kind> (k));
    auto p = kernel (type);
    auto before = dump (p);
    auto options = vector_options ();
    std::size_t packs = 0;
    options.observer = [&] (const OptimizationSnapshot &s) -> Result<void> {
      if (s.statistics.pass == PassId::SLPVectorize && s.phase == PassEvent::Phase::After) {
        packs += s.statistics.transformations;
        REQUIRE (s.ssa ().has_value ());
        require (s.ssa ().value ().verify ());
      }
      return {};
    };
    auto result = optimize (p, options);
    INFO ((result.has_value () ? "" : result.error ().message));
    REQUIRE (result.has_value ());
    CAPTURE (k);
    INFO (result.value ().dump ());
    CHECK (packs == 1);
    CHECK (count (result.value ().find_function ("m", "f").value (), Opcode::VAdd) == 1);
    CHECK (dump (p) == before);
    roundtrip (result.value ().program ());
  }
}
TEST_CASE ("SLP is opt in at O2 and O3; existing O0 and O1 pipelines remain scalar") {
  auto p = kernel (Type (Type::Kind::F32));
  for (unsigned n = 0; n < 4; ++n)
    for (bool enable : {false, true}) {
      auto options = vector_options (static_cast<OptimizationLevel> (n));
      options.vectorization.slp = enable;
      auto result = optimize (p, options);
      REQUIRE (result.has_value ());
      CHECK (count (result.value ().find_function ("m", "f").value (), Opcode::VAdd)
             == (enable && n >= 2));
    }
}
TEST_CASE ("SLP checks aliasing before moving loads across stores") {
  for (bool classes : {false, true}) {
    auto p = kernel (Type (Type::Kind::F32), Opcode::Add, true, classes);
    CHECK (slp (p) == unsigned (classes));
  }
  // Loading all inputs before storing is legal even if caller pointers overlap.
  auto p = kernel (Type (Type::Kind::F32));
  CHECK (slp (p) == 1);
}
TEST_CASE ("SLP rejects partial packs external uses and exhausted search budgets atomically") {
  auto partial = kernel (Type (Type::Kind::I32), Opcode::Add, false, false, 3);
  CHECK (slp (partial) == 0);
  auto external = kernel (Type::i64 (), Opcode::Add, false, false, 0, true);
  CHECK (slp (external) == 0);
  for (auto options :
       {VectorizationOptions{.max_pack_nodes = 0}, VectorizationOptions{.max_block_packs = 0},
        VectorizationOptions{.max_store_distance = 0}}) {
    auto p = kernel (Type (Type::Kind::F32));
    CHECK (slp (p, target::Architecture::Native, options) == 0);
    CHECK (count (function (p), Opcode::VMov) == 0);
  }
}
TEST_CASE (
  "target sequence costs reject unprofitable i64 multiplication and illegal i8 multiplication") {
  for (auto target : {target::Architecture::AArch64, target::Architecture::X86_64}) {
    auto model = target::VectorCostModel::get (target);
    REQUIRE (model.has_value ());
    CHECK_FALSE (
      model.value ().operation (Opcode::VMul, Type::vector128 (Type::VectorElement::I8, 16)));
    for (auto type : {Type (Type::Kind::I8), Type::i64 ()}) {
      auto p = kernel (type, Opcode::Mul);
      CHECK (slp (p, target) == 0);
    }
    auto p = kernel (Type (Type::Kind::I16), Opcode::Mul);
    CHECK (slp (p, target) == 1);
  }
}
TEST_CASE ("vector passes diagnose absent SSA and invalid targets") {
  auto p = kernel (Type (Type::Kind::F64));
  auto &f = function (p);
  FunctionAnalysisManager a (f);
  CHECK_FALSE (transforms::detail::run_pass (SLPVectorizePass (), f, a).has_value ());
  CHECK_FALSE (transforms::detail::run_pass (VectorCombinePass (), f, a).has_value ());
  prepare (f, a);
  CHECK_FALSE (
    transforms::detail::run_pass (SLPVectorizePass (static_cast<target::Architecture> (99)), f, a)
      .has_value ());
}
TEST_CASE ("vector stage observation failure aborts the owning pipeline") {
  auto p = kernel (Type (Type::Kind::F32));
  auto options = vector_options ();
  options.observer = [] (const OptimizationSnapshot &s) -> Result<void> {
    if (s.statistics.pass == PassId::SLPVectorize && s.phase == PassEvent::Phase::After)
      return Error{ErrorCode::InvalidArgument, "stop-vector-observer"};
    return {};
  };
  auto r = optimize (p, options);
  REQUIRE_FALSE (r.has_value ());
  CHECK (r.error ().message.find ("stop-vector-observer") != std::string::npos);
  require (p.verify ());
}
TEST_CASE ("VectorCombine removes identity casts shuffles insertion and repeated broadcasts") {
  Program p;
  MIRBuilder b (p);
  auto m = b.create_module ("m");
  auto v = Type::vector128 (Type::VectorElement::F32, 4);
  auto bits = Type::vector128 (Type::VectorElement::U32, 4);
  b.create_function (m, "f", {{v}, {v, Type (Type::Kind::F32)}});
  auto input = b.argument (0), scalar = b.argument (1);
  auto cast = b.create_bitcast (b.create_bitcast (input, bits), v);
  std::array<unsigned, 4> identity{0, 1, 2, 3};
  auto shuffled = b.create_shuffle (cast, input, identity);
  auto inserted = b.create_insert_lane (shuffled, b.create_extract_lane (shuffled, 2), 2);
  auto splat1 = b.create_vector_splat (v, scalar), splat2 = b.create_vector_splat (v, scalar);
  auto recovered = b.create_extract_lane (splat2, 1);
  b.create_return (
    b.create_add (inserted, b.create_add (splat1, b.create_vector_splat (v, recovered))));
  require (b.finish ());
  auto &f = function (p);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto r = transforms::detail::run_pass (VectorCombinePass (), f, a);
  INFO ((r.has_value () ? "" : r.error ().message));
  REQUIRE (r.has_value ());
  CHECK (r.value ().transformations >= 5);
  require (f.ssa_form ().value ().verify ());
  FunctionPassManager finish;
  finish.add<SSADeadCodePass> ();
  finish.add<ConventionalSSAPass> ();
  finish.add<DestroySSAPass> ();
  require (finish.run (f, a));
  CHECK (count (f, Opcode::VBitcast) == 0);
  CHECK (count (f, Opcode::VShuffle) == 0);
  CHECK (count (f, Opcode::VInsert) == 0);
  CHECK (count (f, Opcode::VExtract) == 0);
  CHECK (count (f, Opcode::VSplat) == 1);
  roundtrip (p);
}
TEST_CASE ("narrow integer extract from splat retains required sign extension") {
  Program p;
  MIRBuilder b (p);
  auto m = b.create_module ("m");
  auto type = Type::vector128 (Type::VectorElement::I8, 16);
  b.create_function (m, "f", {{Type::i64 ()}, {Type::i64 ()}});
  b.create_return (
    b.create_convert (b.create_extract_lane (b.create_vector_splat (type, b.argument (0)), 3),
                      Type::i64 ()));
  require (b.finish ());
  auto r = optimize (p, {.vectorization = {.combine = true}});
  REQUIRE (r.has_value ());
  CHECK (count (r.value ().find_function ("m", "f").value (), Opcode::VExtract) == 1);
  auto session = ExecutionSession::create (std::move (r).value ());
  REQUIRE (session.has_value ());
  std::array<std::int64_t, 1> args{255};
  auto result = session.value ().call_i64 ("m", "f", args);
  REQUIRE (result.has_value ());
  CHECK (result.value () == -1);
}
TEST_CASE ("integer and ordered floating SLP agree across interpreter eager lazy and BBV") {
  std::mt19937_64 random (0x51f128);
  for (auto kind :
       {Type::Kind::I8, Type::Kind::U8, Type::Kind::I16, Type::Kind::U16, Type::Kind::I32,
        Type::Kind::U32, Type::Kind::I64, Type::Kind::U64, Type::Kind::F32, Type::Kind::F64}) {
    auto type = Type (kind);
    for (auto operation : {Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Neg}) {
      auto p = kernel (type, operation);
      auto original = ExecutionSession::create (p, {.mode = ExecutionMode::Interpreter});
      REQUIRE (original.has_value ());
      for (auto level : {OptimizationLevel::O2, OptimizationLevel::O3})
        for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                            ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
          ExecutionOptions options;
          options.optimization = vector_options (level);
          options.select_engine
            = [engine] (const Function &) -> Result<ExecutionEngine> { return engine; };
          auto compiled = ExecutionSession::create (p, options);
          INFO ((compiled.has_value () ? "" : compiled.error ().message));
          REQUIRE (compiled.has_value ());
          for (unsigned trial = 0; trial < 8; ++trial) {
            std::array<std::byte, 16> a{}, b{};
            for (unsigned n = 0; n < 16; ++n) {
              a[n] = std::byte (random ());
              b[n] = std::byte (random ());
            }
            // Finite values give bit-exact elementwise FP results. NaNs are covered separately.
            if (kind == Type::Kind::F32)
              for (unsigned n = 0; n < 4; ++n) {
                float x = float (std::int32_t (random ())) / 65536,
                      y = float (std::int32_t (random ())) / 65536;
                std::memcpy (a.data () + 4 * n, &x, 4);
                std::memcpy (b.data () + 4 * n, &y, 4);
              }
            if (kind == Type::Kind::F64)
              for (unsigned n = 0; n < 2; ++n) {
                double x = double (std::int32_t (random ())) / 65536,
                       y = double (std::int32_t (random ())) / 65536;
                std::memcpy (a.data () + 8 * n, &x, 8);
                std::memcpy (b.data () + 8 * n, &y, 8);
              }
            auto expected = invoke (original.value (), a, b);
            auto actual = invoke (compiled.value (), a, b);
            CAPTURE (kind);
            CAPTURE (operation);
            CAPTURE (engine);
            CAPTURE (level);
            CAPTURE (trial);
            CHECK (actual == expected);
          }
        }
    }
  }
}
TEST_CASE (
  "SLP preserves overlapping input output memory and rejects interleaved memory dependencies") {
  for (bool interleaved : {false, true}) {
    auto p = kernel (Type (Type::Kind::F32), Opcode::Add, interleaved);
    auto reference = ExecutionSession::create (p, {.mode = ExecutionMode::Interpreter});
    REQUIRE (reference.has_value ());
    for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                        ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
      ExecutionOptions options;
      options.optimization = vector_options ();
      options.select_engine
        = [engine] (const Function &) -> Result<ExecutionEngine> { return engine; };
      auto session = ExecutionSession::create (p, options);
      REQUIRE (session.has_value ());
      for (unsigned offset : {0u, 1u, 2u, 4u}) {
        std::array<float, 12> expected{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, actual = expected;
        auto call = [offset] (const ExecutionSession &s, auto &data) {
          std::array<ExecutionArgument, 3>
            arguments{ScalarValue::pointer (
                        reinterpret_cast<std::uintptr_t> (data.data () + offset)),
                      ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (data.data ())),
                      ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (data.data () + 4))};
          auto f = s.find_function ("m", "f");
          REQUIRE (f.has_value ());
          auto r = s.call (f.value (), arguments);
          INFO ((r.has_value () ? "" : r.error ().message));
          REQUIRE (r.has_value ());
        };
        call (reference.value (), expected);
        call (session.value (), actual);
        CHECK (actual == expected);
      }
    }
  }
}
TEST_CASE ("SLP does not move memory across calls barriers or overlapping writes") {
  for (std::string middle :
       {"call sig, side\n", "mov i64:(a), 9\n", "bstart s\n", "alloca s, 16\n"}) {
    auto parsed = serialization::parse_text (
      "m:module\nsig:proto\nimport side\nf:func i64, i64:o, i64:a\nlocal i64:x, i64:y, i64:s\n"
      "mov x, i64:(a)\n"
      + middle + "mov y, i64:8(a)\nmov i64:(o), x\nmov i64:8(o), y\nret 0\nendfunc\nendmodule\n");
    INFO ((parsed.has_value () ? "" : parsed.error ().message));
    REQUIRE (parsed.has_value ());
    CHECK (slp (parsed.value ()) == 0);
  }
}
TEST_CASE ("SLP requires contiguous access widths and stays inside a basic block") {
  for (auto memory : {"i64:16(a)", "i32:8(a)"}) {
    auto parsed = serialization::parse_text (
      "m:module\nf:func i64, i64:o, i64:a\nlocal i64:x, i64:y\nmov x, i64:(a)\nmov y, "
      + std::string (memory) + "\nmov i64:(o), x\nmov i64:8(o), y\nret 0\nendfunc\nendmodule\n");
    REQUIRE (parsed.has_value ());
    CHECK (slp (parsed.value ()) == 0);
  }
  auto parsed = serialization::parse_text (
    "m:module\nf:func i64, i64:o, i64:a, i64:cond\nlocal i64:x,i64:y\nmov x,i64:(a)\nmov "
    "y,i64:8(a)\nmov i64:(o),x\nbt next,cond\nret 0\nnext:\nmov i64:8(o),y\nret "
    "0\nendfunc\nendmodule\n");
  REQUIRE (parsed.has_value ());
  CHECK (slp (parsed.value ()) == 0);
}
TEST_CASE (
  "SLP floating operations preserve signed zero and permit only arithmetic NaN differences") {
  auto exercise = []<class Float, class Bits> (Type type, std::array<Bits, 8> patterns) {
    for (auto operation : {Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::FDiv, Opcode::Neg}) {
      auto p = kernel (type, operation);
      auto original = ExecutionSession::create (p, {.mode = ExecutionMode::Interpreter});
      REQUIRE (original.has_value ());
      for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                          ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
        ExecutionOptions options;
        options.optimization = vector_options ();
        options.select_engine
          = [engine] (const Function &) -> Result<ExecutionEngine> { return engine; };
        auto compiled = ExecutionSession::create (p, options);
        REQUIRE (compiled.has_value ());
        for (unsigned trial = 0; trial < patterns.size (); ++trial) {
          std::array<std::byte, 16> a{}, b{};
          for (unsigned n = 0; n < 16 / sizeof (Float); ++n) {
            auto left = patterns[(trial + n) % patterns.size ()],
                 right = patterns[(trial + n + 3) % patterns.size ()];
            std::memcpy (a.data () + n * sizeof (Float), &left, sizeof (Float));
            std::memcpy (b.data () + n * sizeof (Float), &right, sizeof (Float));
          }
          auto expected = invoke (original.value (), a, b),
               actual = invoke (compiled.value (), a, b);
          for (unsigned n = 0; n < 16 / sizeof (Float); ++n) {
            Bits e, v;
            std::memcpy (&e, expected.data () + n * sizeof (Float), sizeof (Float));
            std::memcpy (&v, actual.data () + n * sizeof (Float), sizeof (Float));
            if (operation != Opcode::Neg && std::isnan (std::bit_cast<Float> (e)))
              CHECK (std::isnan (std::bit_cast<Float> (v)));
            else
              CHECK (v == e);
          }
        }
      }
    }
  };
  exercise.operator()<float, std::uint32_t> (Type (Type::Kind::F32),
                                             {0, 0x80000000u, 0x7fc01234u, 0xffc0abcd, 0x7f800000u,
                                              0xff800000u, 1, 0x3f800001u});
  exercise.operator()<double, std::uint64_t> (Type (Type::Kind::F64),
                                              {0, 0x8000000000000000ull, 0x7ff8000000001234ull,
                                               0xfff800000000abcdull, 0x7ff0000000000000ull,
                                               0xfff0000000000000ull, 1, 0x3ff0000000000001ull});
}
TEST_CASE ("SLP packs arithmetic trees constants and splats but never reassociates FP") {
  for (bool uniform : {false, true}) {
    Program p;
    MIRBuilder b (p);
    auto m = b.create_module ("m");
    b.create_function (m, "f",
                       {{Type::i64 ()},
                        {Type::pointer (), Type::pointer (), Type (Type::Kind::F32)}});
    auto out = b.argument (0), input = b.argument (1), scalar = b.argument (2);
    auto f32 = Type (Type::Kind::F32);
    std::array<Value, 4> values;
    for (unsigned n = 0; n < 4; ++n) {
      auto x = b.create_load (b.memory (f32, input, n * 4));
      values[n] = b.create_mul (b.create_add (x, uniform ? scalar : b.constant (float (n + 1))), x);
    }
    for (unsigned n = 0; n < 4; ++n) b.create_store (b.memory (f32, out, n * 4), values[n]);
    b.create_return (b.constant_i64 (0));
    require (b.finish ());
    auto result = optimize (p, vector_options ());
    INFO ((result.has_value () ? "" : result.error ().message));
    REQUIRE (result.has_value ());
    const auto &f = result.value ().find_function ("m", "f").value ();
    CAPTURE (uniform);
    INFO (result.value ().dump ());
    CHECK (count (f, Opcode::VAdd) == 1);
    CHECK (count (f, Opcode::VMul) == 1);
    CHECK (count (f, Opcode::VSplat) == unsigned (uniform));
    auto original = ExecutionSession::create (p, {.mode = ExecutionMode::Interpreter});
    auto compiled = ExecutionSession::create (std::move (result).value ());
    REQUIRE (original.has_value ());
    REQUIRE (compiled.has_value ());
    auto call = [] (const ExecutionSession &session) {
      std::array<float, 4> data{16777216.f, -16777216.f, 0.125f, -0.f}, out{};
      std::array<ExecutionArgument, 3> args{ScalarValue::pointer (
                                              reinterpret_cast<std::uintptr_t> (out.data ())),
                                            ScalarValue::pointer (
                                              reinterpret_cast<std::uintptr_t> (data.data ())),
                                            ScalarValue (1.f)};
      auto f = session.find_function ("m", "f");
      REQUIRE (f.has_value ());
      auto r = session.call (f.value (), args);
      REQUIRE (r.has_value ());
      return std::bit_cast<std::array<std::byte, 16>> (out);
    };
    CHECK (call (compiled.value ()) == call (original.value ()));
  }
}
TEST_CASE ("SLP vectorizes logical operations and floating division") {
  for (auto op : {Opcode::And, Opcode::Or, Opcode::Xor, Opcode::FDiv}) {
    auto p = kernel (Type (op == Opcode::FDiv ? Type::Kind::F32 : Type::Kind::I32), op);
    auto optimized = optimize (p, vector_options ());
    REQUIRE (optimized.has_value ());
    auto vector = op == Opcode::And   ? Opcode::VAnd
                  : op == Opcode::Or  ? Opcode::VOr
                  : op == Opcode::Xor ? Opcode::VXor
                                      : Opcode::VDiv;
    CHECK (count (optimized.value ().find_function ("m", "f").value (), vector) == 1);
  }
}
TEST_CASE ("VectorCombine may run independently without enabling scalar vectorization") {
  auto p = kernel (Type (Type::Kind::F32));
  auto result = optimize (p, {.vectorization = {.combine = true}});
  REQUIRE (result.has_value ());
  bool seen = false;
  for (auto &s : result.value ().statistics ()) {
    CHECK (s.pass != PassId::SLPVectorize);
    seen |= s.pass == PassId::VectorCombine;
  }
  CHECK (seen);
  CHECK (count (result.value ().find_function ("m", "f").value (), Opcode::VAdd) == 0);
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
TEST_CASE ("SLP touches exactly sixteen bytes and lowers without synthetic stack storage") {
  GuardedMemory input, output;
  auto a = input.data + input.page - 16, result = output.data + output.page - 16;
  std::fill_n (a, 16, std::byte (1));
  result[-1] = std::byte (0x55);
  for (auto kind : {Type::Kind::I8, Type::Kind::I16, Type::Kind::I32, Type::Kind::I64,
                    Type::Kind::F32, Type::Kind::F64}) {
    auto p = kernel (Type (kind));
    auto original = ExecutionSession::create (p, {.mode = ExecutionMode::Interpreter});
    REQUIRE (original.has_value ());
    std::array<std::byte, 16> input_bits;
    std::fill (input_bits.begin (), input_bits.end (), std::byte (1));
    auto expected = invoke (original.value (), input_bits, input_bits);
    for (auto engine : {ExecutionEngine::Interpreter, ExecutionEngine::EagerJIT,
                        ExecutionEngine::LazyJIT, ExecutionEngine::BlockVersioning}) {
      ExecutionOptions options;
      options.optimization = vector_options ();
      options.select_engine
        = [engine] (const Function &) -> Result<ExecutionEngine> { return engine; };
      options.link.compilation_observer
        = [] (const codegen::CompilationSnapshot &s) -> Result<void> {
        if (s.stage == codegen::CompilationStage::Machinize)
          CHECK (s.function.stack_slots ().empty ());
        return {};
      };
      auto session = ExecutionSession::create (p, options);
      REQUIRE (session.has_value ());
      auto f = session.value ().find_function ("m", "f");
      REQUIRE (f.has_value ());
      std::array<ExecutionArgument, 3>
        args{ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (result)),
             ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (a)),
             ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (a))};
      auto r = session.value ().call (f.value (), args);
      REQUIRE (r.has_value ());
      CHECK (result[-1] == std::byte (0x55));
      for (unsigned n = 0; n < 16; ++n) CHECK (result[n] == expected[n]);
    }
  }
}
TEST_CASE ("SLP matches equivalent dynamic indexed addresses after simplify") {
  Program p;
  MIRBuilder b (p);
  auto m = b.create_module ("m");
  b.create_function (m, "f", {{Type::i64 ()}, {Type::pointer (), Type::pointer (), Type::i64 ()}});
  auto out = b.argument (0), input = b.argument (1), index = b.argument (2);
  auto i32 = Type (Type::Kind::I32);
  std::array<Value, 4> values;
  for (unsigned n = 0; n < 4; ++n) values[n] = b.create_load (b.memory (i32, input, n * 4, index));
  for (unsigned n = 0; n < 4; ++n) b.create_store (b.memory (i32, out, n * 4, index), values[n]);
  b.create_return (b.constant_i64 (0));
  require (b.finish ());
  auto optimized = optimize (p, vector_options ());
  INFO ((optimized.has_value () ? "" : optimized.error ().message));
  REQUIRE (optimized.has_value ());
  auto &f = optimized.value ().find_function ("m", "f").value ();
  CHECK (count (f, Opcode::VMov) == 2);
  auto session = ExecutionSession::create (std::move (optimized).value ());
  REQUIRE (session.has_value ());
  for (unsigned offset : {0u, 4u, 8u, 12u}) {
    std::array<std::int32_t, 8> input_data{1, 2, 3, 4, 5, 6, 7, 8}, output{};
    std::array<std::int64_t, 3> args{static_cast<std::int64_t> (
                                       reinterpret_cast<std::uintptr_t> (output.data ())),
                                     static_cast<std::int64_t> (
                                       reinterpret_cast<std::uintptr_t> (input_data.data ())),
                                     offset};
    auto r = session.value ().call_i64 ("m", "f", args);
    REQUIRE (r.has_value ());
    for (unsigned n = 0; n < 8; ++n)
      CHECK (output[n] == (n >= offset / 4 && n < offset / 4 + 4 ? input_data[n] : 0));
  }
}
TEST_CASE ("SLP preserves word arithmetic semantics when the observable lane is wider") {
  for (auto operation : {"adds x,x,1\nadds y,y,1\n", "ext32 x,x\next32 y,y\n"}) {
    auto parsed = serialization::parse_text (
      "m:module\nf:func i64, i64:o, i64:a\nlocal i64:x,i64:y\nmov x,i64:(a)\nmov y,i64:8(a)\n"
      + std::string (operation) + "mov i64:(o),x\nmov i64:8(o),y\nret 0\nendfunc\nendmodule\n");
    REQUIRE (parsed.has_value ());
    CHECK (slp (parsed.value ()) == 0);
  }
}
TEST_CASE ("SLP keeps hardware-bound scalar identities") {
#if defined(__x86_64__) || defined(_M_X64)
  constexpr auto binding = "rdx";
#else
  constexpr auto binding = "r5";
#endif
  auto parsed = serialization::parse_text (
    "m:module\nf:func i64, i64:o, i64:a\nglobal i64:x:" + std::string (binding)
    + "\nlocal i64:y\nmov x,i64:(a)\nmov y,i64:8(a)\nmov i64:(o),x\nmov i64:8(o),y\nret 0\nendfunc\nendmodule\n");
  INFO ((parsed.has_value () ? "" : parsed.error ().message));
  REQUIRE (parsed.has_value ());
  CHECK (slp (parsed.value ()) == 0);
}
namespace {
struct InstructionCountAnalysis {
  using Result = std::size_t;
  static constexpr Dependency dependencies = Dependency::Instructions | Dependency::Registers;
  static mirnext::Result<Result> run (const Function &f) { return f.instruction_count (); }
};
}  // namespace
TEST_CASE ("vector edits retain CFG analysis identities and invalidate instruction caches") {
  auto p = kernel (Type (Type::Kind::F32));
  auto &f = function (p);
  FunctionAnalysisManager a (f);
  prepare (f, a);
  auto graph = a.get<FunctionGraphAnalysis> ();
  auto loops = a.get<LoopAnalysis> ();
  auto dominators = a.get<DominatorAnalysis> ();
  auto uses = a.get<InstructionCountAnalysis> ();
  REQUIRE (graph.has_value ());
  REQUIRE (loops.has_value ());
  REQUIRE (dominators.has_value ());
  REQUIRE (uses.has_value ());
  auto identity = &graph.value ();
  auto result = transforms::detail::run_pass (SLPVectorizePass (), f, a);
  REQUIRE (result.has_value ());
  CHECK (result.value ().transformations == 1);
  a.invalidate (result.value ().preserved);
  REQUIRE (a.get_cached<FunctionGraphAnalysis> ().has_value ());
  CHECK (&a.get_cached<FunctionGraphAnalysis> ().value () == identity);
  CHECK (a.get_cached<LoopAnalysis> ().has_value ());
  CHECK (a.get_cached<DominatorAnalysis> ().has_value ());
  CHECK_FALSE (a.get_cached<InstructionCountAnalysis> ().has_value ());
  require (f.ssa_form ().value ().verify ());
}
