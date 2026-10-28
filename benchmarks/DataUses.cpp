/* Licensed under the MIT License. Data-heavy CFG and simplify costs. */
#include "mirnext/Analysis/Access.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
namespace {
using namespace mirnext;
using namespace mirnext::ir;
constexpr unsigned repetitions = 7;
Result<Program> input (unsigned functions, unsigned data_items) {
  Program context;
  auto m = mirnext::ir::detail::access (context).create_module ("data-uses");
  if (!(m).has_value ()) return m.error ();
  for (unsigned n = 0; n < functions; ++n) {
    auto f = mirnext::ir::detail::access (((m)).value ())
               .create_function ("f" + std::to_string (n), {{Type::i64 ()}, {}});
    if (!(f).has_value ()) return f.error ();
    auto entry = mirnext::ir::detail::access (((f)).value ()).create_block (),
         target = mirnext::ir::detail::access (((f)).value ()).create_block ();
    if (!(entry).has_value ()) return entry.error ();
    if (!(target).has_value ()) return target.error ();
    auto label = mirnext::ir::detail::access (((f)).value ()).create_label ((target).value ());
    if (!(label).has_value ()) return label.error ();
    auto a = InstructionBuilder ((entry).value ()).ret (Operand (static_cast<std::int64_t> (n))),
         b
         = InstructionBuilder ((target).value ()).ret (Operand (static_cast<std::int64_t> (n + 1)));
    if (!(a).has_value ()) return a.error ();
    if (!(b).has_value ()) return b.error ();
    if (n % 4 == 0) {
      auto address
        = mirnext::ir::detail::access (((m)).value ())
            .create_data ("label" + std::to_string (n),
                          mirnext::ir::detail::NodeAccess::make_LabelReference (&(label).value (),
                                                                                &(label).value (),
                                                                                4));
      if (!(address).has_value ()) return address.error ();
    }
    if (n % 8 == 0) {
      auto expression
        = mirnext::ir::detail::access (((m)).value ())
            .create_data ("expression" + std::to_string (n),
                          mirnext::ir::detail::NodeAccess::make_ExpressionData (&(f).value ()));
      if (!(expression).has_value ()) return expression.error ();
    }
  }
  for (unsigned n = 0; n < data_items; ++n) {
    auto data = mirnext::ir::detail::access (((m)).value ())
                  .create_data ("data" + std::to_string (n), ZeroData{8});
    if (!(data).has_value ()) return data.error ();
  }
  auto checked = context.verify ();
  if (!(checked).has_value ()) return checked.error ();
  return context;
}
double milliseconds (std::chrono::steady_clock::duration duration) {
  return std::chrono::duration<double, std::milli> (duration).count ();
}
std::uint64_t checksum (const Program &context) {
  auto encoded = serialization::write_binary (context);
  if (!(encoded).has_value ()) return 0;
  std::uint64_t hash = 14695981039346656037ULL;
  for (auto byte : (encoded).value ())
    hash = (hash ^ std::to_integer<unsigned char> (byte)) * 1099511628211ULL;
  return hash;
}
}  // namespace
int main () {
  for (auto [functions, data_items] : {std::pair{128u, 4096u}, std::pair{512u, 32768u}}) {
    auto source = input (functions, data_items);
    if (!(source).has_value ()) {
      std::cerr << source.error ().message << '\n';
      return 1;
    }
    std::vector<double> graph_times, simplify_times;
    std::uint64_t expected_hash = 0;
    std::size_t expected_changes = 0;
    for (unsigned rep = 0; rep < repetitions; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      std::size_t roots = 0;
      unsigned index = 0;
      for (auto &module : source.value ().modules ())
        for (auto &f : module.functions ()) {
          auto graph = analysis::ControlFlowAnalysis::run (f);
          if (!(graph).has_value ()) return 2;
          roots += graph.value ().entry_roots.size ();
          if (mirnext::analysis::detail::NodeAccess::blocks (((graph)).value ()).size () != 2
              || graph.value ().reachable[1] != (index++ % 4 == 0))
            return 2;
        }
      graph_times.push_back (milliseconds (std::chrono::steady_clock::now () - start));
      if (roots != functions + functions / 4) return 2;
      auto copied = clone ((source).value ());
      if (!(copied).has_value ()) return 3;
      transforms::FunctionPassManager simplify;
      simplify.add<transforms::SimplifyPass> ();
      std::size_t changes = 0;
      start = std::chrono::steady_clock::now ();
      auto result = simplify.run ((copied).value (),
                                  [&] (const transforms::PassEvent &event) -> Result<void> {
                                    if (event.phase == transforms::PassEvent::Phase::After)
                                      changes += event.transformations;
                                    return {};
                                  });
      simplify_times.push_back (milliseconds (std::chrono::steady_clock::now () - start));
      if (!(result).has_value ()) {
        std::cerr << result.error ().message << '\n';
        return 3;
      }
      auto hash = checksum ((copied).value ());
      if (!hash || (rep && (hash != expected_hash || changes != expected_changes))) return 4;
      expected_hash = hash;
      expected_changes = changes;
    }
    std::ranges::sort (graph_times);
    std::ranges::sort (simplify_times);
    std::cout << "functions=" << functions << " ordinary_data=" << data_items
              << " repetitions=" << repetitions
              << " graph_median_ms=" << graph_times[repetitions / 2]
              << " simplify_median_ms=" << simplify_times[repetitions / 2]
              << " changes=" << expected_changes << " fingerprint=" << expected_hash << '\n';
  }
}
