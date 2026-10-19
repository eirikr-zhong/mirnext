/* Licensed under the MIT License. Data-heavy CFG and simplify costs. */
#include "mirnext/Analysis/ControlFlow.hpp"
#include "mirnext/Transforms/Simplify.hpp"
#include "mirnext/Serialization/Format.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
namespace {
using namespace mirnext; using namespace mirnext::ir;
constexpr unsigned repetitions = 7;
Result<std::unique_ptr<Context>> input(unsigned functions, unsigned data_items) {
  auto context = std::make_unique<Context>(); auto m = context->create_module("data-uses"); if (!m) return m.error();
  for (unsigned n = 0; n < functions; ++n) {
    auto f = (*m)->create_function("f" + std::to_string(n), {{Type::i64()}, {}}); if (!f) return f.error();
    auto entry = (*f)->create_block(), target = (*f)->create_block();
    if (!entry) return entry.error(); if (!target) return target.error();
    auto label = (*f)->create_label(**target); if (!label) return label.error();
    auto a = IRBuilder(**entry).ret(Operand(static_cast<std::int64_t>(n))), b = IRBuilder(**target).ret(Operand(static_cast<std::int64_t>(n + 1)));
    if (!a) return a.error(); if (!b) return b.error();
    if (n % 4 == 0) {
      auto address = (*m)->create_data("label" + std::to_string(n), LabelReference{*label, *label, 4}); if (!address) return address.error();
    }
    if (n % 8 == 0) {
      auto expression = (*m)->create_data("expression" + std::to_string(n), ExpressionData{*f}); if (!expression) return expression.error();
    }
  }
  for (unsigned n = 0; n < data_items; ++n) {
    auto data = (*m)->create_data("data" + std::to_string(n), ZeroData{8}); if (!data) return data.error();
  }
  auto checked = context->verify(); if (!checked) return checked.error(); return context;
}
double milliseconds(std::chrono::steady_clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); }
std::uint64_t checksum(const Context &context) {
  auto encoded = serialization::write_binary(context); if (!encoded) return 0;
  std::uint64_t hash = 14695981039346656037ULL;
  for (auto byte : *encoded) hash = (hash ^ std::to_integer<unsigned char>(byte)) * 1099511628211ULL;
  return hash;
}
}
int main() {
  for (auto [functions, data_items] : {std::pair{128u, 4096u}, std::pair{512u, 32768u}}) {
    auto source = input(functions, data_items); if (!source) { std::cerr << source.error().message << '\n'; return 1; }
    std::vector<double> graph_times, simplify_times;
    std::uint64_t expected_hash = 0; std::size_t expected_changes = 0;
    for (unsigned rep = 0; rep < repetitions; ++rep) {
      auto start = std::chrono::steady_clock::now(); std::size_t roots = 0; unsigned index = 0;
      for (auto &module : (*source)->modules()) for (auto &f : module.functions()) {
        auto graph = analysis::ControlFlowAnalysis::run(f); if (!graph) return 2;
        roots += graph->entry_roots.size();
        if (graph->blocks.size() != 2 || graph->reachable[1] != (index++ % 4 == 0)) return 2;
      }
      graph_times.push_back(milliseconds(std::chrono::steady_clock::now() - start));
      if (roots != functions + functions / 4) return 2;
      auto copied = clone(**source); if (!copied) return 3;
      transforms::FunctionPassManager simplify; simplify.add<transforms::SimplifyPass>(); std::size_t changes = 0;
      start = std::chrono::steady_clock::now();
      auto result = simplify.run(**copied, [&](const transforms::PassEvent &event) -> Result<void> {
        if (event.phase == transforms::PassEvent::Phase::After) changes += event.transformations; return {};
      });
      simplify_times.push_back(milliseconds(std::chrono::steady_clock::now() - start));
      if (!result) { std::cerr << result.error().message << '\n'; return 3; }
      auto hash = checksum(**copied); if (!hash || (rep && (hash != expected_hash || changes != expected_changes))) return 4;
      expected_hash = hash; expected_changes = changes;
    }
    std::ranges::sort(graph_times); std::ranges::sort(simplify_times);
    std::cout << "functions=" << functions << " ordinary_data=" << data_items << " repetitions=" << repetitions
      << " graph_median_ms=" << graph_times[repetitions / 2] << " simplify_median_ms=" << simplify_times[repetitions / 2]
      << " changes=" << expected_changes << " fingerprint=" << expected_hash << '\n';
  }
}
