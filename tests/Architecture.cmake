# Dependency guard for the owning product layers and retired transitional sources.
file(READ "${SOURCE_DIR}/GNUmakefile" product_make)
if(product_make MATCHES "\\$\\(shell" OR product_make MATCHES "mir[.]h|libmir[.]|mir-gen[.]c|mir[.]c"
    OR product_make MATCHES "include[^\n]*Legacy[.]mk")
  message(FATAL_ERROR "GNU make product entry points evaluate the old C build")
endif()
file(GLOB retired_sources "${SOURCE_DIR}/mir*.cpp" "${SOURCE_DIR}/mir*.hpp"
  "${SOURCE_DIR}/mir*.inc" "${SOURCE_DIR}/mir-legacy*"
  "${SOURCE_DIR}/mir-machine-bridge.h"
  "${SOURCE_DIR}/mir-gen-stub.c"
  "${SOURCE_DIR}/mir-tests/*.c" "${SOURCE_DIR}/mir-tests/test-read.h"
  "${SOURCE_DIR}/mir-tests/scan-args.h"
  "${SOURCE_DIR}/mir-tests/api-loop.h" "${SOURCE_DIR}/mir-tests/api-memop.h"
  "${SOURCE_DIR}/mir-tests/api-sieve.h"
  "${SOURCE_DIR}/mir-bin-run.c" "${SOURCE_DIR}/mir-bin-driver.c"
  "${SOURCE_DIR}/adt-tests/*.c" "${SOURCE_DIR}/adt-tests/*.h"
  "${SOURCE_DIR}/mir-utils/*.c")
if(retired_sources)
  message(FATAL_ERROR "Retired core sources or test drivers reintroduced: ${retired_sources}")
endif()
file(GLOB_RECURSE c_product_files
  "${SOURCE_DIR}/include/mirnext/*.c" "${SOURCE_DIR}/include/mirnext/*.h"
  "${SOURCE_DIR}/lib/*.c" "${SOURCE_DIR}/lib/*.h"
  "${SOURCE_DIR}/tools/*.c" "${SOURCE_DIR}/tools/*.h")
if(c_product_files)
  message(FATAL_ERROR "C sources or compatibility headers introduced in the owning product: ${c_product_files}")
endif()
file(GLOB_RECURSE native_files
  "${SOURCE_DIR}/include/mirnext/*.hpp" "${SOURCE_DIR}/include/mirnext/*.def"
  "${SOURCE_DIR}/lib/*.cpp" "${SOURCE_DIR}/lib/*.hpp")
file(GLOB command_files "${SOURCE_DIR}/tools/*.cpp" "${SOURCE_DIR}/tools/*.hpp")
list(APPEND native_files ${command_files})
foreach(path IN LISTS native_files)
  file(READ "${path}" source)
  string(REGEX MATCHALL "#[ \t]*include[ \t]*\"[^\"]+\"" quoted_includes "${source}")
  foreach(include IN LISTS quoted_includes)
    if(NOT include MATCHES "\"mirnext/" AND NOT (path MATCHES "/tools/" AND include MATCHES "\"(Streams|Run)[.]hpp\""))
      message(FATAL_ERROR "Native layer includes a header outside its namespace: ${path}: ${include}")
    endif()
  endforeach()
  string(REGEX MATCHALL "MIR_[A-Za-z0-9_]+" legacy_identifiers "${source}")
  foreach(identifier IN LISTS legacy_identifiers)
    # Environment variable spellings are CLI configuration, not C IR APIs.
    if(NOT (path MATCHES "/tools/Run[.]cpp$" AND identifier MATCHES "^MIR_(TYPE|LIBS|LIB_DIRS)$"))
      message(FATAL_ERROR "Native architecture depends on a legacy identifier: ${path}: ${identifier}")
    endif()
  endforeach()
  if(source MATCHES "(VARR|DLIST)[ \t]*\\("
      OR source MATCHES "(^|[^A-Za-z0-9_])gen_ctx([^A-Za-z0-9_]|$)"
      OR source MATCHES "#[ \t]*include[ \t]*[\"<](mir[.]h|mir-gen[.]h|mir-machine-bridge[.]h|mir-legacy[^\">]*|mir-runtime-[^\">]*[.]h)[\">]")
    message(FATAL_ERROR "Native architecture depends on a legacy representation: ${path}")
  endif()
endforeach()
message(STATUS "Checked ${native_files}")
