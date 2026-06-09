/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#ifndef MIRNEXT_C_H
#define MIRNEXT_C_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MIRNext_context *MIRNext_context_t;
typedef struct MIRNext_module *MIRNext_module_t;
typedef struct MIRNext_function *MIRNext_function_t;
typedef struct MIRNext_instruction *MIRNext_instruction_t;

MIRNext_context_t MIRNext_context_create(void);
void MIRNext_context_destroy(MIRNext_context_t ctx);

MIRNext_module_t MIRNext_context_new_module(MIRNext_context_t ctx, const char *name);
const char *MIRNext_module_name(MIRNext_module_t module);

MIRNext_function_t MIRNext_module_new_function(MIRNext_module_t module, const char *name);
const char *MIRNext_function_name(MIRNext_function_t function);
size_t MIRNext_function_instruction_count(MIRNext_function_t function);
MIRNext_instruction_t MIRNext_function_append_label(MIRNext_function_t function);
MIRNext_instruction_t MIRNext_function_append_ret(MIRNext_function_t function);

#ifdef __cplusplus
}
#endif

#endif
