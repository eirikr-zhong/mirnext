/* Licensed under the MIT License. Private CFG metadata views shared by C and C++. */
#ifndef MIRNEXT_RUNTIME_CFG_H
#define MIRNEXT_RUNTIME_CFG_H
#include "mir-runtime-generator.h"
typedef struct bb *bb_t;
DEF_VARR(bb_t);
DEF_DLIST_LINK(bb_t);
typedef struct edge *edge_t;
typedef edge_t in_edge_t;
typedef edge_t out_edge_t;
DEF_DLIST_LINK(in_edge_t);
DEF_DLIST_LINK(out_edge_t);
struct edge {
  bb_t src, dst;
  size_t identity;
  DLIST_LINK(in_edge_t) in_link;
  DLIST_LINK(out_edge_t) out_link;
  unsigned char fall_through_p, back_edge_p, flag1, flag2;
};
DEF_DLIST(in_edge_t,in_link);
DEF_DLIST(out_edge_t,out_link);
typedef struct insn_data *insn_data_t;
struct insn_data {
  bb_t bb;
  union { bitmap_t call_hard_reg_args;size_t label_disp; } u;
};
DEF_DLIST(bb_insn_t,bb_insn_link);
struct bb {
  size_t index, pre, rpost, bfs;
  DLIST_LINK(bb_t) bb_link;
  DLIST(in_edge_t) in_edges;
  /* Preserve fallthrough, label and exit ordering. */
  DLIST(out_edge_t) out_edges;
  DLIST(bb_insn_t) bb_insns;
  unsigned char call_p, flag, reachable_p;
  bitmap_t in, out, gen, kill, dom_in, dom_out;
  loop_node_t loop_node;
  int max_int_pressure, max_fp_pressure;
};
DEF_DLIST(bb_t,bb_link);
DEF_DLIST_LINK(loop_node_t);
DEF_DLIST_TYPE(loop_node_t);
struct loop_node {
  uint32_t index;
  bb_t bb;
  loop_node_t entry, parent;
  union { loop_node_t preheader, preheader_loop; } u;
  DLIST(loop_node_t) children;
  DLIST_LINK(loop_node_t) children_link;
  int max_int_pressure, max_fp_pressure;
};
DEF_DLIST_CODE(loop_node_t,children_link);
typedef struct reg_info {
  long freq;
  size_t live_length;
} reg_info_t;
DEF_VARR(reg_info_t);
struct func_cfg {
  MIR_reg_t max_var;
  uint32_t curr_bb_insn_index;
  size_t next_edge_identity;
  VARR(reg_info_t) *reg_info;
  bitmap_t call_crossed_regs;
  DLIST(bb_t) bbs;
  loop_node_t root_loop_node;
};
#endif
