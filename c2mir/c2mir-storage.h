/* Licensed under the MIT License. Private C frontend storage transport. */
#ifndef MIRNEXT_C2MIR_STORAGE_H
#define MIRNEXT_C2MIR_STORAGE_H
#include "mir.h"
#include "c2mir-literal.h"
#ifdef __cplusplus
extern "C" {
#endif
struct MIR_cpp_c_string {
  const char *bytes;
  size_t length, key, flags;
};
struct MIR_cpp_c_frontend {
  void *(*open)(void *);
  void (*close)(void *,void *);
  void *(*allocate)(void *,void *,size_t);
  size_t (*mark)(void *,void *);
  int (*release)(void *,void *,size_t);
  int (*intern)(void *,void *,const char *,size_t,size_t,size_t,int,struct MIR_cpp_c_string *);
  int (*find)(void *,void *,const char *,size_t,struct MIR_cpp_c_string *);
  const char *(*keyword)(void *,void *,size_t);
  void *data;
  int (*number)(void *,void *,const char *,size_t,const struct MIR_cpp_c_literal_layout *,int,struct MIR_cpp_c_number *);
  int (*string)(void *,void *,const char *,size_t,int,const struct MIR_cpp_c_literal_layout *,int,int,struct MIR_cpp_c_decoded_string *);
  int (*issue)(void *,void *,size_t,struct MIR_cpp_c_literal_issue *);
};
#ifdef MIR_OPT_TEST_HOOKS
size_t _MIR_opt_test_c2mir_legacy_storage(MIR_context_t);
size_t _MIR_opt_test_c2mir_legacy_tables(MIR_context_t);
int _MIR_opt_test_c2mir_intern(MIR_context_t,const char *,size_t,size_t,size_t,int,struct MIR_cpp_c_string *);
int _MIR_opt_test_c2mir_find(MIR_context_t,const char *,size_t,struct MIR_cpp_c_string *);
const char *_MIR_opt_test_c2mir_keyword(MIR_context_t,size_t);
void *_MIR_opt_test_c2mir_allocate(MIR_context_t,size_t);
size_t _MIR_opt_test_c2mir_mark(MIR_context_t);
void _MIR_opt_test_c2mir_release(MIR_context_t,size_t);
struct c2mir_options;
void _MIR_opt_test_c2mir_literals_begin(MIR_context_t,struct c2mir_options *);
void _MIR_opt_test_c2mir_literals_end(MIR_context_t);
int _MIR_opt_test_c2mir_number(MIR_context_t,const char *,struct MIR_cpp_c_number *);
int _MIR_opt_test_c2mir_string(MIR_context_t,const char *,int,int,struct MIR_cpp_c_decoded_string *);
size_t _MIR_opt_test_c2mir_legacy_literals(MIR_context_t);
void _MIR_opt_test_c2mir_literal_counts(MIR_context_t,unsigned *,unsigned *);
#endif
#ifdef __cplusplus
}
#endif
#endif
