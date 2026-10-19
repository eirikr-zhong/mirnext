/* Licensed under the MIT License. Private data-flow and backend pass storage views. */
#ifndef MIRNEXT_RUNTIME_PASSES_H
#define MIRNEXT_RUNTIME_PASSES_H
#include "mir-runtime-generator.h"
#include "mir-runtime-cfg.h"
#include "mir-runtime-target.h"
DEF_VARR(MIR_reg_t);
DEF_VARR(size_t);
typedef struct ssa_edge *ssa_edge_t;
struct ssa_edge {
  bb_insn_t use, def;
  char flag;
  uint16_t def_op_num;
  uint32_t use_op_num;
  ssa_edge_t prev_use, next_use;
};
typedef struct def_tab_el {
  bb_t bb;
  MIR_reg_t reg;
  bb_insn_t def;
} def_tab_el_t;
DEF_VARR(ssa_edge_t);
DEF_VARR(char);
struct ssa_ctx {
  VARR(bb_insn_t) *arg_bb_insns, *undef_insns, *phis, *deleted_phis;
  struct HTAB_def_tab_el_t *def_tab;
  VARR(ssa_edge_t) *ssa_edges_to_process;
  VARR(size_t) *curr_reg_indexes;
  VARR(char) *reg_name;
};
typedef struct expr {
  MIR_insn_t insn;
  uint32_t num;
  MIR_reg_t temp_reg;
} *expr_t;
DEF_VARR(expr_t);
typedef struct mem_expr {
  MIR_insn_t insn;
  uint32_t mem_num;
  MIR_reg_t temp_reg;
  struct mem_expr *next;
} *mem_expr_t;
DEF_VARR(mem_expr_t);
typedef struct insn_nop_pair {
  bb_insn_t bb_insn;
  size_t nop;
} insn_nop_pair_t;
DEF_VARR(insn_nop_pair_t);
struct gvn_ctx {
  MIR_insn_t temp_mem_insn;
  VARR(expr_t) *exprs;
  VARR(mem_expr_t) *mem_exprs;
  struct HTAB_expr_t *expr_tab;
  struct HTAB_mem_expr_t *mem_expr_tab;
  VARR(insn_nop_pair_t) *insn_nop_pairs;
};
struct data_flow_ctx {
  VARR(bb_t) *worklist, *pending;
  bitmap_t bb_to_consider;
};
typedef struct lr_bb *lr_bb_t;
struct lr_bb {
  bb_t bb;
  lr_bb_t next;
};
typedef struct live_range *live_range_t;
struct live_range {
  lr_bb_t lr_bb; /* Whole blocks covered by this interval. */
  int start, finish, ref_cost;
  live_range_t next; /* Intervals ordered toward smaller start/finish points. */
};
DEF_VARR(live_range_t);
struct lr_ctx {
  int ssa_live_info_p, scan_vars_num;
  VARR(int) *var_to_scan_var_map;
  VARR(MIR_reg_t) *scan_var_to_var_map;
  live_range_t free_lr_list;
  lr_bb_t free_lr_bb_list;
  int curr_point;
  bitmap_t live_vars, referenced_vars;
  bitmap_t points_with_born_vars, points_with_dead_vars, points_with_born_or_dead_vars;
  VARR(live_range_t) *var_live_ranges;
  VARR(int) *point_map;
};
typedef struct mv {
  bb_insn_t bb_insn;
  size_t freq;
} mv_t;
DEF_VARR(mv_t);
struct coalesce_ctx {
  VARR(mv_t) *moves;
  VARR(MIR_reg_t) *first_coalesced_reg, *next_coalesced_reg;
  bitmap_t conflict_matrix;
};
typedef struct var_ref {
  MIR_insn_t insn;
  size_t insn_num, nop;
  char def_p, del_p;
} var_ref_t;
DEF_VARR(var_ref_t);
struct combine_ctx {
  VARR(size_t) *var_ref_ages;
  VARR(var_ref_t) *var_refs;
  var_ref_t *var_refs_addr;
  size_t *var_ref_ages_addr;
  size_t curr_bb_var_ref_age, last_mem_ref_insn_num;
  VARR(MIR_reg_t) *insn_vars;
  VARR(size_t) *changed_op_numbers;
  VARR(MIR_op_t) *last_right_ops;
  bitmap_t vars_bitmap;
};
#if defined(__aarch64__) || defined(_M_ARM64)
#define MIR_GENERATOR_HARD_REG_COUNT 64
#else
#define MIR_GENERATOR_HARD_REG_COUNT 34
#endif
typedef struct allocno_info {
  MIR_reg_t reg;
  int tied_reg_p;
  struct reg_info *reg_infos;
} allocno_info_t;
DEF_VARR(allocno_info_t);
typedef struct spill_cache_el {
  uint32_t age;
  MIR_reg_t slot;
} spill_cache_el_t;
DEF_VARR(spill_cache_el_t);
DEF_VARR(bitmap_t);
typedef struct lr_gap {
  int16_t hreg, type;
  MIR_reg_t reg;
  live_range_t lr;
} lr_gap_t;
DEF_VARR(lr_gap_t);
typedef struct spill_el {
  MIR_reg_t reg;
  signed char spill_p, edge_p, bb_end_p;
  union { struct edge *e; bb_t bb; } u;
} spill_el_t;
DEF_VARR(spill_el_t);
typedef struct insn_reload {
  MIR_type_t type;
  MIR_reg_t var, hreg;
} insn_reload_t;
#define MAX_INSN_RELOADS (2 * 4)
struct ra_ctx {
  MIR_reg_t start_mem_loc;
  VARR(MIR_reg_t) *reg_renumber;
  VARR(allocno_info_t) *sorted_regs;
  VARR(bitmap_t) *used_locs, *busy_used_locs, *var_bbs;
  VARR(lr_gap_t) *spill_gaps, *curr_gaps;
  bitmap_t lr_gap_bitmaps[MIR_GENERATOR_HARD_REG_COUNT];
  struct HTAB_lr_gap_t *lr_gap_tab;
  VARR(spill_el_t) *spill_els;
  VARR(spill_cache_el_t) *spill_cache;
  uint32_t spill_cache_age;
  bitmap_t conflict_locs1;
  struct reg_info *curr_reg_infos;
  int in_reloads_num, out_reloads_num;
  insn_reload_t in_reloads[MAX_INSN_RELOADS], out_reloads[MAX_INSN_RELOADS];
};
#endif
