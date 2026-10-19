/* Licensed under the MIT License. Private state of the standalone C reference interpreter. */
#ifndef MIRNEXT_RUNTIME_INTERPRETER_H
#define MIRNEXT_RUNTIME_INTERPRETER_H
#include "mir.h"
#define IC_EL(i) IC_##i
#define REP_SEP ,
typedef enum {
  IC_LDI8 = MIR_INSN_BOUND,
  REP6(IC_EL,LDU8,LDI16,LDU16,LDI32,LDU32,LDI64),
  REP3(IC_EL,LDF,LDD,LDLD),
  REP7(IC_EL,STI8,STU8,STI16,STU16,STI32,STU32,STI64),
  REP8(IC_EL,STF,STD,STLD,MOVI,MOVP,MOVF,MOVD,MOVLD),
  REP6(IC_EL,IMM_CALL,IMM_JCALL,MOVFG,FMOVFG,DMOVFG,LDMOVFG),
  REP5(IC_EL,MOVTG,FMOVTG,DMOVTG,LDMOVTG,INSN_BOUND),
} MIR_full_insn_code_t;
#undef REP_SEP
#undef IC_EL
#if defined(__aarch64__) || defined(_M_ARM64)
enum { MIR_INTERP_HARD_REG_COUNT = 64 };
#else
enum { MIR_INTERP_HARD_REG_COUNT = 34 };
#endif
struct interp_ctx {
  void *dispatch_label_tab[IC_INSN_BOUND];
  MIR_val_t global_regs[MIR_INTERP_HARD_REG_COUNT];
  struct VARR_MIR_val_t *code_varr;
  struct VARR_MIR_insn_t *branches;
  struct VARR_MIR_val_t *arg_vals_varr;
  MIR_val_t *arg_vals;
  int trace_insn_ident;
  void *(*bstart_builtin)(void);
  void (*bend_builtin)(void *);
  void *jret_addr;
  struct VARR_MIR_val_t *call_res_args_varr;
  MIR_val_t *call_res_args;
  struct VARR__MIR_arg_desc_t *call_arg_descs_varr;
  _MIR_arg_desc_t *call_arg_descs;
  struct HTAB_ff_interface_t *ff_interface_tab;
  int64_t addr_offset8,addr_offset16,addr_offset32;
};
#endif
