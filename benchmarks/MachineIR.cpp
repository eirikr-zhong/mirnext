/* Licensed under the MIT License. Preparation includes validation and CFG construction. */
#include "mirnext/CodeGen/Stages.hpp"
#include "mirnext/IR/Access.hpp"
#include "mirnext/IR/Checked.hpp"
#include "mirnext/CodeGen/MachineIR.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!(target).has_value ()) return 1;
  for (unsigned count : {256, 1024, 4096}) {
    Program c;
    auto m = mirnext::ir::detail::access (c).create_module ("machine-benchmark");
    if (!(m).has_value ()) return 1;
    auto f = mirnext::ir::detail::access (((m)).value ())
               .create_function ("entry", {{Type::i64 ()}, {}});
    if (!(f).has_value ()) return 1;
    auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
    auto sum = mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 (), "sum");
    if (!(b).has_value () || !(sum).has_value ()) return 1;
    InstructionBuilder builder ((b).value ());
    if (!(builder.assign ((sum).value (), 0)).has_value ()) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto data = mirnext::ir::detail::access (((m)).value ())
                    .create_data ("data" + std::to_string (n),
                                  InitializedData{Type::i64 (), {Operand (n)}});
      if (!(data).has_value ()) return 1;
      auto address
        = builder.address_of (data.value ().symbol (), 0, "address" + std::to_string (n));
      if (!(address).has_value ()) return 1;
      auto value = builder.load (mirnext::ir::detail::NodeAccess::make_Memory (Type::i64 (),
                                                                               &(address).value ()),
                                 "value" + std::to_string (n));
      if (!(value).has_value ()) return 1;
      if (!(builder.emit (Opcode::Add, {Operand ((sum).value ()), Operand ((sum).value ()),
                                        Operand ((value).value ())}))
             .has_value ())
        return 1;
    }
    if (!(builder.ret (Operand ((sum).value ()))).has_value ()) return 1;
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    std::size_t instructions = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      auto machine = prepare_machine_ir ((f).value (), (target).value ());
      auto end = std::chrono::steady_clock::now ();
      if (!(machine).has_value ()) {
        std::cerr << machine.error ().message << '\n';
        return 1;
      }
      std::uint64_t hash = 14695981039346656037ULL;
      for (unsigned char ch : machine.value ().dump ()) hash = (hash ^ ch) * 1099511628211ULL;
      if (rep && hash != fingerprint) return 2;
      fingerprint = hash;
      instructions = machine.value ().instruction_count ();
      times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
    }
    std::ranges::sort (times);
    std::cout << "symbols=" << count << " instructions=" << instructions
              << " median_ms=" << times[times.size () / 2] << " fingerprint=" << fingerprint
              << '\n';
  }
  // A module-wide hardware declaration is observable in every function. Ordinary
  // registers in unrelated functions should not make each preparation expensive.
  for (bool binding : {false, true})
    for (unsigned count : {64, 256, 1024}) {
      Program c;
      auto m = mirnext::ir::detail::access (c).create_module ("hardware-benchmark");
      if (!(m).has_value ()) return 1;
      for (unsigned n = 0; n < count; ++n) {
        auto f = mirnext::ir::detail::access (((m)).value ())
                   .create_function ("f" + std::to_string (n), {});
        if (!(f).has_value ()) return 1;
        auto block = mirnext::ir::detail::access (((f)).value ()).create_block ();
        if (!(block).has_value () || !(InstructionBuilder ((block).value ()).ret ()).has_value ())
          return 1;
        for (unsigned r = 0; r < 256; ++r)
          if (!(mirnext::ir::detail::access (((f)).value ()).create_register (Type::i64 ()))
                 .has_value ())
            return 1;
        if (binding && n == count / 2
            && !(mirnext::ir::detail::access (((f)).value ())
                   .create_register (Type::i64 (), "bound",
                                     target.value ().architecture ()
                                         == target::Architecture::AArch64
                                       ? "r19"
                                       : "r12"))
                  .has_value ())
          return 1;
      }
      auto f = mirnext::ir::detail::access (((m)).value ())
                 .create_function ("entry", {{Type::i64 ()}, {}});
      if (!(f).has_value ()) return 1;
      auto b = mirnext::ir::detail::access (((f)).value ()).create_block ();
      if (!(b).has_value () || !(InstructionBuilder ((b).value ()).ret (Operand (42))).has_value ())
        return 1;
      std::vector<double> times;
      std::uint64_t fingerprint = 0;
      for (unsigned rep = 0; rep < 11; ++rep) {
        auto start = std::chrono::steady_clock::now ();
        auto machine = prepare_machine_ir ((f).value (), (target).value ());
        auto end = std::chrono::steady_clock::now ();
        if (!(machine).has_value ()
            || machine.value ().global_registers ().size () != unsigned (binding))
          return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char ch : machine.value ().dump ()) hash = (hash ^ ch) * 1099511628211ULL;
        if (rep && hash != fingerprint) return 2;
        fingerprint = hash;
        times.push_back (std::chrono::duration<double, std::micro> (end - start).count ());
      }
      std::ranges::sort (times);
      std::cout << "hardware=" << binding << " functions=" << count
                << " ordinary_registers=" << count * 256
                << " median_us=" << times[times.size () / 2] << " fingerprint=" << fingerprint
                << '\n';
    }
}
