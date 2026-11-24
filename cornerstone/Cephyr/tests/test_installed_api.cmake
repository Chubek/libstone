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
# A root build installs shared dependencies from sibling directories.
foreach(dependency IN ITEMS "${TEST_MOONSTONE_PROLIFIC}" "${TEST_BLACKSTONE}")
  if(dependency)
    get_filename_component(soname "${dependency}" NAME)
    if(NOT EXISTS "${staging}/${INSTALL_LIBDIR}/${soname}")
      file(COPY "${dependency}" DESTINATION "${staging}/${INSTALL_LIBDIR}" FOLLOW_SYMLINK_CHAIN)
    endif()
  endif()
endforeach()
# Use a moved prefix, so resource/library discovery cannot rely on the install path.
file(REMOVE_RECURSE "${prefix}")
file(RENAME "${staging}" "${prefix}")
file(APPEND "${prefix}/${INSTALL_DATADIR}/cephyr/stdlib/include/limits.h"
  "\n#define CEPHYR_TEST_INSTALLED_HEADER 1\n")
run("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/tests/installed_consumer" -B "${consumer}"
  "-DCephyr_DIR=${prefix}/${INSTALL_LIBDIR}/cmake/Cephyr"
  "-DCEPHYR_TEST_SDK_EXAMPLE=${prefix}/${INSTALL_DATADIR}/cephyr/examples/sdk_desugar.c")
run("${CMAKE_COMMAND}" --build "${consumer}" ${config_args})
run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_CONFIG "PATH=${TEST_DIR}/empty-path"
  "${TEST_CTEST}" --test-dir "${consumer}" --output-on-failure)
run("${prefix}/${INSTALL_BINDIR}/cephyr" --version)
if(NOT EXISTS "${prefix}/${INSTALL_DATADIR}/cephyr/contracts/C11Syntax.schema.json")
  message(FATAL_ERROR "Missing installed C11 syntax schema")
endif()
run("${TEST_PYTHON}" -B "${SOURCE_DIR}/tests/test_c11_ast.py"
  "${prefix}/${INSTALL_BINDIR}/cephyr")
run("${TEST_PYTHON}" -B "${SOURCE_DIR}/tests/test_c11_interface.py"
  "${prefix}/${INSTALL_BINDIR}/cephyr")
run("${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_cephyr_cli_regression.py"
  "${prefix}/${INSTALL_BINDIR}/cephyr" CephyrCLIRegressionTests.test_40_packaged_tools_cold_cache)
set(qml_assets "${prefix}/${INSTALL_DATADIR}/cephyr/qml")
foreach(asset IN ITEMS builtin.qmb compile_rules.py qml_frontend.py)
  if(NOT EXISTS "${qml_assets}/${asset}")
    message(FATAL_ERROR "Missing installed QML asset: ${asset}")
  endif()
endforeach()
run("${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_qml_driver.py"
  "${prefix}/${INSTALL_BINDIR}/cephyr"
  QmlDriverTests.test_opt_in_builtin_does_not_require_python
  QmlDriverTests.test_profile_file_paths_merge_and_refresh)
if(TEST_MOONSTONE_PROLIFIC)
  run("${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_moonstone_tools.py"
    "${prefix}/${INSTALL_BINDIR}/cephyr" --have prolific --only-profile)
endif()

if(TEST_PYTHON_BINDINGS)
  run("${CMAKE_COMMAND}" -E env --unset=CEPHYR_LIBRARY --unset=CEPHYR_CONFIG
    "PYTHONPATH=${prefix}/${INSTALL_LIBDIR}/cephyr/python"
    "${TEST_PYTHON}" "${SOURCE_DIR}/tests/test_python_api.py")
endif()
