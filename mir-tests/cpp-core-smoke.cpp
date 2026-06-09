/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mir.hpp"

#include <sstream>
#include <string>

int main() {
  mirnext::Context ctx;
  mirnext::Module &module = ctx.new_module("smoke");
  mirnext::Function &function = module.new_function("main");

  function.append_label();
  function.append_ret();

  if (module.name() != "smoke") return 1;
  if (function.name() != "main") return 2;
  if (function.instruction_count() != 2) return 3;

  std::ostringstream out;
  ctx.dump(out);
  const std::string text = out.str();
  if (text.find("module smoke") == std::string::npos) return 4;
  if (text.find("func main") == std::string::npos) return 5;
  if (text.find("ret") == std::string::npos) return 6;

  return 0;
}
