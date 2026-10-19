/* Licensed under the MIT License. Private state of the standalone C reference runtime. */
#ifndef MIRNEXT_RUNTIME_CONTEXT_H
#define MIRNEXT_RUNTIME_CONTEXT_H
#include "mir.h"
struct gen_ctx;
struct c2mir_ctx;
struct string_ctx;
struct reg_ctx;
struct alias_ctx;
struct simplify_ctx;
struct machine_code_ctx;
struct io_ctx;
struct scan_ctx;
struct hard_reg_ctx;
struct interp_ctx;
struct MIR_context {
  struct gen_ctx *gen_ctx;
  struct c2mir_ctx *c2mir_ctx;
  MIR_error_func_t error_func;
#ifdef MIR_OPT_TEST_HOOKS
  MIR_opt_test_observer_t opt_test_observer;
  void *opt_test_data;
#endif
  MIR_alloc_t alloc;
  MIR_code_alloc_t code_alloc;
  int func_redef_permission_p;
  struct VARR_size_t *insn_nops;
  struct VARR_MIR_proto_t *unspec_protos;
  struct VARR_char *temp_string;
  struct VARR_uint8_t *temp_data, *used_label_p;
  struct HTAB_MIR_item_t *module_item_tab;
  struct MIR_module environment_module;
  MIR_module_t curr_module;
  MIR_func_t curr_func;
  size_t curr_label_num;
  DLIST(MIR_module_t) all_modules;
  struct VARR_MIR_module_t *modules_to_link;
  struct VARR_MIR_op_t *temp_ops;
  struct string_ctx *string_ctx;
  struct reg_ctx *reg_ctx;
  struct alias_ctx *alias_ctx;
  struct simplify_ctx *simplify_ctx;
  struct machine_code_ctx *machine_code_ctx;
  struct io_ctx *io_ctx;
  struct scan_ctx *scan_ctx;
  struct hard_reg_ctx *hard_reg_ctx;
  struct interp_ctx *interp_ctx;
  void *setjmp_addr;
  void *wrapper_end_addr;
};
#endif
