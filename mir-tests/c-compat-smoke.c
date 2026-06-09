/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mirnext-c.h"

#include <stddef.h>
#include <string.h>

int main(void) {
  MIRNext_context_t ctx = MIRNext_context_create();
  if (ctx == NULL) return 1;

  MIRNext_module_t module = MIRNext_context_new_module(ctx, "ccompat");
  if (module == NULL) return 2;
  if (strcmp(MIRNext_module_name(module), "ccompat") != 0) return 3;

  MIRNext_function_t function = MIRNext_module_new_function(module, "main");
  if (function == NULL) return 4;
  if (strcmp(MIRNext_function_name(function), "main") != 0) return 5;

  if (MIRNext_function_append_label(function) == NULL) return 6;
  if (MIRNext_function_append_ret(function) == NULL) return 7;
  if (MIRNext_function_instruction_count(function) != 2) return 8;

  MIRNext_context_destroy(ctx);
  return 0;
}
