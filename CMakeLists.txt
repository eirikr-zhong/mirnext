
cmake_minimum_required (VERSION 3.11)

#set(CMAKE_C_COMPILER "clang-cl")

project ("MIRNext" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT DEFINED CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

#set(CMAKE_VERBOSE_MAKEFILE ON)
include(CheckCCompilerFlag)
set(MIR_C_WORKAROUND_FLAGS "")
if(CMAKE_COMPILER_IS_GNUCC)
  check_c_compiler_flag(-fno-tree-sra NO_TREE_SRA)
  if (NO_TREE_SRA)
    list(APPEND MIR_C_WORKAROUND_FLAGS -fno-tree-sra)
  endif()
  check_c_compiler_flag(-fno-ipa-cp-clone NO_IPA_CP_CLONE)
  if (NO_IPA_CP_CLONE)
    list(APPEND MIR_C_WORKAROUND_FLAGS -fno-ipa-cp-clone)
  endif()
endif()

function(mir_target_common_options target)
  if(NOT WIN32)
    target_compile_options(${target} PRIVATE
      $<$<CONFIG:RELEASE>:-O3>
      $<$<CONFIG:RelWithDebInfo>:-O3>
      -fsigned-char
      -fPIC
    )
  endif()
endfunction()

function(mir_target_c_options target)
  if(NOT WIN32)
    target_compile_options(${target} PRIVATE -std=gnu11 -Wno-abi ${MIR_C_WORKAROUND_FLAGS})
  endif()
endfunction()

function(mir_strip_msvc_exception_flags variable)
  if(DEFINED ${variable})
    string(REGEX REPLACE "(^| )[/-]EH[a-zA-Z0-9+-]*" "" ${variable} "${${variable}}")
    string(STRIP "${${variable}}" ${variable})
    set(${variable} "${${variable}}" PARENT_SCOPE)
  endif()
endfunction()

function(mir_target_cxx_options target)
  if(MSVC)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    target_compile_options(${target} PRIVATE /EHs- /EHc- /GR-)
  else()
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
  endif()
endfunction()

if(MSVC)
  mir_strip_msvc_exception_flags(CMAKE_CXX_FLAGS)
  foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    mir_strip_msvc_exception_flags(CMAKE_CXX_FLAGS_${config})
  endforeach()
endif()

include(FindThreads)
if(Threads_FOUND)
  link_libraries(${CMAKE_THREAD_LIBS_INIT})
endif()

message("C compiler flags: ${CMAKE_C_FLAGS}")
message("C++ compiler flags: ${CMAKE_CXX_FLAGS}")

option(BUILD_TESTING "" ON)
include(CTest)

if(BUILD_TESTING)
  include(FetchContent)
  set(DOCTEST_WITH_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG v2.4.12
  )
  FetchContent_GetProperties(doctest)
  if(NOT doctest_POPULATED)
    FetchContent_Populate(doctest)
    add_subdirectory(${doctest_SOURCE_DIR} ${doctest_BINARY_DIR})
  endif()
endif()

add_library(mir_c_obj OBJECT mir.c mir-gen.c c2mir/c2mir.c mir.h mir-gen.h c2mir/c2mir.h)
target_include_directories(mir_c_obj PRIVATE ${PROJECT_SOURCE_DIR})
if(Threads_FOUND)
  target_compile_definitions(mir_c_obj PUBLIC "MIR_PARALLEL_GEN")
endif()
if(UNIX)
	target_link_libraries(mir_c_obj m)
endif()

# ------------------ LEGACY C LIBMIR -----------------------
add_library(mir-c-static STATIC)
target_link_libraries(mir-c-static PRIVATE mir_c_obj)
set_target_properties(mir-c-static PROPERTIES OUTPUT_NAME "mir-c-static")

# ------------------ LEGACY C LIBMIR SO --------------------
add_library(mir-c SHARED)
if(WIN32)
  set_target_properties(mir-c PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()
set_target_properties(mir-c PROPERTIES OUTPUT_NAME "mir-c")
target_link_libraries(mir-c PRIVATE mir_c_obj)

# ------------------ MIRNEXT C++ CORE -----------------------
add_library(mirnext_obj OBJECT
  mir-core.cpp
  mir-builder.cpp
  mir-dump.cpp
  mir-legacy.cpp
  mir-binary.cpp
  mir.hpp
  mir-legacy.hpp
)
target_include_directories(mirnext_obj PUBLIC ${PROJECT_SOURCE_DIR})
mir_target_cxx_options(mirnext_obj)

add_library(mirnext_static STATIC)
target_link_libraries(mirnext_static PRIVATE mirnext_obj mir_c_obj)
set_target_properties(mirnext_static PROPERTIES OUTPUT_NAME "mirnext-static")

add_library(mirnext SHARED)
if(WIN32)
  set_target_properties(mirnext PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()
set_target_properties(mirnext PROPERTIES OUTPUT_NAME "mirnext")
target_link_libraries(mirnext PRIVATE mirnext_obj mir_c_obj)

# ------------------ C2M --------------------------
add_executable (c2m "c2mir/c2mir-driver.c")
target_include_directories(c2m PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(c2m mir_c_obj ${CMAKE_DL_LIBS} )

# ------------------ MIR RUN ----------------------

#add_executable (mir-run "mir-run.c")
#target_include_directories (mir-run PRIVATE ${PROJECT_SOURCE_DIR})
#target_link_libraries(mir-run mir_c_obj ${CMAKE_DL_LIBS} )

# ------------------ MIR utils --------------------
add_executable (m2b "mir-utils/m2b.c")
target_include_directories(m2b PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(m2b mir_c_obj)

add_executable (b2m "mir-utils/b2m.c")
target_include_directories(b2m PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(b2m mir_c_obj)

add_executable (b2ctab "mir-utils/b2ctab.c")
target_include_directories(b2ctab PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(b2ctab mir_c_obj)

# ------------------ ADT tests --------------------------
add_executable (varr_test "adt-tests/mir-varr-test.c")
target_include_directories(varr_test PRIVATE ${PROJECT_SOURCE_DIR})

add_executable (dlist_test "adt-tests/mir-dlist-test.c")
target_include_directories(dlist_test PRIVATE ${PROJECT_SOURCE_DIR})

add_executable (bitmap_test "adt-tests/mir-bitmap-test.c")
target_include_directories(bitmap_test PRIVATE ${PROJECT_SOURCE_DIR})

add_executable (htab_test "adt-tests/mir-htab-test.c")
target_include_directories(htab_test PRIVATE ${PROJECT_SOURCE_DIR})

add_executable (reduce_test "adt-tests/mir-reduce-test.c")
target_include_directories(reduce_test PRIVATE ${PROJECT_SOURCE_DIR})

add_test(varr-test varr_test)
add_test(dlist-test dlist_test)
add_test(bitmap-test bitmap_test)
add_test(htab-test htab_test)
add_test(reduce-test reduce_test ${PROJECT_SOURCE_DIR}/c2mir/c2mir.c)

# ------------------ MIR utility tests ------------------------
add_executable (simplify_test "mir-tests/simplify.c")
target_link_libraries(simplify_test mir_c_obj)

add_executable (scan_test "mir-tests/scan-test.c")
target_link_libraries(scan_test mir_c_obj)

add_executable (io_test "mir-tests/io.c")
target_link_libraries(io_test mir_c_obj)

add_test(simplify-test simplify_test)
add_test(scan-test scan_test)
add_test(io-test io_test)

# ------------------ Common for MIR tests ----------------------
add_executable (run_test "mir-tests/run-test.c")
target_include_directories(run_test PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(run_test mir_c_obj)

# ------------------ MIR interp tests --------------------------
add_executable (interp_loop "mir-tests/loop-interp.c")
target_include_directories(interp_loop PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_loop PRIVATE MIR_INTERP_DEBUG=1)
target_link_libraries(interp_loop mir_c_obj)

add_executable (interp_loop_c "mir-tests/loop-interp.c")
target_include_directories(interp_loop_c PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_loop_c PRIVATE MIR_INTERP_DEBUG=1 MIR_C_INTERFACE=1)
target_link_libraries(interp_loop_c mir_c_obj)

add_executable (interp_sieve "mir-tests/sieve-interp.c")
target_include_directories(interp_sieve PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_sieve PRIVATE MIR_INTERP_DEBUG=1)
target_link_libraries(interp_sieve mir_c_obj)

add_executable (interp_sieve_c "mir-tests/sieve-interp.c")
target_include_directories(interp_sieve_c PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_sieve_c PRIVATE MIR_INTERP_DEBUG=1 MIR_C_INTERFACE=1)
target_link_libraries(interp_sieve_c mir_c_obj)

add_executable (interp_hi "mir-tests/hi-interp.c")
target_include_directories(interp_hi PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_hi PRIVATE MIR_INTERP_DEBUG=1)
target_link_libraries(interp_hi mir_c_obj)

add_executable (interp_args "mir-tests/args-interp.c")
target_include_directories(interp_args PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(interp_args mir_c_obj)

add_executable (interp_args_c "mir-tests/args-interp.c")
target_include_directories(interp_args_c PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(interp_args_c PRIVATE MIR_C_INTERFACE=1)
target_link_libraries(interp_args_c mir_c_obj)

add_executable (run_test_d "mir-tests/run-test.c")
target_include_directories(run_test_d PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(run_test_d PRIVATE TEST_GEN_DEBUG=1)
target_link_libraries(run_test_d mir_c_obj)

add_test(interp-test1 interp_loop)
add_test(interp-test2 interp_loop_c)
add_test(interp-test3 interp_sieve)
add_test(interp-test4 interp_sieve_c)
add_test(interp-test5 interp_hi)
add_test(interp-test6 interp_args)
add_test(interp-test7 interp_args_c)

foreach (num 8 9 10)
  add_test(interp-test${num} run_test -i ${PROJECT_SOURCE_DIR}/mir-tests/test${num}.mir)
endforeach()

if(NOT WIN32)
  add_test(interp-test11 run_test -i ${PROJECT_SOURCE_DIR}/mir-tests/test11.mir) # for long double >= 10 bytes
  add_test(interp-test12 run_test -i ${PROJECT_SOURCE_DIR}/mir-tests/test12.mir) # multiple return values
endif()

foreach (num 13 14 15 16)
  add_test(interp-test${num} run_test -i ${PROJECT_SOURCE_DIR}/mir-tests/test${num}.mir)
endforeach()

# ------------------ MIR gen tests --------------------------

foreach (num 1 2 3 4 5 6 7)
  add_test(gen-test${num} run_test -d ${PROJECT_SOURCE_DIR}/mir-tests/test${num}.mir)
endforeach()

foreach (num 8 9 10)
  add_test(gen-test${num} run_test -g ${PROJECT_SOURCE_DIR}/mir-tests/test${num}.mir)
endforeach()

if(NOT WIN32)
  add_test(gen-test11 run_test -g ${PROJECT_SOURCE_DIR}/mir-tests/test11.mir) # for long double >= 10 bytes
  add_test(gen-test12 run_test -g ${PROJECT_SOURCE_DIR}/mir-tests/test12.mir) # multiple return values
endif()

foreach (num 13 14 15 16)
  add_test(gen-test${num} run_test -g ${PROJECT_SOURCE_DIR}/mir-tests/test${num}.mir)
endforeach()

add_executable (gen_loop "mir-tests/loop-sieve-gen.c")
target_include_directories(gen_loop PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(gen_loop PRIVATE TEST_GEN_DEBUG=1 TEST_GEN_LOOP)
target_link_libraries(gen_loop mir_c_obj)

add_executable (gen_sieve "mir-tests/loop-sieve-gen.c")
target_include_directories(gen_sieve PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(gen_sieve PRIVATE TEST_GEN_DEBUG=1 TEST_GEN_SIEVE)
target_link_libraries(gen_sieve mir_c_obj)

add_test(gen-test-loop gen_loop)
add_test(gen-test-sieve gen_sieve)

# ------------------ readme example test ----------------

add_executable (readme_example "mir-tests/readme-example.c")
target_include_directories(readme_example PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(readme_example mir_c_obj)

add_test(readme-example-test readme_example)

# ------------------ mir2c test -------------------------

add_executable (mir2c_test "mir2c/mir2c.c")
target_include_directories(mir2c_test PRIVATE ${PROJECT_SOURCE_DIR})
target_compile_definitions(mir2c_test PRIVATE TEST_MIR2C)
target_link_libraries(mir2c_test mir_c_obj)

add_test(mir2c-test mir2c_test)

# ------------------ MIRNEXT C++ core tests --------------------------

add_executable(cpp_core_smoke "mir-tests/cpp-core-smoke.cpp")
target_include_directories(cpp_core_smoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cpp_core_smoke mirnext)

add_executable(cpp_sieve_smoke "mir-tests/cpp-sieve-smoke.cpp")
target_include_directories(cpp_sieve_smoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cpp_sieve_smoke mirnext)

add_executable(cpp_exec_smoke "mir-tests/cpp-exec-smoke.cpp")
target_include_directories(cpp_exec_smoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(cpp_exec_smoke mirnext doctest::doctest)

add_test(mirnext-cpp-core-smoke cpp_core_smoke)
add_test(mirnext-cpp-sieve-smoke cpp_sieve_smoke)
add_test(mirnext-cpp-exec-smoke cpp_exec_smoke)

# ------------------ c2m tests --------------------------

add_test(c2mir-simple-test c2m -v ${PROJECT_SOURCE_DIR}/sieve.c -ei)
add_test(c2mir-simple-gen-test c2m -v ${PROJECT_SOURCE_DIR}/sieve.c -eg)

set(MIR_C_TARGETS
  mir_c_obj
  varr_test
  dlist_test
  bitmap_test
  htab_test
  reduce_test
  c2m
  m2b
  b2m
  b2ctab
  simplify_test
  scan_test
  io_test
  run_test
  interp_loop
  interp_loop_c
  interp_sieve
  interp_sieve_c
  interp_hi
  interp_args
  interp_args_c
  run_test_d
  gen_loop
  gen_sieve
  readme_example
  mir2c_test
)

foreach(target IN LISTS MIR_C_TARGETS)
  mir_target_common_options(${target})
  mir_target_c_options(${target})
endforeach()

set(MIR_CXX_TARGETS
  mirnext_obj
  cpp_core_smoke
  cpp_sieve_smoke
  cpp_exec_smoke
)

foreach(target IN LISTS MIR_CXX_TARGETS)
  mir_target_common_options(${target})
  mir_target_cxx_options(${target})
endforeach()

