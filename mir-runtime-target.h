/* Licensed under the MIT License. Private state of the standalone C reference targets. */
#ifndef MIRNEXT_RUNTIME_TARGET_H
#define MIRNEXT_RUNTIME_TARGET_H
#include "mir.h"
DEF_VARR(int);
DEF_VARR(uint8_t);
DEF_VARR(uint64_t);
DEF_VARR(MIR_code_reloc_t);
typedef struct insn_pattern_info {
  int start, num;
} insn_pattern_info_t;
DEF_VARR(insn_pattern_info_t);
struct pattern {
  MIR_insn_code_t code;
  const char *pattern, *replacement;
#if defined(__x86_64__) || defined(_M_AMD64)
  int max_insn_size;
#endif
};
#if defined(__x86_64__) || defined(_M_AMD64)
typedef struct const_ref {
  int call_p;
  MIR_item_t func_item;
  size_t pc, next_insn_disp, const_num;
} const_ref_t;
DEF_VARR(const_ref_t);
typedef struct call_ref {
  MIR_item_t ref_func_item;
  uint8_t *call_addr;
} call_ref_t;
DEF_VARR(call_ref_t);
typedef struct label_ref {
  char abs_addr_p, short_p;
  size_t label_val_disp, next_insn_disp;
  union { MIR_label_t label; void *jump_addr; } u;
} label_ref_t;
DEF_VARR(label_ref_t);
/* Persistent data carried by a lazily generated basic-block version.  The
   object is private to the generator and is never serialized. */
struct target_bb_version {
  uint8_t *base;
  label_ref_t branch_ref;
};
struct target_ctx {
  unsigned char alloca_p, block_arg_func_p, leaf_p, keep_fp_p;
  int start_sp_from_bp_offset;
  MIR_insn_t temp_jump;
  int temp_jump_pat_ind;
  VARR(int) *pattern_indexes, *insn_pattern_indexes;
  VARR(insn_pattern_info_t) *insn_pattern_info;
  VARR(uint8_t) *result_code;
  VARR(uint64_t) *const_pool;
  VARR(const_ref_t) *const_refs;
  VARR(label_ref_t) *label_refs;
  VARR(uint64_t) *abs_address_locs;
  VARR(MIR_code_reloc_t) *relocs;
  VARR(call_ref_t) *call_refs;
};
#elif defined(__aarch64__) || defined(_M_ARM64)
typedef struct label_ref {
  int abs_addr_p, short_p;
  size_t label_val_disp;
  union { MIR_label_t label; void *jump_addr; } u;
} label_ref_t;
DEF_VARR(label_ref_t);
struct target_bb_version {
  uint8_t *base;
  label_ref_t branch_ref;
};
struct target_ctx {
  unsigned char alloca_p, block_arg_func_p, leaf_p, short_bb_branch_p;
  size_t small_aggregate_save_area;
  MIR_insn_t temp_jump;
  const char *temp_jump_replacement;
  VARR(int) *pattern_indexes;
  VARR(insn_pattern_info_t) *insn_pattern_info;
  VARR(uint8_t) *result_code;
  VARR(label_ref_t) *label_refs;
  VARR(uint64_t) *abs_address_locs;
  VARR(MIR_code_reloc_t) *relocs;
};
#else
#error "MIRNext supports only x86_64 and aarch64 targets"
#endif
#endif
