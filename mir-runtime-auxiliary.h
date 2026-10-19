/* Licensed under the MIT License. Private auxiliary state of the standalone C reference. */
#ifndef MIRNEXT_RUNTIME_AUXILIARY_H
#define MIRNEXT_RUNTIME_AUXILIARY_H
#include "mir.h"
#include <setjmp.h>
typedef struct string {
  size_t num;
  MIR_str_t str;
} string_t;
typedef const char *label_name_t;
typedef struct {
  int def_p;
  const char *name;
  MIR_label_t label;
} label_desc_t;
struct io_ctx {
  FILE *io_file;
  int (*io_writer)(MIR_context_t,uint8_t);
  int (*io_reader)(MIR_context_t);
  struct reduce_data *io_reduce_data;
  struct VARR_MIR_var_t *proto_vars;
  struct VARR_MIR_type_t *proto_types;
  struct VARR_MIR_op_t *read_insn_ops;
  struct VARR_string_t *output_strings;
  struct HTAB_string_t *output_string_tab;
  struct VARR_MIR_str_t *bin_strings;
  struct VARR_uint64_t *insn_label_string_nums;
  struct VARR_MIR_label_t *func_labels;
  size_t output_insns_len,output_labs_len;
  size_t output_regs_len,output_mem_len,output_int_len,output_float_len;
};
struct scan_ctx {
  jmp_buf error_jmp_buf;
  struct VARR_char *error_msg_buf;
  struct VARR_MIR_var_t *scan_vars;
  struct VARR_MIR_type_t *scan_types;
  struct VARR_MIR_op_t *scan_insn_ops;
  size_t curr_lno;
  struct HTAB_insn_name_t *insn_name_tab;
  const char *input_string;
  size_t input_string_char_num;
  struct VARR_label_name_t *label_names;
  struct HTAB_label_desc_t *label_desc_tab;
};
struct simplify_ctx {
  struct HTAB_val_t *val_tab;
  struct VARR_MIR_insn_t *temp_insns,*cold_insns,*labels;
  struct VARR_MIR_reg_t *inline_reg_map;
  struct VARR_MIR_insn_t *anchors;
  struct VARR_size_t *alloca_sizes;
  size_t new_label_num,inlined_calls,inline_insns_before,inline_insns_after;
};
typedef struct code_holder {
  uint8_t *start,*free,*bound;
} code_holder_t;
struct machine_code_ctx {
  struct VARR_code_holder_t *code_holders;
  size_t page_size;
};
#endif
