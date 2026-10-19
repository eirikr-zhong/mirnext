# This is test tooling only. Installed/product builds do not require Python.
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(NAME mirnext-native-evidence-checks
  COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/Platform/TestAcceptanceAudit.py")
get_property(native_tests DIRECTORY PROPERTY TESTS)
list(SORT native_tests)
string(REPLACE ";" "\",\n    \"" native_tests_json "${native_tests}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/native-test-manifest-$<CONFIG>.json" CONTENT
"{\n  \"schema\": 1, \"source_fingerprint\": \"${MIRNEXT_TEST_SOURCE_FINGERPRINT}\",\n  \"configuration\": \"$<CONFIG>\", \"c_reference\": $<IF:$<BOOL:${MIRNEXT_BUILD_C_REFERENCE}>,true,false>,\n  \"tests\": [\n    \"${native_tests_json}\"\n  ]\n}\n")
add_custom_target(mirnext_check_native_evidence
  COMMAND "${Python3_EXECUTABLE}" -B "${PROJECT_SOURCE_DIR}/tests/Platform/AuditAcceptance.py"
    --source "${PROJECT_SOURCE_DIR}" --platform native --configuration "$<CONFIG>"
    --reference $<IF:$<BOOL:${MIRNEXT_BUILD_C_REFERENCE}>,on,off>
    --output "${CMAKE_CURRENT_BINARY_DIR}/native-evidence-$<CONFIG>.json" "${CMAKE_CURRENT_BINARY_DIR}"
  USES_TERMINAL)
