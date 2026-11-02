/* Licensed under the MIT License. Opt-in SLP compilation and checked native throughput. */
#include <mirnext/Execution/Session.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
using Clock = std::chrono::steady_clock;
using Entry = void (*) (void *, const void *, const void *, std::int64_t);
namespace {
double ns (Clock::time_point start) {
  return std::chrono::duration<double, std::nano> (Clock::now () - start).count ();
}
double median (std::vector<double> samples) {
  std::ranges::sort (samples);
  return samples[samples.size () / 2];
}
Program kernel (Type type, bool multiply, unsigned bytes) {
  Program p;
  MIRBuilder b (p);
  auto module = b.create_module ("slp");
  b.create_function<void (void *, const void *, const void *, std::int64_t)> (module, "f");
  auto output = b.argument (0), x = b.argument (1), y = b.argument (2), end = b.argument (3);
  auto offset = b.create_register (Type::i64 ());
  b.create_assign (offset, b.constant_i64 (0));
  auto check = b.create_block (), body = b.create_block (), exit = b.create_block ();
  b.create_jump (check);
  b.set_insert_point (check);
  b.create_cond_branch (b.create_ge (offset, end), exit);
  b.set_insert_point (body);
  std::vector<Value> results;
  for (unsigned n = 0; n < 16 / bytes; ++n) {
    auto a = b.create_load (b.memory (type, x, n * bytes, offset));
    auto c = b.create_load (b.memory (type, y, n * bytes, offset));
    results.push_back (multiply ? b.create_mul (a, c) : b.create_add (a, c));
  }
  for (unsigned n = 0; n < results.size (); ++n)
    b.create_store (b.memory (type, output, n * bytes, offset), results[n]);
  b.create_assign (offset, b.create_add (offset, b.constant_i64 (16)));
  b.create_jump (check);
  b.set_insert_point (exit);
  b.create_return ();
  if (auto built = b.finish (); !built.has_value ()) {
    std::cerr << built.error ().message;
    std::abort ();
  }
  return p;
}
}  // namespace
int main () {
  std::cout << "workload,level,slp,packs,optimize_us,total_prepare_us,code_bytes,ns_per_chunk\n";
  constexpr unsigned chunks = 128, iterations = 2000;
  for (unsigned workload = 0; workload < 6; ++workload) {
    auto type = workload < 2    ? Type (Type::Kind::F32)
                : workload == 2 ? Type (Type::Kind::F64)
                : workload == 3 ? Type (Type::Kind::I32)
                : workload == 4 ? Type (Type::Kind::I16)
                                : Type::i64 ();
    unsigned bytes = workload == 4 ? 2 : workload == 2 || workload == 5 ? 8 : 4;
    bool multiply = workload == 1 || workload == 2 || workload >= 4;
    auto program = kernel (type, multiply, bytes);
    std::array<std::byte, chunks * 16> x{}, y{}, expected{}, actual{};
    for (unsigned n = 0; n < x.size () / bytes; ++n) {
      if (type.is_float ()) {
        if (bytes == 4) {
          float a = float (n % 17) / 8, c = float (n % 7) / 4;
          std::memcpy (x.data () + n * bytes, &a, bytes);
          std::memcpy (y.data () + n * bytes, &c, bytes);
        } else {
          double a = double (n % 17) / 8, c = double (n % 7) / 4;
          std::memcpy (x.data () + n * bytes, &a, bytes);
          std::memcpy (y.data () + n * bytes, &c, bytes);
        }
      } else {
        std::uint64_t a = n * 101 + 1, c = n * 39 + 3;
        std::memcpy (x.data () + n * bytes, &a, bytes);
        std::memcpy (y.data () + n * bytes, &c, bytes);
      }
    }
    auto reference = ExecutionSession::create (program, {.mode = ExecutionMode::Interpreter});
    if (!reference.has_value ()) return 1;
    auto f = reference.value ().find_function ("slp", "f");
    if (!f.has_value ()) return 2;
    std::array<ExecutionArgument, 4>
      args{ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (expected.data ())),
           ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (x.data ())),
           ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (y.data ())),
           std::int64_t (x.size ())};
    if (!reference.value ().call (f.value (), args).has_value ()) return 3;
    for (auto level : {transforms::OptimizationLevel::O2, transforms::OptimizationLevel::O3})
      for (bool slp : {false, true}) {
        std::vector<double> optimization, preparation, runtime;
        std::size_t packs = 0, code = 0;
        for (unsigned trial = 0; trial < 18; ++trial) {
          transforms::OptimizationOptions options;
          options.level = level;
          options.vectorization.slp = slp;
          auto start = Clock::now ();
          auto optimized = transforms::optimize (program, options);
          auto optimize_time = ns (start);
          if (!optimized.has_value ()) {
            std::cerr << optimized.error ().message;
            return 4;
          }
          packs = 0;
          for (auto &stat : optimized.value ().statistics ())
            if (stat.pass == transforms::PassId::SLPVectorize) packs += stat.transformations;
          auto session = ExecutionSession::create (std::move (optimized).value ());
          auto prepare_time = ns (start);
          if (!session.has_value ()) {
            std::cerr << session.error ().message;
            return 5;
          }
          auto function = session.value ().find_function ("slp", "f");
          if (!function.has_value ()) return 6;
          auto native = session.value ().native_entry (function.value ());
          if (!native.has_value ()) return 7;
          auto entry = reinterpret_cast<Entry> (native.value ().address ());
          entry (actual.data (), x.data (), y.data (), x.size ());
          if (actual != expected) return 8;
          start = Clock::now ();
          for (unsigned n = 0; n < iterations; ++n)
            entry (actual.data (), x.data (), y.data (), x.size ());
          auto time = ns (start) / (iterations * chunks);
          if (actual != expected) return 9;
          if (trial >= 3) {
            optimization.push_back (optimize_time / 1000);
            preparation.push_back (prepare_time / 1000);
            runtime.push_back (time);
          }
          code = session.value ().statistics ().code_bytes;
        }
        std::cout
          << std::array{"add_f32", "mul_f32", "mul_f64", "add_i32", "mul_i16", "mul_i64"}[workload]
          << ',' << unsigned (level) << ',' << slp << ',' << packs << ',' << median (optimization)
          << ',' << median (preparation) << ',' << code << ',' << median (runtime) << '\n';
      }
  }
}
