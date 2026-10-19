/* Licensed under the MIT License. SSA timings are informational, not test thresholds. */
#include "mirnext/Transforms/SSA.hpp"
#include "mirnext/Transforms/Address.hpp"
#include "mirnext/Analysis/FunctionGraph.hpp"
#include <chrono>
#include <iostream>

using namespace mirnext;
using namespace mirnext::ir;
using namespace mirnext::analysis;
using namespace mirnext::transforms;
namespace {
using Clock = std::chrono::steady_clock;
double microseconds(Clock::duration elapsed) {
  return std::chrono::duration<double, std::micro>(elapsed).count();
}
bool measure(Function &function, std::string_view shape, unsigned count) {
  FunctionAnalysisManager analyses(function);
  if (!analyses.get<FunctionGraphAnalysis>()) return false;
  auto start = Clock::now();
  if (!BuildSSAPass().run(function, analyses)) return false;
  auto built = Clock::now();
  constexpr unsigned repetitions = 5;
  for (unsigned n = 0; n < repetitions; ++n) if (!function.ssa_form()->verify()) return false;
  auto verified = Clock::now();
  std::size_t definitions = 0, uses = 0;
  for (auto &value : function.ssa_form()->values()) definitions += value.alive();
  for (auto &use : function.ssa_form()->uses()) uses += use.alive();
  if (shape == "fanout" && (definitions != count + 2 || uses != 2 * count + 1)) return false;
  if (shape == "call-results" && (definitions != count || uses != count)) return false;
  std::cout << "shape=" << shape << " count=" << count << " definitions=" << definitions << " uses=" << uses
    << " build_us=" << microseconds(built - start)
    << " verify_us=" << microseconds(verified - built) / repetitions << '\n';
  if (!ConventionalSSAPass().run(function, analyses) || !DestroySSAPass().run(function, analyses)) return false;
  return true;
}
}
int main() {
  for (unsigned count : {4096, 16384, 65536}) {
    Context context;
    auto module = context.create_module("fanout"); if (!module) return 1;
    auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return 1;
    auto block = (*function)->create_block(); if (!block) return 1;
    auto source = (*function)->create_register(Type::i64()), value = (*function)->create_register(Type::i64());
    if (!source || !value) return 1;
    IRBuilder body(**block);
    if (!body.assign(**source, 1) || !body.assign(**source, 42)) return 1;
    for (unsigned n = 0; n < count; ++n)
      if (!body.emit(Opcode::Add, {Operand(**value), Operand(**source), Operand(**source)})) return 1;
    if (!body.ret(Operand(**value)) || !measure(**function, "fanout", count)) return 1;
  }
  for (unsigned count : {64, 256, 1024}) {
    Context context;
    auto module = context.create_module("wide"); if (!module) return 1;
    Signature signature; signature.results.assign(count, Type::i64());
    auto target = (*module)->import("target", signature); if (!target) return 1;
    auto function = (*module)->create_function("entry", signature); if (!function) return 1;
    auto block = (*function)->create_block(); if (!block) return 1;
    IRBuilder body(**block); auto call = body.call(**target); if (!call) return 1;
    std::vector<Operand> returned;
    for (auto reg : call->results()) returned.emplace_back(*reg);
    if (!body.emit(Opcode::Ret, returned) || !measure(**function, "call-results", count)) return 1;
  }
  for (unsigned count : {1024, 4096, 16384}) {
    Context context; auto module = context.create_module("address"); if (!module) return 1;
    auto function = (*module)->create_function("entry", {{Type::i64()}, {}}); if (!function) return 1;
    auto block = (*function)->create_block(); if (!block) return 1; IRBuilder body(**block);
    auto source = (*function)->create_register(Type::i64()), pointer = (*function)->create_register(Type::i64()), value = (*function)->create_register(Type::i64());
    if (!source || !pointer || !value || !body.assign(**source, 42)) return 1;
    if (!body.emit(Opcode::Addr, {Operand(**pointer), Operand(**source)})) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto copy = (*function)->create_register(Type::i64()); if (!copy) return 1;
      if (!body.assign(**copy, Operand(**pointer)) || !body.emit(Opcode::Mov, {Operand(**value), Operand(Memory{Type::i64(), *copy})})) return 1;
    }
    if (!body.ret(Operand(**value))) return 1;
    FunctionAnalysisManager analyses(**function);
    if (!BuildSSAPass(false).run(**function, analyses)) return 1;
    auto start = Clock::now(); auto result = AddressTransformPass().run(**function, analyses); auto end = Clock::now();
    if (!result || result->transformations != count + 1 || (*function)->ssa_form() || !(*function)->verify()) return 1;
    unsigned replacements = 0;
    for (auto &instruction : (*block)->instructions()) {
      if (instruction.opcode() == Opcode::Addr) return 1;
      for (auto &operand : instruction.operands()) if (operand.get<Memory>()) return 1;
      if (instruction.opcode() == Opcode::Mov && *instruction.operands()[0].get<const Register *>() == *value
          && instruction.operands()[1].get<const Register *>() && *instruction.operands()[1].get<const Register *>() == *source) ++replacements;
    }
    if (replacements != count) return 1;
    std::cout << "shape=address-copies count=" << count << " replacements=" << replacements
      << " lower_us=" << microseconds(end - start) << '\n';
  }
}
