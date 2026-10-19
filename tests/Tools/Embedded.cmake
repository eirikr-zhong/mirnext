foreach(mode interp gen lazy bbv)
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env "MIR_TYPE=${mode}" "MIR_LIBS=" "${RUN}" "embedded payload"
    RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  string(REPLACE "\r\n" "\n" output "${output}")
  if(NOT status EQUAL 42 OR NOT output STREQUAL "embedded payload\n")
    message(FATAL_ERROR "${mode} embedded native runner: ${status}: ${output}: ${error}")
  endif()
endforeach()
