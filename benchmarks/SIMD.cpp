/* Licensed under the MIT License. v128 native lowering latency and throughput. */
#include <mirnext/Execution/Session.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
using Clock = std::chrono::steady_clock;
using Entry = void (*) (void *, const void *, const void *, std::int64_t);
double elapsed (Clock::time_point start) {
  return std::chrono::duration<double, std::nano> (Clock::now () - start).count ();
}
int main () {
  std::cout << "workload,level,compile_us,code_bytes,machine_instructions,stack_accesses,ns_per_"
               "iteration\n";
  std::array<std::array<std::byte, 16>, 3> vector_results{};
  for (unsigned workload = 0; workload < 9; ++workload) {
    const unsigned task = workload < 6 ? workload : workload - 3;
    const bool scalar_reference = workload >= 6;
    Program program;
    MIRBuilder b (program);
    auto module = b.create_module ("simd");
    auto type = task == 0 || task == 4   ? Type::vector128 (Type::VectorElement::U64, 2)
                : task == 1              ? Type::vector128 (Type::VectorElement::U8, 16)
                : task == 3 || task == 5 ? Type::vector128 (Type::VectorElement::I16, 8)
                                         : Type::vector128 (Type::VectorElement::U32, 4);
    b.create_function<void (void *, const void *, const void *, std::int64_t)> (module, "f");
    Value a, c, value;
    std::vector<Value> scalar_values, scalar_constants;
    if (!scalar_reference) {
      a = b.create_load (b.memory (type, b.argument (1)));
      c = b.create_load (b.memory (type, b.argument (2)));
      value = b.create_register (type);
      b.create_assign (value, a);
    } else {
      for (unsigned lane = 0; lane < type.vector_lanes (); ++lane) {
        auto offset = lane * (16 / type.vector_lanes ());
        auto v = b.create_register (Type::i64 ());
        b.create_assign (v, b.create_convert (b.create_load (b.memory (type.vector_lane_type (),
                                                                       b.argument (1), offset)),
                                              Type::i64 ()));
        scalar_values.push_back (v);
        scalar_constants.push_back (
          b.create_convert (b.create_load (
                              b.memory (type.vector_lane_type (), b.argument (2), offset)),
                            Type::i64 ()));
      }
    }
    auto index = b.create_register (Type::i64 ());
    b.create_assign (index, b.constant_i64 (0));
    auto loop = b.create_block (), body = b.create_block (), exit = b.create_block ();
    b.create_jump (loop);
    b.set_insert_point (loop);
    b.create_cond_branch (b.create_ge (index, b.argument (3)), exit);
    b.set_insert_point (body);
    auto choose = [&] (Value condition, Value yes, Value no) {
      auto mask = b.create_sub (b.constant_i64 (0), b.create_convert (condition, Type::i64 ()));
      return b.create_or (b.create_and (yes, mask), b.create_and (no, b.create_bit_not (mask)));
    };
    if (scalar_reference) {
      for (unsigned lane = 0; lane < type.vector_lanes (); ++lane) {
        auto v = scalar_values[lane], k = scalar_constants[lane];
        auto next = b.create_add (v, k);
        if (task == 4)
          next = b.create_mul (next, k);
        else if (task == 3) {
          next = b.create_convert (b.create_convert (next, type.vector_lane_type ()), Type::i64 ());
          next = b.create_shr (b.create_add (b.create_mul (next, k), b.constant_i64 (16384)),
                               b.constant_i64 (15));
          next = choose (b.create_gt (next, b.constant_i64 (32767)), b.constant_i64 (32767), next);
        } else {
          next = choose (b.create_gt (next, b.constant_i64 (32767)), b.constant_i64 (32767), next);
          next
            = choose (b.create_lt (next, b.constant_i64 (-32768)), b.constant_i64 (-32768), next);
        }
        b.create_assign (v, b.create_convert (b.create_convert (next, type.vector_lane_type ()),
                                              Type::i64 ()));
      }
    } else {
      auto next = b.create_add (value, c);
      if (task == 0)
        next = b.create_xor (next, b.create_gt (next, a));
      else if (task == 1)
        next
          = b.create_shuffle (next, c, {31, 0, 17, 2, 19, 4, 21, 6, 23, 8, 25, 10, 27, 12, 29, 14});
      else if (task == 3)
        next = b.create_q15mulr_sat (next, c);
      else if (task == 4)
        next = b.create_mul (next, c);
      else if (task == 5)
        next = b.create_add_sat (value, c);
      else {
        auto scalar = b.create_extract_lane (next, 3);
        next = b.create_insert_lane (next, b.create_extract_lane (c, 1), 2);
        next = b.create_xor (next, b.create_vector_splat (type, scalar));
      }
      b.create_assign (value, next);
    }
    b.create_assign (index, b.create_add (index, b.constant_i64 (1)));
    b.create_jump (loop);
    b.set_insert_point (exit);
    if (scalar_reference) {
      for (unsigned lane = 0; lane < type.vector_lanes (); ++lane)
        b.create_store (b.memory (type.vector_lane_type (), b.argument (0),
                                  lane * (16 / type.vector_lanes ())),
                        b.create_convert (scalar_values[lane], type.vector_lane_type ()));
    } else
      b.create_store (b.memory (type, b.argument (0)), value);
    b.create_return ();
    auto built = b.finish ();
    if (!built.has_value ()) {
      std::cerr << built.error ().message << '\n';
      return 1;
    }
    std::array<std::byte, 16> a_bytes{}, c_bytes{}, expected{}, actual{};
    for (unsigned n = 0; n < 16; ++n) {
      a_bytes[n] = std::byte (17 * n + 3);
      c_bytes[n] = std::byte (11 * n + 9);
    }
    auto reference = ExecutionSession::create (program, {.mode = ExecutionMode::Interpreter});
    if (!reference.has_value ()) return 2;
    auto ref = reference.value ().find_function ("simd", "f");
    if (!ref.has_value ()) return 3;
    std::array<ExecutionArgument, 4>
      arguments{ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (expected.data ())),
                ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (a_bytes.data ())),
                ScalarValue::pointer (reinterpret_cast<std::uintptr_t> (c_bytes.data ())),
                std::int64_t (128)};
    if (!reference.value ().call (ref.value (), arguments).has_value ()) return 4;
    if (task >= 3) {
      if (scalar_reference && expected != vector_results[task - 3]) return 9;
      if (!scalar_reference) vector_results[task - 3] = expected;
    }
    for (unsigned level = 0; level < 4; ++level) {
      std::vector<double> compilation, runtime;
      std::size_t bytes = 0, instructions = 0, stack_accesses = 0;
      for (unsigned trial = 0; trial < 34; ++trial) {
        ExecutionOptions options;
        options.optimization = transforms::OptimizationOptions{
          .level = static_cast<transforms::OptimizationLevel> (level)};
        options.link.compilation_observer
          = [&] (const codegen::CompilationSnapshot &snapshot) -> Result<void> {
          if (snapshot.stage != codegen::CompilationStage::Frame
              || snapshot.function.name () != "f")
            return {};
          instructions = snapshot.instruction_count ();
          std::size_t memory_instructions = 0;
          for (auto &block : snapshot.function.blocks ())
            for (auto &instruction : block.instructions ())
              memory_instructions += std::ranges::any_of (instruction.operands (), [] (auto &o) {
                return o.template get<codegen::MachineMemory> ().has_value ();
              });
          // Each vector kernel has two user loads and one store; the scalar
          // reference has these accesses per lane. Other body memory instructions
          // access compiler-created stack storage,
          // including accesses through derived addresses rather than SP/FP.
          const unsigned user_accesses = scalar_reference ? type.vector_lanes () * 3 : 3;
          if (memory_instructions < user_accesses)
            return Error{ErrorCode::InvalidOperand,
                         "benchmark kernel lost its user memory accesses"};
          stack_accesses = memory_instructions - user_accesses;
          return {};
        };
        auto start = Clock::now ();
        auto session = ExecutionSession::create (program, options);
        auto duration = elapsed (start);
        if (!session.has_value ()) {
          std::cerr << session.error ().message;
          return 5;
        }
        auto f = session.value ().find_function ("simd", "f");
        if (!f.has_value () || !f.value ().address ()) return 6;
        auto entry = reinterpret_cast<Entry> (f.value ().address ());
        entry (actual.data (), a_bytes.data (), c_bytes.data (), 128);
        if (actual != expected) return 7;
        start = Clock::now ();
        for (unsigned repeat = 0; repeat < 100000; ++repeat)
          entry (actual.data (), a_bytes.data (), c_bytes.data (), 128);
        auto time = elapsed (start) / 12800000;
        if (actual != expected) return 8;
        if (trial >= 3) {
          compilation.push_back (duration / 1000);
          runtime.push_back (time);
        }
        bytes = session.value ().statistics ().code_bytes;
      }
      std::ranges::sort (compilation);
      std::ranges::sort (runtime);
      std::cout
        << std::array{"compare_u64",    "shuffle_u8",          "lanes_u32",
                      "q15_i16",        "multiply_u64",        "saturate_i16",
                      "q15_i16_scalar", "multiply_u64_scalar", "saturate_i16_scalar"}[workload]
        << ',' << level << ',' << compilation[compilation.size () / 2] << ',' << bytes << ','
        << instructions << ',' << stack_accesses << ',' << runtime[runtime.size () / 2] << '\n';
    }
  }
}
