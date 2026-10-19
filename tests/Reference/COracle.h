/* Licensed under the MIT License. Private protocol of the optional C test oracle. */
#ifndef MIRNEXT_TEST_REFERENCE_C_ORACLE_H
#define MIRNEXT_TEST_REFERENCE_C_ORACLE_H
#ifndef MIR_OPT_TEST_HOOKS
#error "C oracle observations require MIR_OPT_TEST_HOOKS"
#endif
#include "mir.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Test-only observation; no binary protocol or production ABI additions. */
typedef void (*MIR_opt_test_observer_t) (MIR_context_t, const char *, MIR_item_t, FILE *, void *);
extern int _MIR_opt_test_target_register_info (unsigned, int *, int *, int *, int *, int *, int *);
extern void _MIR_opt_test_live_target_clobbers (int, uint32_t *);
extern void _MIR_opt_test_prepare_module_lrefs (MIR_context_t, MIR_module_t);
/* Isolated backend analysis oracle: frequency, four live sets, pressure,
   and instruction definitions/uses/deaths. */
extern void _MIR_opt_test_visit_machine_liveness (MIR_context_t, MIR_item_t,
  void (*)(unsigned, size_t, MIR_insn_t, MIR_reg_t, long, void *), void *);
extern void MIR_opt_set_test_observer (MIR_context_t, MIR_opt_test_observer_t, void *);
extern void _MIR_opt_test_emit (MIR_context_t, const char *, MIR_item_t, FILE *);
extern int _MIR_opt_test_enabled (MIR_context_t);
extern void _MIR_opt_test_simplify_module (MIR_context_t, MIR_module_t);
extern void _MIR_opt_test_inline_context (MIR_context_t);
typedef void (*MIR_opt_test_cfg_visitor_t) (size_t block, MIR_insn_t insn, size_t successor, void *);
extern void _MIR_opt_test_visit_cfg (MIR_context_t, MIR_item_t, MIR_opt_test_cfg_visitor_t, void *);
typedef void (*MIR_opt_test_ssa_visitor_t) (MIR_insn_t use, size_t use_operand,
  MIR_insn_t def, size_t def_operand, unsigned part, unsigned fake_kind, MIR_reg_t fake_register, void *);
extern void _MIR_opt_test_visit_ssa (MIR_context_t, MIR_item_t, MIR_opt_test_ssa_visitor_t, void *);
extern MIR_reg_t _MIR_opt_test_var_register (MIR_reg_t);
extern unsigned _MIR_opt_test_initial_register_kind (MIR_context_t, MIR_reg_t);
extern unsigned _MIR_opt_test_optimize_level (MIR_context_t);
extern int _MIR_opt_test_gvn_function (MIR_context_t, MIR_item_t, unsigned,
  void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_memory_function (MIR_context_t, MIR_item_t, unsigned,
  void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_dce_function (MIR_context_t, MIR_item_t, unsigned,
                                     void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_motion_function (MIR_context_t, MIR_item_t, unsigned,
                                        void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_combine_function (MIR_context_t, MIR_item_t, unsigned, int full_pipeline,
                                         void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_jump_function (MIR_context_t, MIR_item_t, unsigned, int full_pipeline,
                                      void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_pipeline_function (MIR_context_t, MIR_item_t, unsigned,
                                          void (*)(const char *, MIR_context_t, MIR_item_t, void *), void *);
typedef void (*MIR_opt_test_analysis_visitor_t) (unsigned kind, size_t block, size_t value, void *);
extern void _MIR_opt_test_visit_jump_state (MIR_context_t, MIR_item_t, MIR_opt_test_analysis_visitor_t, void *);
extern void _MIR_opt_test_visit_analysis (MIR_context_t, MIR_opt_test_analysis_visitor_t, void *);
extern int _MIR_opt_test_analyze_function (MIR_context_t, MIR_item_t, unsigned level,
                                         void (*observe)(MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_machinize_function (MIR_context_t, MIR_item_t, unsigned,
                                           void (*)(MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_legalize_function (MIR_context_t, MIR_item_t, unsigned,
                                         void (*)(MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_instruction_pattern (MIR_context_t, MIR_insn_t);
extern int _MIR_opt_test_allocation_function (MIR_context_t, MIR_item_t, unsigned, int,
                                            void (*)(MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_assign_function (MIR_context_t, MIR_item_t, unsigned,
                                        void (*)(MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_rewrite_function (MIR_context_t, MIR_item_t, unsigned,
                                         void (*)(MIR_context_t, MIR_item_t, void *), void *);
extern void _MIR_opt_test_visit_lowering_state (MIR_context_t,
                                               void (*)(unsigned, int64_t, void *), void *);
extern void _MIR_opt_test_visit_machine_calls (MIR_context_t, MIR_item_t,
                                              void (*)(MIR_insn_t, uint32_t, void *), void *);
extern int _MIR_opt_test_ssa_function (MIR_context_t, MIR_item_t, unsigned level, int rename,
  void (*observe)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_address_function (MIR_context_t, MIR_item_t, unsigned level,
  void (*observe)(const char *, MIR_context_t, MIR_item_t, void *), void *);
extern int _MIR_opt_test_clone_function (MIR_context_t, MIR_item_t, unsigned level,
  void (*observe)(const char *, MIR_context_t, MIR_item_t, void *), void *);
struct MIR_reference_machine_state {
  unsigned char uses_alloca, has_block_arguments, is_leaf, requires_fp;
  int initial_sp_offset;
  size_t aggregate_save_bytes;
};
struct MIR_reference_live_visitor {
  void (*begin)(void *, size_t, unsigned);
  void (*reg)(void *, uint32_t, int, long, int);
  void (*scan_reg)(void *, uint32_t);
  void (*global_reg)(void *, uint32_t);
  void (*block)(void *, size_t, size_t, unsigned, int, int);
  void (*edge)(void *, size_t, size_t);
  void (*instruction)(void *, MIR_insn_t, size_t, size_t);
  void (*operand)(void *, uint32_t, int);
  void (*call_arg)(void *, uint32_t);
  void (*block_bit)(void *, size_t, unsigned, uint32_t);
  void (*dead_var)(void *, uint32_t);
};
struct MIR_reference_ranges_visitor {
  void (*begin)(void *, size_t, int);
  void (*range)(void *, uint32_t, int, int, int);
  void (*range_block)(void *, size_t);
  void (*point)(void *, int);
  void (*call_crossed)(void *, uint32_t);
  void (*spill_bit)(void *, size_t, int, uint32_t);
};
struct MIR_reference_assignment_visitor {
  void (*begin)(void *, size_t, uint32_t);
  void (*reg)(void *, uint32_t, uint32_t, size_t);
  void (*order)(void *, uint32_t);
  void (*point_location)(void *, size_t, int, uint32_t);
  void (*used_hardware)(void *, uint32_t);
  void (*gap)(void *, uint32_t, uint32_t, size_t);
};
struct MIR_reference_encoded_constant { int call; MIR_item_t function; size_t pc, next_instruction, constant; };
struct MIR_reference_spill_graph_visitor {
  void (*begin)(void *, size_t, size_t);
  void (*edge)(void *, size_t, size_t, size_t, int, int, int, int);
  void (*block_edge)(void *, size_t, int, size_t);
  void (*cache)(void *, uint32_t, uint32_t);
};
struct MIR_reference_encoded_visitor {
  void (*bytes)(void *, const uint8_t *, size_t);
  void (*absolute)(void *, size_t);
  void (*pool)(void *, uint64_t);
  void (*constant)(void *, const struct MIR_reference_encoded_constant *);
  void (*label)(void *, MIR_insn_t, size_t);
};
void _MIR_reference_set_machine_state(MIR_context_t, const struct MIR_reference_machine_state *);
void _MIR_reference_get_machine_state(MIR_context_t, struct MIR_reference_machine_state *);
void _MIR_reference_visit_liveness(MIR_context_t, MIR_item_t,
                                const struct MIR_reference_live_visitor *, void *);
void _MIR_reference_visit_live_ranges(MIR_context_t, MIR_item_t, const struct MIR_reference_ranges_visitor *, void *);
void _MIR_reference_visit_assignment(MIR_context_t, MIR_item_t, const struct MIR_reference_assignment_visitor *, void *);
void _MIR_reference_visit_frame(MIR_context_t, void (*)(void *, size_t), void (*)(void *, uint32_t), void *);
void _MIR_reference_visit_encoding_labels(MIR_context_t, MIR_item_t, void (*)(void *, MIR_insn_t, int), void *);
void _MIR_reference_visit_spill_graph(MIR_context_t, const struct MIR_reference_spill_graph_visitor *, void *);
void _MIR_opt_test_visit_encoding(MIR_context_t, MIR_item_t, const struct MIR_reference_encoded_visitor *, void *);
int _MIR_opt_test_x86_encoding_max(size_t);
void _MIR_opt_test_set_frame_facts(MIR_context_t, size_t, const uint32_t *, size_t);
void _MIR_opt_test_visit_assignment(MIR_context_t, MIR_item_t, const struct MIR_reference_assignment_visitor *, void *);
void _MIR_opt_test_visit_spill_cache(MIR_context_t, void (*)(void *, uint32_t, uint32_t), void *);
void _MIR_opt_test_visit_live_ranges(MIR_context_t, MIR_item_t, const struct MIR_reference_ranges_visitor *, void *);
#ifdef __cplusplus
}
#endif
#endif
