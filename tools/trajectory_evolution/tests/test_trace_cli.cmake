execute_process(COMMAND "${EVOLUTION_EXE}" doctor "${TRACE_INPUT}" --level L0
    RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT report MATCHES "\"effective_level\": *\"L0\"")
  message(FATAL_ERROR "doctor did not enable a valid L0 trace: ${error}")
endif()
execute_process(COMMAND "${EVOLUTION_EXE}" doctor "${TRACE_INPUT}" --level L4 --fallback strict
    RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE error)
if(NOT result EQUAL 3 OR NOT report MATCHES "\"effective_level\": *\"LEVEL_NONE\"")
  message(FATAL_ERROR "doctor falsely certified strict L4")
endif()
execute_process(COMMAND "${EVOLUTION_EXE}" doctor "${TRACE_INPUT}" --level L4 --fallback downgrade
    RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE error)
if(NOT result EQUAL 0 OR NOT report MATCHES "\"effective_level\": *\"L0\"")
  message(FATAL_ERROR "doctor did not explicitly downgrade unsupported L4")
endif()
execute_process(COMMAND "${EVOLUTION_EXE}" doctor "${TRACE_INPUT}" --level fake-sensitive-value
    RESULT_VARIABLE result OUTPUT_VARIABLE report ERROR_VARIABLE error)
if(NOT result EQUAL 2 OR report MATCHES "fake-sensitive-value" OR error MATCHES "fake-sensitive-value")
  message(FATAL_ERROR "invalid doctor argument exposed input or returned success")
endif()
