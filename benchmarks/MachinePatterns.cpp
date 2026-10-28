/* Licensed under the MIT License. Target matching excludes IR construction. */
#include "mirnext/CodeGen/Access.hpp"
#include "mirnext/CodeGen/Checked.hpp"
#include "mirnext/CodeGen/Lowering.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
int main () {
  using namespace mirnext;
  using namespace mirnext::ir;
  using namespace mirnext::codegen;
  auto target = target::TargetMachine::native ();
  if (!(target).has_value () || target.value ().architecture () != target::Architecture::AArch64)
    return 1;
  MachineFunction f ("benchmark", "patterns", {{}, {}}, (target).value ());
  auto &b = mirnext::codegen::detail::access (f).create_block ();
  MachineRewriter rw (f);
  auto a = mirnext::codegen::detail::access (f).create_register (Type::i64 ()),
       c = mirnext::codegen::detail::access (f).create_register (Type::i64 ()),
       d = mirnext::codegen::detail::access (f).create_register (Type::i64 ());
  if (!(a).has_value () || !(c).has_value () || !(d).has_value ()) return 1;
  std::array codes{Opcode::Add, Opcode::Sub, Opcode::Mul, Opcode::Div, Opcode::And, Opcode::Or,
                   Opcode::Xor, Opcode::Lsh, Opcode::Rsh, Opcode::Eq,  Opcode::Le,  Opcode::UGt};
  for (unsigned n = 0; n < 65536; ++n) {
    auto input
      = n % 4 == 0
          ? mirnext::codegen::detail::NodeAccess::make_MachineOperand (
              mirnext::codegen::detail::NodeAccess::
                make_MachineMemory (Type::i64 (),
                                    mirnext::codegen::detail::NodeAccess::make_MachineRegister (
                                      &(c).value ()),
                                    {}, 0, 1, {}, {}))
        : n % 4 == 1
          ? mirnext::codegen::detail::NodeAccess::make_MachineOperand (std::uint64_t (n % 8192))
          : mirnext::codegen::detail::NodeAccess::make_MachineOperand ((c).value ());
    if (!(rw.append (b, codes[n % codes.size ()],
                     {mirnext::codegen::detail::NodeAccess::make_MachineOperand ((a).value ()),
                      mirnext::codegen::detail::NodeAccess::make_MachineOperand ((d).value ()),
                      input}))
           .has_value ())
      return 2;
  }
  std::vector<double> times;
  std::uint64_t fingerprint = 0;
  for (unsigned rep = 0; rep < 9; ++rep) {
    auto start = std::chrono::steady_clock::now ();
    std::uint64_t hash = 14695981039346656037ULL;
    for (auto &i : b.instructions ()) {
      auto match = has_instruction_pattern (i);
      if (!(match).has_value ()) return 3;
      hash = (hash ^ (match).value ()) * 1099511628211ULL;
    }
    auto end = std::chrono::steady_clock::now ();
    if (rep && hash != fingerprint) return 4;
    fingerprint = hash;
    times.push_back (std::chrono::duration<double, std::milli> (end - start).count ());
  }
  std::ranges::sort (times);
  std::cout << "instructions=" << f.instruction_count ()
            << " median_ms=" << times[times.size () / 2] << " fingerprint=" << fingerprint << '\n';
}
