/* Licensed under the MIT License. IR-to-executable latency, excluding parsing and execution. */
#include "mirnext/Execution/Session.hpp"
#include "../tests/Fixtures/ClassicPrograms.hpp"
#include <bit>
#include <array>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <iostream>
using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::execution;
namespace {
using Clock = std::chrono::steady_clock;
double us (Clock::time_point a, Clock::time_point b) {
  return std::chrono::duration<double, std::micro> (b - a).count ();
}
struct Workload {
  std::string name;
  Program context;
  std::int64_t expected;
  bool argument = true;
};
Result<Workload> parsed (std::string name, std::string text, std::int64_t expected,
                         bool argument = true) {
  auto context = serialization::parse_text (text);
  if (!(context).has_value ()) return context.error ();
  return Workload{std::move (name), std::move ((context).value ()), expected, argument};
}
Result<Workload> arithmetic (unsigned operations, unsigned functions = 1) {
  std::string text = "bench:module\n";
  std::uint64_t expected = 42;
  for (unsigned n = 0; n < operations; ++n) {
    expected *= 3;
    expected ^= 42;
    expected += n + 1;
  }
  for (unsigned n = 0; n < functions; ++n) {
    text += "f" + std::to_string (n) + ":func i64,i64:x\nlocal i64:r\nmov r,x\n";
    for (unsigned k = 0; k < operations; ++k)
      text += "mul r,r,3\nxor r,r,x\nadd r,r," + std::to_string (k + 1) + "\n";
    text += "ret r\nendfunc\n";
  }
  text += "endmodule\n";
  return parsed ("arithmetic_" + std::to_string (operations) + "x" + std::to_string (functions),
                 std::move (text), std::bit_cast<std::int64_t> (expected));
}
Result<Workload> branches (unsigned count) {
  std::string text = "bench:module\nf0:func i64,i64:x\nlocal i64:r,i64:t\nmov r,0\n";
  std::int64_t expected = 0;
  for (unsigned n = 0; n < count; ++n) {
    auto id = std::to_string (n);
    auto mask = 1u << (n % 8);
    text += "and t,x," + std::to_string (mask) + "\nbt yes" + id + ",t\nadd r,r,1\njmp done" + id
            + "\nyes" + id + ":add r,r,2\ndone" + id + ":\n";
    expected += (42 & mask) ? 2 : 1;
  }
  return parsed ("branches_" + std::to_string (count), text + "ret r\nendfunc\nendmodule\n",
                 expected);
}
Result<void> sample (const Workload &w, unsigned level, int trial) {
  // Source creation/verification, execution, and destruction are outside the
  // interval. Every iteration owns a fresh optimized and linked program; there
  // is no optimized-IR or generated-code cache shared across samples.
  std::size_t functions = 0, blocks = 0, instructions = 0;
  for (auto &m : w.context.modules ())
    for (auto &f : m.functions ()) {
      ++functions;
      for (auto &b : f.blocks ()) {
        ++blocks;
        for ([[maybe_unused]] auto &i : b.instructions ()) ++instructions;
      }
    }
  auto begin = Clock::now ();
  auto optimized
    = transforms::optimize (w.context,
                            {.level = static_cast<transforms::OptimizationLevel> (level)});
  auto middle = Clock::now ();
  if (!(optimized).has_value ()) return optimized.error ();
  auto program = ExecutionSession::create (std::move ((optimized).value ()));
  auto end = Clock::now ();
  if (!(program).has_value ()) return program.error ();
  if (program.value ().statistics ().compiled_functions != functions)
    return Error{ErrorCode::InvalidOperand, "benchmark did not eagerly compile all functions"};
  // Validate every generated function, not just the first one in the batch.
  for (auto &m : w.context.modules ())
    for (auto &f : m.functions ()) {
      auto function = program.value ().find_function (m.name (), f.symbol ().name ());
      if (!(function).has_value ()) return function.error ();
      const auto address = function.value ().address ();
      if (!address) return Error{ErrorCode::InvalidOperand, "missing executable entry"};
      const auto actual = w.argument
                            ? reinterpret_cast<std::int64_t (*) (std::int64_t)> (address) (42)
                            : reinterpret_cast<std::int64_t (*) ()> (address) ();
      if (actual != w.expected)
        return Error{ErrorCode::InvalidOperand, "wrong benchmark result: " + w.name};
    }
  if (trial >= 0)
    std::cout << w.name << ',' << level << ',' << trial << ',' << functions << ',' << blocks << ','
              << instructions << ',' << us (begin, middle) << ',' << us (middle, end) << ','
              << us (begin, end) << ',' << program.value ().statistics ().code_bytes << '\n';
  return {};
}
Result<void> checked_entry_sample (const Workload &w, unsigned level, int trial) {
  auto begin = Clock::now ();
  auto session
    = ExecutionSession::create (w.context,
                                {.optimization = transforms::OptimizationOptions{
                                   .level = static_cast<transforms::OptimizationLevel> (level)}});
  auto ready = Clock::now ();
  if (!(session).has_value ()) return session.error ();
  const double session_us = us (begin, ready);
  auto function = session.value ().find_function ("bench", "f0");
  if (!(function).has_value ()) return function.error ();
  const std::array<std::int64_t, 1> arguments{42};
  begin = Clock::now ();
  auto first = function.value ().call_i64 (arguments);
  auto end = Clock::now ();
  if (!(first).has_value ()) return first.error ();
  if ((first).value () != 43)
    return Error{ErrorCode::InvalidOperand, "wrong checked first-call result"};
  const double first_us = us (begin, end);
  // The extra interval contains adapter construction and one tiny execution;
  // unlike total_us above, it is deliberately a first-call measurement.
  begin = Clock::now ();
  std::int64_t sum = 0;
  constexpr unsigned cached_calls = 1000;
  for (unsigned n = 0; n < cached_calls; ++n) {
    auto value = function.value ().call_i64 (arguments);
    if (!(value).has_value ()) return value.error ();
    sum += (value).value ();
  }
  end = Clock::now ();
  if (sum != 43 * cached_calls || session.value ().host_entry_count () != 1)
    return Error{ErrorCode::InvalidOperand, "checked adapter was not reused"};
  if (trial >= 0)
    std::cout << level << ',' << trial << ',' << session_us << ',' << first_us << ','
              << us (begin, end) / cached_calls << ',' << session.value ().host_entry_count ()
              << '\n';
  return {};
}
}  // namespace
int main (int argc, char **argv) {
  unsigned trials = 31, warmups = 3;
  bool checked_entry = false;
  std::optional<unsigned> selected_level;
  std::string selected_workload;
  for (int n = 1; n < argc; ++n) {
    std::string_view option (argv[n]);
    if (option == "--checked-entry") {
      checked_entry = true;
      continue;
    }
    if ((option != "--trials" && option != "--warmups" && option != "--level"
         && option != "--workload")
        || ++n == argc)
      return 1;
    if (option == "--workload") {
      selected_workload = argv[n];
      continue;
    }
    std::string_view value (argv[n]);
    unsigned number = 0;
    auto result = std::from_chars (value.data (), value.data () + value.size (), number);
    if (result.ec != std::errc{} || result.ptr != value.data () + value.size ()
        || (!number && option == "--trials") || number > 1000)
      return 1;
    if (option == "--level") {
      if (number > 3) return 1;
      selected_level = number;
      continue;
    }
    (option == "--trials" ? trials : warmups) = number;
  }
  std::vector<Workload> workloads;
  auto append = [&] (Result<Workload> w) {
    if (!(w).has_value ()) {
      std::cerr << w.error ().message << '\n';
      return false;
    }
    workloads.push_back (std::move ((w).value ()));
    return true;
  };
  if (!append (parsed ("tiny",
                       "bench:module\nf0:func i64,i64:x\nlocal i64:r\nadd r,x,1\nret "
                       "r\nendfunc\nendmodule\n",
                       43)))
    return 1;
  auto loop = test::classic_loop ();
  if (!(loop).has_value ()) {
    std::cerr << loop.error ().message << '\n';
    return 1;
  }
  workloads.push_back ({"loop", std::move ((loop).value ()), 42});
  if (!append (parsed ("sieve", test::classic_sieve_text (127, 2), 31, false))
      || !append (arithmetic (64)) || !append (arithmetic (256)) || !append (branches (32))
      || !append (arithmetic (8, 64)))
    return 1;
  if (!selected_workload.empty ()) {
    bool found = false;
    for (auto &w : workloads)
      if (w.name == selected_workload) found = true;
    if (!found || (checked_entry && selected_workload != "tiny")) return 1;
  }
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) {
    std::cerr << target.error ().message << '\n';
    return 1;
  }
  std::cout << "# target=" << target.value ().name () << " trials=" << trials
            << " warmups=" << warmups << '\n';
  if (checked_entry)
    std::cout << "level,trial,session_create_us,first_checked_call_us,cached_checked_call_us,host_"
                 "entries\n";
  else
    std::cout << "workload,level,trial,functions,blocks,ir_instructions,optimize_us,backend_link_"
                 "publish_us,total_us,code_bytes\n";
  std::cout << std::fixed << std::setprecision (3);
  // Rotate levels within every round to avoid always measuring O0 cold and
  // O3 after thermal drift. Timings are serial, with no parallel compiler work.
  for (int round = -static_cast<int> (warmups); round < static_cast<int> (trials); ++round)
    for (auto &w : workloads)
      for (unsigned n = 0; n < 4; ++n) {
        unsigned level = (n + static_cast<unsigned> (round + warmups)) % 4;
        if (selected_level && level != *selected_level) continue;
        if (!selected_workload.empty () && w.name != selected_workload) continue;
        if (checked_entry && &w != &workloads.front ()) continue;
        auto result
          = checked_entry ? checked_entry_sample (w, level, round) : sample (w, level, round);
        if (!(result).has_value ()) {
          std::cerr << w.name << " O" << level << ": " << result.error ().message << '\n';
          return 1;
        }
      }
}
