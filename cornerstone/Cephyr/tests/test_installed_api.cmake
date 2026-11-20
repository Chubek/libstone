function(run)
  execute_process(COMMAND ${ARGV} WORKING_DIRECTORY "${TEST_DIR}" RESULT_VARIABLE result)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Command failed (${result}): ${ARGV}")
  endif()
endfunction()

file(MAKE_DIRECTORY "${TEST_DIR}")
set(staging "${TEST_DIR}/staging")
set(prefix "${TEST_DIR}/relocated-prefix")
set(consumer "${TEST_DIR}/consumer")
set(config_args)
if(NOT "${TEST_CONFIG}" STREQUAL "")
  list(APPEND config_args --config "${TEST_CONFIG}")
endif()

# Install from Cephyr's binary directory so a parent build need not install
# unrelated components.
run("${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${staging}" ${config_args})
# Use a moved prefix, so resource/library discovery cannot rely on the install path.
file(REMOVE_RECURSE "${prefix}")
file(RENAME "${staging}" "${prefix}")
file(APPEND "${prefix}/${INSTALL_DATADIR}/cephyr/stdlib/include/limits.h"
  "\n#define CEPHYR_TEST_INSTALLED_HEADER 1\n")
run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/installed_consumer" -B "${consumer}"
  "-DCephyr_DIR=${prefix}/${INSTALL_LIBDIR}/cmake/Cephyr"
  "-DCEPHYR_TEST_SDK_EXAMPLE=${prefix}/${INSTALL_DATADIR}/cephyr/examples/sdk_desugar.c")
run("${CMAKE_COMMAND}" --build "${consumer}" ${config_args})
run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_CONFIG
  "${TEST_CTEST}" --test-dir "${consumer}" --output-on-failure)
run("${prefix}/${INSTALL_BINDIR}/cephyr" --version)

if(TEST_PYTHON_BINDINGS)
  run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_LIBRARY --unset=CEPHYR_CONFIG
    "PYTHONPATH=${prefix}/${INSTALL_LIBDIR}/cephyr/python"
    "${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_python_api.py")
endif()
