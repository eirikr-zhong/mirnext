/* Licensed under the MIT License. SSA timings are informational, not test thresholds. */
#include "mirnext/Transforms/Testing.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
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
double microseconds (Clock::duration elapsed) {
  return std::chrono::duration<double, std::micro> (elapsed).count ();
}
bool measure (Function &function, std::string_view shape, unsigned count) {
  FunctionAnalysisManager analyses (function);
  if (!(analyses.get<FunctionGraphAnalysis> ()).has_value ()) return false;
  auto start = Clock::now ();
  if (!(::mirnext::transforms::detail::run_pass (BuildSSAPass (), function, analyses)).has_value ())
    return false;
  auto built = Clock::now ();
  constexpr unsigned repetitions = 5;
  for (unsigned n = 0; n < repetitions; ++n)
    if (!(function.ssa_form ().get ()->verify ()).has_value ()) return false;
  auto verified = Clock::now ();
  std::size_t definitions = 0, uses = 0;
  for (auto &value : function.ssa_form ().get ()->values ()) definitions += value.alive ();
  for (auto &use : function.ssa_form ().get ()->uses ()) uses += use.alive ();
  if (shape == "fanout" && (definitions != count + 2 || uses != 2 * count + 1)) return false;
  if (shape == "call-results" && (definitions != count || uses != count)) return false;
  std::cout << "shape=" << shape << " count=" << count << " definitions=" << definitions
            << " uses=" << uses << " build_us=" << microseconds (built - start)
            << " verify_us=" << microseconds (verified - built) / repetitions << '\n';
  if (!(::mirnext::transforms::detail::run_pass (ConventionalSSAPass (), function, analyses))
         .has_value ()
      || !(::mirnext::transforms::detail::run_pass (DestroySSAPass (), function, analyses))
            .has_value ())
    return false;
  return true;
}
}  // namespace
int main () {
  for (unsigned count : {4096, 16384, 65536}) {
    Program context;
    auto module = mirnext::ir::detail::access (context).create_module ("fanout");
    if (!(module).has_value ()) return 1;
    auto function = mirnext::ir::detail::access (((module)).value ())
                      .create_function ("entry", {{Type::i64 ()}, {}});
    if (!(function).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    auto source
      = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()),
      value = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
    if (!(source).has_value () || !(value).has_value ()) return 1;
    InstructionBuilder body ((block).value ());
    if (!(body.assign ((source).value (), 1)).has_value ()
        || !(body.assign ((source).value (), 42)).has_value ())
      return 1;
    for (unsigned n = 0; n < count; ++n)
      if (!(body.emit (Opcode::Add, {Operand ((value).value ()), Operand ((source).value ()),
                                     Operand ((source).value ())}))
             .has_value ())
        return 1;
    if (!(body.ret (Operand ((value).value ()))).has_value ()
        || !measure ((function).value (), "fanout", count))
      return 1;
  }
  for (unsigned count : {64, 256, 1024}) {
    Program context;
    auto module = mirnext::ir::detail::access (context).create_module ("wide");
    if (!(module).has_value ()) return 1;
    Signature signature;
    signature.results.assign (count, Type::i64 ());
    auto target = mirnext::ir::detail::access (((module)).value ()).import ("target", signature);
    if (!(target).has_value ()) return 1;
    auto function
      = mirnext::ir::detail::access (((module)).value ()).create_function ("entry", signature);
    if (!(function).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder body ((block).value ());
    auto call = body.call ((target).value ());
    if (!(call).has_value ()) return 1;
    std::vector<Operand> returned;
    for (auto reg : mirnext::ir::detail::ReferenceAccess::pointers (call.value ().results ()))
      returned.emplace_back (*reg);
    if (!(body.emit (Opcode::Ret, returned)).has_value ()
        || !measure ((function).value (), "call-results", count))
      return 1;
  }
  for (unsigned count : {1024, 4096, 16384}) {
    Program context;
    auto module = mirnext::ir::detail::access (context).create_module ("address");
    if (!(module).has_value ()) return 1;
    auto function = mirnext::ir::detail::access (((module)).value ())
                      .create_function ("entry", {{Type::i64 ()}, {}});
    if (!(function).has_value ()) return 1;
    auto block = mirnext::ir::detail::access (((function)).value ()).create_block ();
    if (!(block).has_value ()) return 1;
    InstructionBuilder body ((block).value ());
    auto source
      = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()),
      pointer = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ()),
      value = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
    if (!(source).has_value () || !(pointer).has_value () || !(value).has_value ()
        || !(body.assign ((source).value (), 42)).has_value ())
      return 1;
    if (!(body.emit (Opcode::Addr, {Operand ((pointer).value ()), Operand ((source).value ())}))
           .has_value ())
      return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto copy
        = mirnext::ir::detail::access (((function)).value ()).create_register (Type::i64 ());
      if (!(copy).has_value ()) return 1;
      if (!(body.assign ((copy).value (), Operand ((pointer).value ()))).has_value ()
          || !(body.emit (Opcode::Mov,
                          {Operand ((value).value ()),
                           Operand (
                             mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                           &(copy).value ()))}))
                .has_value ())
        return 1;
    }
    if (!(body.ret (Operand ((value).value ()))).has_value ()) return 1;
    FunctionAnalysisManager analyses ((function).value ());
    if (!(::mirnext::transforms::detail::run_pass (BuildSSAPass (false), (function).value (),
                                                   analyses))
           .has_value ())
      return 1;
    auto start = Clock::now ();
    auto result = ::mirnext::transforms::detail::run_pass (AddressTransformPass (),
                                                           (function).value (), analyses);
    auto end = Clock::now ();
    if (!(result).has_value () || result.value ().transformations != count + 1
        || function.value ().ssa_form ().get () || !(function.value ().verify ()).has_value ())
      return 1;
    unsigned replacements = 0;
    for (auto &instruction : block.value ().instructions ()) {
      if (instruction.opcode () == Opcode::Addr) return 1;
      for (auto &operand : instruction.operands ())
        if (mirnext::ir::detail::operand_access (operand).get<Memory> ()) return 1;
      if (instruction.opcode () == Opcode::Mov
          && *mirnext::ir::detail::operand_access (instruction.operands ()[0])
                 .get<const Register *> ()
               == &(value).value ()
          && mirnext::ir::detail::operand_access (instruction.operands ()[1])
               .get<const Register *> ()
          && *mirnext::ir::detail::operand_access (instruction.operands ()[1])
                 .get<const Register *> ()
               == &(source).value ())
        ++replacements;
    }
    if (replacements != count) return 1;
    std::cout << "shape=address-copies count=" << count << " replacements=" << replacements
              << " lower_us=" << microseconds (end - start) << '\n';
  }
}
