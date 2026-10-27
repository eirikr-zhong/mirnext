/* Licensed under the MIT License. Warm generated-code throughput and compilation amortization. */
#include "mirnext/Execution/Linker.hpp"
#include "JITExecutionReference.hpp"
#include "../tests/Fixtures/ClassicPrograms.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
using namespace mirnext;
using namespace mirnext::execution;
using namespace mirnext::benchmark;
namespace {
using Clock = std::chrono::steady_clock;
double ns (Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::nano> (b - a).count ();
}
struct Workload {
  std::string name, module, function;
  std::unique_ptr<ir::Context> context;
  NativeFunction reference;
  std::uint64_t base, mask;
  std::vector<std::uint64_t> expected;
  std::uint64_t period_sum = 0;
  std::array<std::uint64_t, 5> calls{};
};
#if defined(_MSC_VER)
__declspec (noinline)
#else
__attribute__ ((noinline))
#endif
std::uint64_t batch (NativeFunction function, std::uint64_t base, std::uint64_t mask,
                     std::uint64_t count) {
  std::uint64_t checksum = 0;
  for (std::uint64_t n = 0; n < count; ++n) checksum += function (base + (n & mask));
  return checksum;
}
Result<Workload> workload (std::string name, std::string text, RuntimeWorkload kind,
                           std::uint64_t base = 0, std::uint64_t mask = 255,
                           std::string module = "bench", std::string function = "f") {
  auto context = serialization::parse_text (text);
  if (!context) return context.error ();
  Workload w{std::move (name),
             std::move (module),
             std::move (function),
             std::move (*context),
             reference_function (kind),
             base,
             mask};
  if (!w.reference) return Error{ErrorCode::InvalidArgument, "missing native runtime reference"};
  for (std::uint64_t n = 0; n <= mask; ++n) {
    auto value = w.reference (base + n);
    w.expected.push_back (value);
    w.period_sum += value;
  }
  return w;
}
Result<Workload> arithmetic (unsigned count) {
  std::string text = "bench:module\nf:func i64,i64:x\nlocal i64:r\nmov r,x\n";
  for (unsigned n = 0; n < count; ++n)
    text += "mul r,r,3\nxor r,r,x\nadd r,r," + std::to_string (n + 1) + "\n";
  return workload ("arithmetic_" + std::to_string (count), text + "ret r\nendfunc\nendmodule\n",
                   count == 64 ? RuntimeWorkload::Arithmetic64 : RuntimeWorkload::Arithmetic256);
}
Result<Workload> branches () {
  std::string text = "bench:module\nf:func i64,i64:x\nlocal i64:r,i64:t\nmov r,0\n";
  for (unsigned n = 0; n < 32; ++n) {
    auto id = std::to_string (n);
    text += "and t,x," + std::to_string (1u << (n % 8)) + "\nbt yes" + id
            + ",t\nadd r,r,1\njmp done" + id + "\nyes" + id + ":add r,r,2\ndone" + id + ":\n";
  }
  return workload ("branches_32", text + "ret r\nendfunc\nendmodule\n",
                   RuntimeWorkload::Branches32);
}
Result<void> sample (Workload &w, unsigned engine, int trial, double target_ns) {
  NativeFunction function = w.reference;
  std::optional<LinkedProgram> owner;
  double compilation_us = 0;
  std::size_t bytes = 0;
  if (engine < 4) {
    auto begin = Clock::now ();
    auto program
      = LinkedProgram::create (*w.context,
                               {.optimization
                                = {.level = static_cast<transforms::OptimizationLevel> (engine)}});
    auto end = Clock::now ();
    if (!program) return program.error ();
    compilation_us = ns (begin, end) / 1000;
    bytes = program->statistics ().code_bytes;
    owner.emplace (std::move (*program));
    auto entry = owner->find_function (w.module, w.function);
    if (!entry) return entry.error ();
    if (!entry->address ())
      return Error{ErrorCode::InvalidOperand, "missing runtime benchmark code"};
    function = reinterpret_cast<NativeFunction> (entry->address ());
  }
  // Validate all input patterns and warm the code/data/branch predictor before
  // measuring. Reference functions are compiled in a separate TU without LTO.
  for (std::size_t n = 0; n < w.expected.size (); ++n)
    if (function (w.base + n) != w.expected[n])
      return Error{ErrorCode::InvalidOperand, "wrong generated result: " + w.name};
  auto expected = [&] (std::uint64_t count) {
    auto sum = w.period_sum * (count / w.expected.size ());
    for (std::size_t n = 0; n < count % w.expected.size (); ++n) sum += w.expected[n];
    return sum;
  };
  auto run = [&] (std::uint64_t count) -> Result<double> {
    auto begin = Clock::now ();
    auto checksum = batch (function, w.base, w.mask, count);
    auto end = Clock::now ();
    if (checksum != expected (count))
      return Error{ErrorCode::InvalidOperand, "wrong batch checksum: " + w.name};
    return ns (begin, end);
  };
  auto &calls = w.calls[engine];
  if (!calls) {
    calls = 1;
    for (;;) {
      auto elapsed = run (calls);
      if (!elapsed) return elapsed.error ();
      if (*elapsed >= target_ns / 4) {
        calls
          = std::max<std::uint64_t> (1, static_cast<std::uint64_t> (calls * target_ns / *elapsed));
        break;
      }
      if (calls > UINT64_MAX / 4)
        return Error{ErrorCode::InvalidArgument, "runtime calibration overflow"};
      calls *= 4;
    }
  }
  auto elapsed = run (calls);
  if (!elapsed) return elapsed.error ();
  if (trial >= 0)
    std::cout << w.name << ',' << (engine == 4 ? "cpp" : "O" + std::to_string (engine)) << ','
              << trial << ',' << calls << ',' << *elapsed / 1e6 << ',' << *elapsed / calls << ','
              << compilation_us << ',' << bytes << '\n';
  return {};
}
}  // namespace
int main (int argc, char **argv) {
  unsigned trials = 31, warmups = 3, milliseconds = 10;
  std::string selected;
  for (int n = 1; n < argc; ++n) {
    std::string_view option (argv[n]);
    if ((option != "--trials" && option != "--warmups" && option != "--batch-ms"
         && option != "--workload")
        || ++n == argc)
      return 1;
    if (option == "--workload") {
      selected = argv[n];
      continue;
    }
    std::string_view text (argv[n]);
    unsigned value = 0;
    auto parsed = std::from_chars (text.data (), text.data () + text.size (), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data () + text.size () || value > 1000
        || (!value && option != "--warmups"))
      return 1;
    if (option == "--trials")
      trials = value;
    else if (option == "--warmups")
      warmups = value;
    else
      milliseconds = value;
  }
  std::vector<Workload> workloads;
  auto append = [&] (Result<Workload> w) {
    if (!w) {
      std::cerr << w.error ().message << '\n';
      return false;
    }
    workloads.push_back (std::move (*w));
    return true;
  };
  if (!append (workload ("tiny",
                         "bench:module\nf:func i64,i64:x\nlocal i64:r\nadd r,x,1\nret "
                         "r\nendfunc\nendmodule\n",
                         RuntimeWorkload::Tiny))
      || !append (workload ("checksum_loop_16384",
                            "bench:module\nf:func i64,i64:n\nlocal i64:r,i64:i\nmov r,42\nmov "
                            "i,0\nbge done,i,n\nloop:mul r,r,3\nxor r,r,i\nadd i,i,1\nblt "
                            "loop,i,n\ndone:ret r\nendfunc\nendmodule\n",
                            RuntimeWorkload::ChecksumLoop, 16384, 15)))
    return 1;
  auto sieve = test::classic_sieve_text (8190, 2);
  auto position = sieve.find ("func i64");
  if (position == sieve.npos) return 1;
  sieve.replace (position, 8, "func i64,i64:rounds");
  position = sieve.find ("iter, 2");
  if (position == sieve.npos) return 1;
  sieve.replace (position, 7, "iter, rounds");
  if (!append (workload ("sieve_8190", std::move (sieve), RuntimeWorkload::Sieve, 2, 1, "m_sieve",
                         "sieve"))
      || !append (arithmetic (64)) || !append (arithmetic (256)) || !append (branches ()))
    return 1;
  std::vector<std::uint64_t> memory (4096);
  std::uint64_t seed = 42;
  for (auto &value : memory) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    value = seed;
  }
  if (!append (
        workload ("memory_sum_4096",
                  "bench:module\nf:func i64,p:data\nlocal i64:r,i64:i\nmov r,0\nmov i,0\nloop:add "
                  "r,r,u64:(data,i,8)\nadd i,i,1\nblt loop,i,4096\nret r\nendfunc\nendmodule\n",
                  RuntimeWorkload::MemorySum, reinterpret_cast<std::uintptr_t> (memory.data ()),
                  0)))
    return 1;
  if (!selected.empty () && std::none_of (workloads.begin (), workloads.end (), [&] (auto &w) {
        return w.name == selected;
      }))
    return 1;
  auto target = target::TargetMachine::native ();
  if (!target) {
    std::cerr << target.error ().message << '\n';
    return 1;
  }
  std::cout << "# target=" << target->name () << " trials=" << trials << " warmups=" << warmups
            << " target_batch_ms=" << milliseconds
            << " reference=separate-TU-Release input=periodic-varying scalar_call=direct\n"
            << "workload,engine,trial,calls,batch_ms,ns_per_call,compile_us,code_bytes\n"
            << std::fixed << std::setprecision (4);
  for (int trial = -static_cast<int> (warmups); trial < static_cast<int> (trials); ++trial)
    for (auto &w : workloads)
      for (unsigned n = 0; n < 5; ++n) {
        if (!selected.empty () && selected != w.name) continue;
        auto engine = (n + static_cast<unsigned> (trial + warmups)) % 5;
        auto result = sample (w, engine, trial, double (milliseconds) * 1e6);
        if (!result) {
          std::cerr << w.name << " engine=" << engine << ": " << result.error ().message << '\n';
          return 1;
        }
      }
}
