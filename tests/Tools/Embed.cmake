execute_process(COMMAND "${M2B}" INPUT_FILE "${INPUT}" OUTPUT_FILE "${OUTPUT}.bmir"
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "embedded text conversion: ${status}: ${error}")
endif()
execute_process(COMMAND "${B2CTAB}" INPUT_FILE "${OUTPUT}.bmir" OUTPUT_FILE "${OUTPUT}"
  RESULT_VARIABLE status ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "embedded array conversion: ${status}: ${error}")
endif()
