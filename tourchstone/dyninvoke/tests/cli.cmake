function(run expected pattern)
  execute_process(COMMAND "${CLI}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT "${result}" STREQUAL "${expected}" OR NOT "${output}${error}" MATCHES "${pattern}")
    message(FATAL_ERROR "dinvoke ${ARGN}: expected ${expected}/${pattern}; got ${result}: ${output}${error}")
  endif()
endfunction()

get_filename_component(fixture_dir "${FIXTURE}" DIRECTORY)
get_filename_component(fixture_name "${FIXTURE}" NAME)
run(0 "return: gpr0:4@0" --layout --abi sysv_amd64 --ret i32 --arg i32:1 --arg i32:2
    -L "${fixture_dir}" "${fixture_name}" missing_layout_symbol)
run(0 "symbol dinvk_add2: found" --inspect "${FIXTURE}" dinvk_add2)
run(1 "ABI mismatch" --abi win64 "${FIXTURE}" dinvk_add2)
run(1 "not a callable dynamic symbol" "${FIXTURE}" dinvk_not_callable)
run(2 "invalid i8 value" --ret i8 --arg i8:128 --arg i16:0 "${FIXTURE}" dinvk_narrow)
run(0 "^-31\n$" --ret i8 --arg i8:-41 --arg i16:10 "${FIXTURE}" dinvk_narrow)
run(0 "^65\n$" --arg ptr:str:ABC "${FIXTURE}" dinvk_first)
