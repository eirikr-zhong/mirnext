include_directories(${PROJECT_SOURCE_DIR}/lib)
find_package(Threads REQUIRED)

if(MIRNEXT_ENABLE_NATIVE_EVIDENCE)
  add_test(NAME mirnext-native-evidence-checks COMMAND ${Python3_EXECUTABLE} -B
    ${CMAKE_CURRENT_SOURCE_DIR}/Platform/TestAcceptanceAudit.py)
endif()
include(FetchContent)
set(DOCTEST_WITH_TESTS OFF)
set(DOCTEST_NO_INSTALL ON)
set(DOCTEST_WITH_MAIN_IN_STATIC_LIB OFF)
FetchContent_Declare(
  doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG v2.4.12
)
FetchContent_MakeAvailable(doctest)
function(mirnext_add_test target name)
  cmake_parse_arguments(ARG "WITH_RTTI" "" "SOURCES" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE MIRNext::MIRNext doctest::doctest Threads::Threads)
  if(ARG_WITH_RTTI)
    mir_target_cxx_options(${target} WITH_RTTI)
  else()
    mir_target_cxx_options(${target})
  endif()
  add_test(NAME ${name} COMMAND ${target})
endfunction()

if(MIRNEXT_BUILD_UTILITIES AND MIRNEXT_BUILD_SCRIPT_TESTS)
  add_subdirectory(Tools)
endif()

set(MIRNEXT_TEST_EXPECTED_TARGET "" CACHE STRING "Required native test ABI (empty detects the host)")
add_executable(mirnext_result_test Support/Result.cpp)
target_include_directories(mirnext_result_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(mirnext_result_test PRIVATE doctest::doctest Threads::Threads)
mir_target_cxx_options(mirnext_result_test)
add_test(NAME mirnext-support-result COMMAND mirnext_result_test)
add_executable(mirnext_result_fault_probe Support/ResultFault.cpp)
target_include_directories(mirnext_result_fault_probe PRIVATE ${PROJECT_SOURCE_DIR}/include)
mir_target_cxx_options(mirnext_result_fault_probe)
if(MIRNEXT_BUILD_SCRIPT_TESTS)
  add_test(NAME mirnext-support-result-fault COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/tests/Support/CheckResultFault.py $<TARGET_FILE:mirnext_result_fault_probe>)
endif()
set_property(CACHE MIRNEXT_TEST_EXPECTED_TARGET PROPERTY STRINGS "" x86_64-sysv x86_64-windows aarch64-linux aarch64-macos)
add_executable(mirnext_public_api_test Support/PublicAPI.cpp)
target_include_directories(mirnext_public_api_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
mir_target_cxx_options(mirnext_public_api_test)
add_test(NAME mirnext-public-api COMMAND mirnext_public_api_test)
if(NOT MIRNEXT_TEST_EXPECTED_TARGET MATCHES "^(|x86_64-sysv|x86_64-windows|aarch64-linux|aarch64-macos)$")
  message(FATAL_ERROR "Unknown MIRNEXT_TEST_EXPECTED_TARGET: ${MIRNEXT_TEST_EXPECTED_TARGET}")
endif()
if(BUILD_SHARED_LIBS)
  set(linkage shared)
else()
  set(linkage static)
endif()
add_executable(mirnext_native_acceptance
  Platform/NativeAcceptanceMain.cpp Platform/NativeHost.cpp Platform/NativeAcceptance.cpp)
target_compile_definitions(mirnext_native_acceptance PRIVATE MIRNEXT_TEST_LINKAGE="${linkage}")
if(MIRNEXT_ENABLE_NATIVE_EVIDENCE)
  set(acceptance_identity "${CMAKE_CURRENT_BINARY_DIR}/AcceptanceIdentity.hpp")
  add_custom_target(mirnext_acceptance_identity
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/Platform/SourceFingerprint.py
      --source ${PROJECT_SOURCE_DIR} --header ${acceptance_identity}
      --build ${PROJECT_BINARY_DIR} --configuration $<CONFIG> --ctest ${CMAKE_CTEST_COMMAND}
    BYPRODUCTS ${acceptance_identity}
    VERBATIM)
  add_dependencies(mirnext_native_acceptance mirnext_acceptance_identity)
  target_compile_definitions(mirnext_native_acceptance PRIVATE
    MIRNEXT_TEST_IDENTITY_HEADER="${acceptance_identity}")
else()
  target_compile_definitions(mirnext_native_acceptance PRIVATE MIRNEXT_TEST_SOURCE_FINGERPRINT="")
endif()
target_compile_definitions(mirnext_native_acceptance PRIVATE MIRNEXT_TEST_CONFIGURATION="$<CONFIG>")
target_link_libraries(mirnext_native_acceptance PRIVATE MIRNext::MIRNext)
mir_target_cxx_options(mirnext_native_acceptance)
mirnext_add_test(mirnext_native_host_test mirnext-native-host
  SOURCES Platform/HostChecks.cpp Platform/NativeHost.cpp)
set(acceptance_report "${PROJECT_BINARY_DIR}/native-acceptance-${linkage}-$<CONFIG>.json")
if(MIRNEXT_BUILD_SCRIPT_TESTS)
  set(acceptance_command ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/Platform/CheckAcceptance.py
    --run $<TARGET_FILE:mirnext_native_acceptance>
    --report ${acceptance_report} --configuration $<CONFIG> --linkage ${linkage})
  if(MIRNEXT_ENABLE_NATIVE_EVIDENCE)
    list(APPEND acceptance_command --source ${PROJECT_SOURCE_DIR}
      --manifest ${PROJECT_BINARY_DIR}/native-test-manifest-$<CONFIG>.json)
  endif()
else()
  set(acceptance_command $<TARGET_FILE:mirnext_native_acceptance> --report ${acceptance_report})
endif()
if(MIRNEXT_TEST_EXPECTED_TARGET)
  list(APPEND acceptance_command --expected ${MIRNEXT_TEST_EXPECTED_TARGET})
endif()
add_test(NAME mirnext-native-acceptance-${linkage} COMMAND ${acceptance_command})
set_tests_properties(mirnext-native-acceptance-${linkage} PROPERTIES LABELS "native-acceptance" TIMEOUT 120)
mirnext_add_test(mirnext_ir_test mirnext-unified-ir
  SOURCES IR/Core.cpp IR/DataUses.cpp)
mirnext_add_test(mirnext_builder_test mirnext-explicit-builder
  SOURCES IR/Builder.cpp IR/NativeBuilder.cpp)
mirnext_add_test(mirnext_text_test mirnext-unified-text WITH_RTTI
  SOURCES Serialization/Text.cpp)
target_compile_definitions(mirnext_text_test PRIVATE MIRNEXT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
mirnext_add_test(mirnext_binary_test mirnext-unified-binary
  SOURCES Serialization/Binary.cpp)
target_compile_definitions(mirnext_binary_test PRIVATE MIRNEXT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
mirnext_add_test(mirnext_scope_test mirnext-unified-scopes
  SOURCES IR/Scopes.cpp)
mirnext_add_test(mirnext_call_test mirnext-unified-calls
  SOURCES IR/Calls.cpp)
mirnext_add_test(mirnext_variable_test mirnext-unified-variables
  SOURCES IR/Variables.cpp)
mirnext_add_test(mirnext_expression_test mirnext-unified-expressions
  SOURCES IR/Expressions.cpp)
mirnext_add_test(mirnext_rewriter_test mirnext-unified-rewriter
  SOURCES IR/Rewriter.cpp)
mirnext_add_test(mirnext_analysis_test mirnext-unified-analysis
  SOURCES Analysis/Core.cpp Analysis/Module.cpp)
mirnext_add_test(mirnext_function_graph_test mirnext-unified-function-graph
  SOURCES Analysis/FunctionGraph.cpp)
mirnext_add_test(mirnext_simplify_test mirnext-unified-simplify
  SOURCES Transforms/Simplify.cpp)
mirnext_add_test(mirnext_inline_test mirnext-unified-inline
  SOURCES Transforms/Inline.cpp)
mirnext_add_test(mirnext_ssa_test mirnext-unified-ssa
  SOURCES Transforms/SSA.cpp)
mirnext_add_test(mirnext_address_test mirnext-unified-address
  SOURCES Transforms/Address.cpp)
mirnext_add_test(mirnext_cloning_test mirnext-unified-cloning
  SOURCES Transforms/BlockCloning.cpp)
mirnext_add_test(mirnext_gvn_test mirnext-unified-gvn
  SOURCES Transforms/GVN.cpp)
mirnext_add_test(mirnext_memory_test mirnext-unified-memory
  SOURCES Transforms/Memory.cpp)
mirnext_add_test(mirnext_motion_test mirnext-unified-motion
  SOURCES Transforms/Motion.cpp)
mirnext_add_test(mirnext_combine_test mirnext-unified-combine
  SOURCES Transforms/SSACombine.cpp)
mirnext_add_test(mirnext_jump_test mirnext-unified-jump
  SOURCES Transforms/JumpOptimization.cpp)
mirnext_add_test(mirnext_optimization_test mirnext-unified-optimization
  SOURCES Transforms/Optimization.cpp)
mirnext_add_test(mirnext_machine_ir_test mirnext-unified-machine-ir
  SOURCES
    CodeGen/MachineIR.cpp
    CodeGen/Analysis.cpp
    CodeGen/Lowering.cpp
    CodeGen/Legalization.cpp
    CodeGen/X86Patterns.cpp
    CodeGen/X86ABI.cpp
    CodeGen/X86Allocation.cpp
    CodeGen/X86Frame.cpp
    CodeGen/X86Encoding.cpp
    CodeGen/RegisterAllocation.cpp
    CodeGen/RegisterRewrite.cpp
    CodeGen/EdgeSpills.cpp
    CodeGen/PostAllocation.cpp
    CodeGen/Frame.cpp
    CodeGen/Encoding.cpp
    CodeGen/BlockVersioning.cpp
    CodeGen/BlockEncoding.cpp)
mirnext_add_test(mirnext_code_memory_native_test mirnext-unified-code-memory
  SOURCES Execution/CodeMemory.cpp)
add_executable(mirnext_linker_native_test
  Execution/Linker.cpp Execution/Builtins.cpp Execution/Session.cpp
  Execution/SessionModes.cpp Execution/NativeEntries.cpp Execution/Callable.cpp
  Execution/Lazy.cpp Execution/RuntimeEntry.cpp Execution/BlockEntry.cpp
  Execution/BBVRuntime.cpp Execution/Recovery.cpp Execution/InvocationState.cpp
  Execution/NativeBoundary.cpp Execution/SessionBBV.cpp)
target_sources(mirnext_linker_native_test PRIVATE Execution/MixedEngines.cpp Execution/SourceSnapshots.cpp
  Execution/SeededExecution.cpp Execution/OriginalCorpus.cpp Execution/Regressions.cpp
  Execution/ClassicPrograms.cpp Execution/UtilityPrograms.cpp)
target_compile_definitions(mirnext_linker_native_test PRIVATE MIRNEXT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(mirnext_linker_native_test PRIVATE MIRNext::MIRNext doctest::doctest Threads::Threads)
mir_target_cxx_options(mirnext_linker_native_test)
add_test(NAME mirnext-unified-linker COMMAND mirnext_linker_native_test)
mirnext_add_test(mirnext_interpreter_native_test mirnext-unified-interpreter
  SOURCES Execution/Interpreter.cpp)
if(MIRNEXT_BUILD_SCRIPT_TESTS)
  add_test(NAME mirnext-unified-architecture COMMAND ${Python3_EXECUTABLE} -B
    ${PROJECT_SOURCE_DIR}/tests/Architecture.py ${PROJECT_SOURCE_DIR})
endif()
add_executable(mirnext_format_corpus_test Serialization/Corpus.cpp)
target_link_libraries(mirnext_format_corpus_test PRIVATE MIRNext::MIRNext doctest::doctest Threads::Threads)
target_compile_definitions(mirnext_format_corpus_test PRIVATE MIRNEXT_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
mir_target_cxx_options(mirnext_format_corpus_test)
add_test(NAME mirnext-native-format-corpus COMMAND mirnext_format_corpus_test)
