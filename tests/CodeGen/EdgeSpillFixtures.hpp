/* Licensed under the MIT License. Actual allocation gaps and critical edges. */
#pragma once
#include "mirnext/Target/TargetMachine.hpp"
#include <sstream>
namespace mirnext::test {
inline std::string edge_spill_fixture (const target::TargetMachine &target, bool critical) {
  std::ostringstream text;
  text << "m:module\nf:func i64";
  if (critical) text << ",i64:selector";
  text << "\nlocal i64:a,i64:c";
  for (auto &p : target.registers ())
    if (!p.reserved && p.bank == target::RegisterBank::Integer && p.reg.encoding ())
      text << "\nglobal i64:hold" << p.reg.encoding () << ':' << p.name;
  text << "\nmov a,42\n";
  for (unsigned n = 0; n < 20; ++n) text << "add a,a,1\n";
  if (critical) text << "beq Middle,selector,0\nOther:\njmp Join\n";
  text << "Middle:\nmov c,7\n";
  for (unsigned n = 0; n < 3; ++n) text << "add c,c,1\n";
  if (critical) text << "beq Join,selector,1\nNoA:\nmov a,c\njmp Join\n";
  text << "Join:\nret a\nendfunc\nendmodule\n";
  return text.str ();
}
}  // namespace mirnext::test
