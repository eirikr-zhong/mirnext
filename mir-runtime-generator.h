/* Licensed under the MIT License. Private state of the standalone C reference generator. */
#ifndef MIRNEXT_RUNTIME_GENERATOR_H
#define MIRNEXT_RUNTIME_GENERATOR_H
#include "mir.h"
#include "mir-runtime-target.h"
#include "mir-bitmap.h"
#include "mir-dlist.h"
typedef struct func_cfg *func_cfg_t;
typedef struct loop_node *loop_node_t;
DEF_VARR(loop_node_t);
typedef struct dead_var *dead_var_t;
DEF_DLIST_LINK(dead_var_t);
struct dead_var {
  MIR_reg_t var;
  DLIST_LINK(dead_var_t) dead_var_link;
};
DEF_DLIST(dead_var_t,dead_var_link);
typedef struct bb_insn *bb_insn_t;
DEF_DLIST_LINK(bb_insn_t);
struct bb_insn {
  MIR_insn_t insn;
  unsigned char gvn_val_const_p, alloca_flag;
  uint32_t index, mem_index;
  int64_t gvn_val;
  DLIST_LINK(bb_insn_t) bb_insn_link;
  struct bb *bb;
  DLIST(dead_var_t) insn_dead_vars;
  bitmap_t call_hard_reg_args;
  size_t label_disp;
};
DEF_VARR(bb_insn_t);
typedef struct target_bb_version *target_bb_version_t;
DEF_VARR(target_bb_version_t);
typedef void *void_ptr_t;
DEF_VARR(void_ptr_t);
typedef struct {
  unsigned char alloca_flag, disp_def_p;
  MIR_type_t type;
  MIR_alias_t alias, nonalias;
  MIR_insn_t def_insn;
  int64_t disp; /* Known displacement only when disp_def_p is set. */
} mem_attr_t;
DEF_VARR(mem_attr_t);
typedef struct spot_attr {
  uint32_t spot, prop;
  MIR_op_t *mem_ref;
} spot_attr_t;
DEF_VARR(spot_attr_t);
DEF_VARR(MIR_op_t);
DEF_VARR(MIR_insn_t);

/* Lazy BBV records belong to the standalone C generator. */
typedef struct bb_version *bb_version_t;
typedef struct bb_stub *bb_stub_t;
DEF_DLIST_LINK (bb_version_t);
struct bb_version {
  bb_stub_t bb_stub;
  DLIST_LINK (bb_version_t) bb_version_link;
  int call_p;
  void *addr;
  void *machine_code;
  struct target_bb_version target_data;
  uint32_t n_attrs;
  spot_attr_t attrs[1];
};
DEF_DLIST (bb_version_t, bb_version_link);
struct bb_stub {
  DLIST (bb_version_t) bb_versions;
  MIR_item_t func_item;
  MIR_insn_t first_insn, last_insn;
};
struct gen_ctx {
  MIR_context_t ctx;
  unsigned optimize_level;
  MIR_item_t curr_func_item;
  FILE *debug_file;
  int debug_level;
  VARR(void_ptr_t) *to_free;
  int addr_insn_p;
  bitmap_t tied_regs, addr_regs;
  bitmap_t insn_to_consider, temp_bitmap, temp_bitmap2, temp_bitmap3;
  bitmap_t call_used_hard_regs[MIR_T_BOUND];
  bitmap_t func_used_hard_regs;
  func_cfg_t curr_cfg;
  uint32_t curr_bb_index, curr_loop_node_index;
  DLIST(dead_var_t) free_dead_vars;
  unsigned long long overall_bbs_num, overall_gen_bbs_num;
  struct target_ctx *target_ctx;
  struct data_flow_ctx *data_flow_ctx;
  struct ssa_ctx *ssa_ctx;
  struct gvn_ctx *gvn_ctx;
  struct lr_ctx *lr_ctx;
  struct coalesce_ctx *coalesce_ctx;
  struct ra_ctx *ra_ctx;
  struct combine_ctx *combine_ctx;
  VARR(MIR_op_t) *temp_ops;
  VARR(MIR_insn_t) *temp_insns, *temp_insns2;
  VARR(bb_insn_t) *temp_bb_insns, *temp_bb_insns2;
  VARR(loop_node_t) *loop_nodes, *queue_nodes, *loop_entries;
  unsigned char full_escape_p;
  VARR(mem_attr_t) *mem_attrs;
  int max_int_hard_regs, max_fp_hard_regs;
  size_t func_stack_slots_num;
  VARR(target_bb_version_t) *target_succ_bb_versions;
  VARR(void_ptr_t) *succ_bb_addrs;
  void *bb_wrapper;
  VARR(spot_attr_t) *spot2attr, *spot_attrs;
};
#endif
