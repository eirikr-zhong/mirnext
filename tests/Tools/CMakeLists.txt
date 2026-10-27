add_library(mirnext_cli_test_library SHARED HostLibrary.cpp)
mir_target_cxx_options(mirnext_cli_test_library)
add_test(NAME mirnext-native-commands COMMAND ${Python3_EXECUTABLE} -B
  ${CMAKE_CURRENT_SOURCE_DIR}/Commands.py
  --m2b $<TARGET_FILE:m2b> --b2m $<TARGET_FILE:b2m> --b2cpp $<TARGET_FILE:b2cpp>
  --run $<TARGET_FILE:mir-bin-run> --host $<TARGET_FILE:mirnext_cli_test_library>
  --work ${PROJECT_BINARY_DIR}/native-command-tests)
set(test_embedded_header "${PROJECT_BINARY_DIR}/native-embedded-test.inc")
add_custom_command(OUTPUT "${test_embedded_header}"
  COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/Embed.py
    --m2b $<TARGET_FILE:m2b> --b2cpp $<TARGET_FILE:b2cpp>
    --input ${CMAKE_CURRENT_SOURCE_DIR}/Embedded.mir --output ${test_embedded_header}
  DEPENDS m2b b2cpp ${CMAKE_CURRENT_SOURCE_DIR}/Embedded.mir ${CMAKE_CURRENT_SOURCE_DIR}/Embed.py VERBATIM)
add_executable(mirnext_embedded_test ${PROJECT_SOURCE_DIR}/tools/EmbeddedRun.cpp
  ${PROJECT_SOURCE_DIR}/tools/Run.cpp "${test_embedded_header}")
target_compile_definitions(mirnext_embedded_test PRIVATE MIRNEXT_EMBEDDED_HEADER="${test_embedded_header}")
target_link_libraries(mirnext_embedded_test PRIVATE MIRNext::MIRNext ${CMAKE_DL_LIBS})
mir_target_cxx_options(mirnext_embedded_test)
add_test(NAME mirnext-native-embedded COMMAND ${Python3_EXECUTABLE} -B
  ${CMAKE_CURRENT_SOURCE_DIR}/Embedded.py $<TARGET_FILE:mirnext_embedded_test>)
