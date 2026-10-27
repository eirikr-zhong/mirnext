add_library(mirnext
  IR/Expressions.cpp
  IR/Variables.cpp
  IR/BuilderCalls.cpp
  IR/IR.cpp
  IR/IRBuilder.cpp
  IR/SSA.cpp
  IR/Clone.cpp
  IR/Dump.cpp
  IR/Materialize.cpp

  Analysis/ControlFlow.cpp
  Analysis/FunctionGraph.cpp
  Analysis/ConstructionOrder.cpp

  Transforms/PassManager.cpp
  Transforms/Materialize.cpp
  Transforms/Simplify.cpp
  Transforms/Inline.cpp
  Transforms/PrepareCFG.cpp
  Transforms/SSA.cpp
  Transforms/Address.cpp
  Transforms/BlockCloning.cpp
  Transforms/IntegerFolding.cpp
  Transforms/GVN.cpp
  Transforms/CopyPropagation.cpp
  Transforms/DeadStores.cpp
  Transforms/DeadCode.cpp
  Transforms/Motion.cpp
  Transforms/SSACombine.cpp
  Transforms/JumpOptimization.cpp
  Transforms/Optimization.cpp

  Target/Addressing.cpp
  Target/TargetMachine.cpp
  Target/X86ABI.cpp

  CodeGen/MachineIR.cpp
  CodeGen/Prepare.cpp
  CodeGen/GraphAnalysis.cpp
  CodeGen/Liveness.cpp
  CodeGen/Lowering.cpp
  CodeGen/AArch64Lowering.cpp
  CodeGen/X86Lowering.cpp
  CodeGen/AArch64Patterns.cpp
  CodeGen/X86Patterns.cpp
  CodeGen/X86EncodingRecipes.cpp
  CodeGen/InstructionPatterns.cpp
  CodeGen/LegalizeOperands.cpp
  CodeGen/Coalescing.cpp
  CodeGen/LiveRanges.cpp
  CodeGen/Assignment.cpp
  CodeGen/RegisterRewrite.cpp
  CodeGen/SpillEmitter.cpp
  CodeGen/EdgeSpills.cpp
  CodeGen/PostAllocation.cpp
  CodeGen/Frame.cpp
  CodeGen/X86Frame.cpp
  CodeGen/Encoding.cpp
  CodeGen/AArch64Encoding.cpp
  CodeGen/X86Encoding.cpp
  CodeGen/CompiledFunction.cpp
  CodeGen/CompiledBlock.cpp
  CodeGen/Compilation.cpp
  CodeGen/BlockVersioning.cpp
  CodeGen/BBVSpecialization.cpp
  CodeGen/RuntimeEntry.cpp
  CodeGen/X86RuntimeEntry.cpp
  CodeGen/InvocationEntry.cpp
  CodeGen/NativeBoundary.cpp

  Execution/CodeMemory.cpp
  Execution/Linker.cpp
  Execution/Builtins.cpp
  Execution/Session.cpp
  Execution/LoadedProgram.cpp
  Execution/NativeProgram.cpp
  Execution/Function.cpp
  Execution/ProgramImage.cpp
  Execution/BBVRuntime.cpp
  Execution/Recovery.cpp
  Execution/SessionBBV.cpp
  Execution/NativeBoundary.cpp
  Execution/Value.cpp
  Execution/NativeCalls.cpp
  Execution/InterpreterEntries.cpp
  Execution/InterpreterScalar.cpp
  Execution/Interpreter.cpp
  Execution/InterpreterFrame.cpp
  Execution/InterpreterStack.cpp
  Execution/InterpreterEvaluate.cpp

  Serialization/Text.cpp
  Serialization/Binary.cpp
  Serialization/Compression.cpp
)
add_library(MIRNext::MIRNext ALIAS mirnext)
set_target_properties(mirnext PROPERTIES EXPORT_NAME MIRNext POSITION_INDEPENDENT_CODE ON)
target_include_directories(mirnext PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(mirnext PUBLIC cxx_std_20)
if(NOT MSVC)
  target_compile_options(mirnext PRIVATE -fsigned-char)
endif()
mir_target_cxx_options(mirnext)
if(MIRNEXT_ENABLE_ASAN)
  target_link_options(mirnext INTERFACE -fsanitize=address)
endif()
if(BUILD_SHARED_LIBS)
  set_target_properties(mirnext PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS TRUE)
endif()

install(TARGETS mirnext EXPORT MIRNextTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/mirnext DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT MIRNextTargets NAMESPACE MIRNext:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MIRNext)
install(FILES ${PROJECT_SOURCE_DIR}/cmake/MIRNextConfig.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MIRNext)
install(FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/NOTICE DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/MIRNext)
