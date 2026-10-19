/* Licensed under the MIT License. Private scanner token view shared with C. */
#ifndef MIRNEXT_RUNTIME_LEXER_H
#define MIRNEXT_RUNTIME_LEXER_H
#include "mir.h"
enum MIR_scan_token_code {
  TC_INT,TC_FLOAT,TC_DOUBLE,TC_LDOUBLE,TC_NAME,TC_STR,TC_NL,TC_EOFILE,
  TC_LEFT_PAR,TC_RIGHT_PAR,TC_COMMA,TC_SEMICOL,TC_COL
};
typedef struct MIR_scan_token {
  int code;
  union {int64_t i;float f;double d;long double ld;const char *name;MIR_str_t str;} u;
} token_t;
#endif
