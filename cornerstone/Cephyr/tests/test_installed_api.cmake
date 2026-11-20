function(run)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_DIR}")
set(prefix "${TEST_DIR}/prefix")
set(consumer "${TEST_DIR}/consumer")
set(config_args)
if(NOT "${TEST_CONFIG}" STREQUAL "")
  list(APPEND config_args --config "${TEST_CONFIG}")
endif()

# Install from Cephyr's binary directory so a parent build need not install
# unrelated components.
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${prefix}" ${config_args})
file(APPEND "${prefix}/${INSTALL_DATADIR}/cephyr/stdlib/include/limits.h"
  "\n#define CEPHYR_TEST_INSTALLED_HEADER 1\n")
run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/installed_consumer" -B "${consumer}"
  "-DCMAKE_PREFIX_PATH=${prefix}")
run("${CMAKE_COMMAND}" --build "${consumer}" ${config_args})
run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_CONFIG
  "${TEST_CTEST}" --test-dir "${consumer}" --output-on-failure)
run("${prefix}/${INSTALL_BINDIR}/cephyr" --version)

if(TEST_PYTHON_BINDINGS)
  run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_LIBRARY
    "PYTHONPATH=${prefix}/${INSTALL_LIBDIR}/cephyr/python"
    "${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_python_api.py")
endif()
