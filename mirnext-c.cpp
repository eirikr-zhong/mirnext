/* This file is a part of MIRNext project.
   Licensed under the MIT License. */

#include "mirnext-c.h"

#include "mir.hpp"

#include <new>

struct MIRNext_context {
  mirnext::Context impl;
};

struct MIRNext_module;
struct MIRNext_function;
struct MIRNext_instruction;

static mirnext::Module *as_module(MIRNext_module_t module) {
  return reinterpret_cast<mirnext::Module *>(module);
}

static mirnext::Function *as_function(MIRNext_function_t function) {
  return reinterpret_cast<mirnext::Function *>(function);
}

static MIRNext_module_t as_c_module(mirnext::Module *module) {
  return reinterpret_cast<MIRNext_module_t>(module);
}

static MIRNext_function_t as_c_function(mirnext::Function *function) {
  return reinterpret_cast<MIRNext_function_t>(function);
}

static MIRNext_instruction_t as_c_instruction(mirnext::Instruction *instruction) {
  return reinterpret_cast<MIRNext_instruction_t>(instruction);
}

extern "C" MIRNext_context_t MIRNext_context_create(void) {
  try {
    return new MIRNext_context;
  } catch (...) {
    return nullptr;
  }
}

extern "C" void MIRNext_context_destroy(MIRNext_context_t ctx) { delete ctx; }

extern "C" MIRNext_module_t MIRNext_context_new_module(MIRNext_context_t ctx, const char *name) {
  if (ctx == nullptr) return nullptr;
  try {
    return as_c_module(&ctx->impl.new_module(name == nullptr ? "" : name));
  } catch (...) {
    return nullptr;
  }
}

extern "C" const char *MIRNext_module_name(MIRNext_module_t module) {
  if (module == nullptr) return nullptr;
  return as_module(module)->name().data();
}

extern "C" MIRNext_function_t MIRNext_module_new_function(MIRNext_module_t module,
                                                          const char *name) {
  if (module == nullptr) return nullptr;
  try {
    return as_c_function(&as_module(module)->new_function(name == nullptr ? "" : name));
  } catch (...) {
    return nullptr;
  }
}

extern "C" const char *MIRNext_function_name(MIRNext_function_t function) {
  if (function == nullptr) return nullptr;
  return as_function(function)->name().data();
}

extern "C" size_t MIRNext_function_instruction_count(MIRNext_function_t function) {
  if (function == nullptr) return 0;
  return as_function(function)->instruction_count();
}

extern "C" MIRNext_instruction_t MIRNext_function_append_label(MIRNext_function_t function) {
  if (function == nullptr) return nullptr;
  try {
    return as_c_instruction(&as_function(function)->append_label());
  } catch (...) {
    return nullptr;
  }
}

extern "C" MIRNext_instruction_t MIRNext_function_append_ret(MIRNext_function_t function) {
  if (function == nullptr) return nullptr;
  try {
    return as_c_instruction(&as_function(function)->append_ret());
  } catch (...) {
    return nullptr;
  }
}
