function(mirnext_add_example target name source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE MIRNext::MIRNext)
  mir_target_cxx_options(${target})
  if(BUILD_TESTING)
    add_test(NAME ${name} COMMAND ${target})
  endif()
endfunction()

mirnext_add_example(mirnext_native_simd_example mirnext-native-simd-example native-simd.cpp)

mirnext_add_example(mirnext_custom_code_memory_example mirnext_custom_code_memory_example
  custom-code-memory.cpp)
mirnext_add_example(mirnext_native_scope_example mirnext_native_scope_example
  native-stack-scope.cpp)
mirnext_add_example(mirnext_native_call_example mirnext_native_call_example
  native-calls.cpp)
mirnext_add_example(mirnext_native_variable_example mirnext_native_variable_example
  native-variables.cpp)
mirnext_add_example(mirnext_native_expression_example mirnext_native_expression_example
  native-expressions.cpp)
mirnext_add_example(mirnext_native_simplify_example mirnext_native_simplify_example
  native-simplify.cpp)
mirnext_add_example(mirnext_native_inline_example mirnext_native_inline_example
  native-inline.cpp)
mirnext_add_example(mirnext_native_analysis_example mirnext_native_analysis_example
  native-analysis.cpp)
mirnext_add_example(mirnext_native_ssa_example mirnext_native_ssa_example
  native-ssa.cpp)
mirnext_add_example(mirnext_native_address_example mirnext_native_address_example
  native-address.cpp)
mirnext_add_example(mirnext_native_cloning_example mirnext_native_cloning_example
  native-cloning.cpp)
mirnext_add_example(mirnext_native_gvn_example mirnext_native_gvn_example
  native-gvn.cpp)
mirnext_add_example(mirnext_native_machine_ir_example mirnext-native-machine-ir-example
  native-machine-ir.cpp)
mirnext_add_example(mirnext_native_compile_example mirnext-native-compile-example
  native-compile.cpp)
mirnext_add_example(mirnext_native_bbv_example mirnext_native_bbv_example
  native-bbv.cpp)
mirnext_add_example(mirnext_bbv_session_example mirnext_bbv_session_example
  bbv-session.cpp)
mirnext_add_example(mirnext_native_session_example mirnext-native-session-example
  native-session.cpp)
mirnext_add_example(mirnext_mixed_engines_example mirnext-mixed-engines-example
  mixed-engines.cpp)
mirnext_add_example(mirnext_cross_module_example mirnext-cross-module-example
  native-cross-module.cpp)
mirnext_add_example(mirnext_native_interpreter_example mirnext-native-interpreter-example
  native-interpreter.cpp)
mirnext_add_example(mirnext_execution_values_example mirnext-execution-values-example
  execution-values.cpp)
mirnext_add_example(mirnext_interpreted_callback_example mirnext-interpreted-callback-example
  interpreted-callback.cpp)
