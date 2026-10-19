# Test-only identity of the native product, its fixtures, build rules and oracle.
# Normalize checkout line endings so Windows and Unix identify the same sources.
function(mirnext_source_fingerprint root output)
  set(tracking)
  if(NOT CMAKE_SCRIPT_MODE_FILE)
    set(tracking CONFIGURE_DEPENDS)
  endif()
  file(GLOB_RECURSE sources ${tracking} RELATIVE "${root}"
    "${root}/include/mirnext/*.hpp" "${root}/include/mirnext/*.def"
    "${root}/lib/*.cpp" "${root}/lib/*.hpp"
    "${root}/tools/*.cpp" "${root}/tools/*.hpp" "${root}/tools/*.md"
    "${root}/examples/*.cpp" "${root}/cmake/*.cmake" "${root}/cmake/CMakeLists.txt"
    "${root}/tests/*.cpp" "${root}/tests/*.hpp" "${root}/tests/*.c" "${root}/tests/*.h"
    "${root}/tests/*.cmake" "${root}/tests/*.py" "${root}/tests/*.mir" "${root}/tests/CMakeLists.txt"
    "${root}/mir-tests/*.mir" "${root}/.github/workflows/*.yml")
  file(GLOB oracle ${tracking} RELATIVE "${root}"
    "${root}/mir*.c" "${root}/mir*.h" "${root}/mir*.def" "${root}/real-time.h")
  list(APPEND sources ${oracle} CMakeLists.txt GNUmakefile)
  list(REMOVE_DUPLICATES sources)
  list(SORT sources)
  set(manifest "MIRNext native source fingerprint v1\n")
  foreach(path IN LISTS sources)
    file(READ "${root}/${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(SHA256 digest "${content}")
    string(APPEND manifest "${digest} ${path}\n")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/${path}")
    endif()
  endforeach()
  string(SHA256 fingerprint "${manifest}")
  set(${output} "${fingerprint}" PARENT_SCOPE)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
  if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT_FILE)
    message(FATAL_ERROR "Source fingerprint requires SOURCE_DIR and OUTPUT_FILE")
  endif()
  mirnext_source_fingerprint("${SOURCE_DIR}" fingerprint)
  file(WRITE "${OUTPUT_FILE}" "${fingerprint}\n")
endif()
