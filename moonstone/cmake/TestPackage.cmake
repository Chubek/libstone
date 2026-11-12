function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}\n${output}\n${errors}")
  endif()
endfunction()
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${TEST_DIR}/prefix"
  --config "${CONFIG}" --component Moonstone)
# Relocate the install, so absolute references to its original prefix cannot work.
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(prefix "${TEST_DIR}/relocated-${nonce}")
file(RENAME "${TEST_DIR}/prefix" "${prefix}")
run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/package" -B "${TEST_DIR}/consumer"
  "-DMoonstone_DIR=Moonstone_DIR-NOTFOUND" "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_BUILD_TYPE=${CONFIG}"
  "-DCMAKE_C_COMPILER=${C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
run("${CMAKE_COMMAND}" --build "${TEST_DIR}/consumer" --config "${CONFIG}")
# Windows shared libraries must be visible to both installed consumer programs.
if(WIN32)
  set(ENV{PATH} "${prefix}/bin;$ENV{PATH}")
  set(suffix ".exe")
endif()
run("${CMAKE_CTEST_COMMAND}" --test-dir "${TEST_DIR}/consumer" -C "${CONFIG}" --output-on-failure)
if(EXISTS "${prefix}/bin/moonstone-cli${suffix}")
  run("${prefix}/bin/moonstone-cli${suffix}" synthesize-demo)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/package" -B "${TEST_DIR}/missing-component"
  "-DMoonstone_DIR=Moonstone_DIR-NOTFOUND" "-DCMAKE_PREFIX_PATH=${prefix}"
  "-DMOONSTONE_TEST_MISSING_COMPONENT=ON" "-DCMAKE_C_COMPILER=${C_COMPILER}" "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT "${output}${errors}" MATCHES "Moonstone_FOUND")
  message(FATAL_ERROR "Missing required component must fail package discovery: ${output}${errors}")
endif()
