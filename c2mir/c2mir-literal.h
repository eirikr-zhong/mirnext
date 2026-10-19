/* Licensed under the MIT License. Private C frontend literal transport. */
#ifndef MIRNEXT_C2MIR_LITERAL_H
#define MIRNEXT_C2MIR_LITERAL_H
#include <stddef.h>
#include <stdint.h>
enum MIR_cpp_c_number_kind {
  MIR_CPP_C_INT,MIR_CPP_C_LONG,MIR_CPP_C_LLONG,MIR_CPP_C_UINT,MIR_CPP_C_ULONG,
  MIR_CPP_C_ULLONG,MIR_CPP_C_FLOAT,MIR_CPP_C_DOUBLE,MIR_CPP_C_LDOUBLE
};
enum MIR_cpp_c_literal_issue_code {
  MIR_CPP_C_BINARY_EXTENSION,MIR_CPP_C_INVALID_NUMBER,MIR_CPP_C_NUMBER_TAIL,
  MIR_CPP_C_FLOAT_RANGE,MIR_CPP_C_INT_RANGE,MIR_CPP_C_MULTIBYTE_CHAR,
  MIR_CPP_C_ESCAPE_EXTENSION,MIR_CPP_C_HEX_MISSING,MIR_CPP_C_HEX_RANGE,
  MIR_CPP_C_UNIVERSAL_INCOMPLETE,MIR_CPP_C_UNIVERSAL_RANGE,
  MIR_CPP_C_UNIVERSAL_RESERVED,MIR_CPP_C_INVALID_ESCAPE,MIR_CPP_C_EMPTY_CHAR
};
struct MIR_cpp_c_literal_layout {
  uint64_t int_max,uint_max,long_max,ulong_max,llong_max;
  uint32_t wchar_max;
  unsigned wchar_size;
  int little_endian,long_double_is_double;
};
union MIR_cpp_c_number_value {int64_t i;uint64_t u;float f;double d;long double ld;};
struct MIR_cpp_c_number {
  int kind;
  union MIR_cpp_c_number_value value;
  size_t stop,expected_end,issues;
};
struct MIR_cpp_c_decoded_string {
  int string_p;
  int64_t last_character;
  const char *bytes;
  size_t length,issues;
};
struct MIR_cpp_c_literal_issue {int code,error;uint64_t value;int character;};
#endif
