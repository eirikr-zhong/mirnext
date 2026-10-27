/* Licensed under the MIT License. Preparation includes validation and CFG construction. */
#include "mirnext/CodeGen/MachineIR.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!target) return 1;
  for (unsigned count : {256, 1024, 4096}) {
    Context c;
    auto m = c.create_module ("machine-benchmark");
    if (!m) return 1;
    auto f = (*m)->create_function ("entry", {{Type::i64 ()}, {}});
    if (!f) return 1;
    auto b = (*f)->create_block ();
    auto sum = (*f)->create_register (Type::i64 (), "sum");
    if (!b || !sum) return 1;
    IRBuilder builder (**b);
    if (!builder.assign (**sum, 0)) return 1;
    for (unsigned n = 0; n < count; ++n) {
      auto data = (*m)->create_data ("data" + std::to_string (n),
                                     InitializedData{Type::i64 (), {Operand (n)}});
      if (!data) return 1;
      auto address = builder.address_of ((*data)->symbol (), 0, "address" + std::to_string (n));
      if (!address) return 1;
      auto value = builder.load (Memory{Type::i64 (), *address}, "value" + std::to_string (n));
      if (!value) return 1;
      if (!builder.emit (Opcode::Add, {Operand (**sum), Operand (**sum), Operand (**value)}))
        return 1;
    }
    if (!builder.ret (Operand (**sum))) return 1;
    std::vector<double> times;
    std::uint64_t fingerprint = 0;
    std::size_t instructions = 0;
    for (unsigned rep = 0; rep < 7; ++rep) {
      auto start = std::chrono::steady_clock::now ();
      auto machine = prepare_machine_ir (**f, *target);
      auto end = std::chrono::steady_clock::now ();
      if (!machine) {
        std::cerr << machine.error ().message << '\n';
        return 1;
      }
      std::uint64_t hash = 14695981039346656037ULL;
      for (unsigned char ch : (*machine)->dump ()) hash = (hash ^ ch) * 1099511628211ULL;
      if (rep && hash != fingerprint) return 2;
      fingerprint = hash;
      instructions = (*machine)->instruction_count ();
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
      Context c;
      auto m = c.create_module ("hardware-benchmark");
      if (!m) return 1;
      for (unsigned n = 0; n < count; ++n) {
        auto f = (*m)->create_function ("f" + std::to_string (n), {});
        if (!f) return 1;
        auto block = (*f)->create_block ();
        if (!block || !IRBuilder (**block).ret ()) return 1;
        for (unsigned r = 0; r < 256; ++r)
          if (!(*f)->create_register (Type::i64 ())) return 1;
        if (binding && n == count / 2
            && !(*f)->create_register (Type::i64 (), "bound",
                                       target->architecture () == target::Architecture::AArch64
                                         ? "r19"
                                         : "r12"))
          return 1;
      }
      auto f = (*m)->create_function ("entry", {{Type::i64 ()}, {}});
      if (!f) return 1;
      auto b = (*f)->create_block ();
      if (!b || !IRBuilder (**b).ret (Operand (42))) return 1;
      std::vector<double> times;
      std::uint64_t fingerprint = 0;
      for (unsigned rep = 0; rep < 11; ++rep) {
        auto start = std::chrono::steady_clock::now ();
        auto machine = prepare_machine_ir (**f, *target);
        auto end = std::chrono::steady_clock::now ();
        if (!machine || (*machine)->global_registers ().size () != unsigned (binding)) return 1;
        std::uint64_t hash = 14695981039346656037ULL;
        for (unsigned char ch : (*machine)->dump ()) hash = (hash ^ ch) * 1099511628211ULL;
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
