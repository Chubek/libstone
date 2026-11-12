string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(owned "${CACHE}/${suffix}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "LD_PRELOAD=${INJECT}" "PROLIFIC_CACHE=${owned}" "PROLIFIC_RUN=injected"
  "${HOST}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "injected host failed: ${result}: ${error}")
endif()
execute_process(COMMAND "${CHECK}" "${owned}" RESULT_VARIABLE result ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "injected cache validation failed: ${result}: ${error}")
endif()
file(REMOVE_RECURSE "${owned}")
