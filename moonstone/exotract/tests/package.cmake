function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Package command failed: ${ARGV}\n${output}\n${error}")
  endif()
endfunction()
if(CONFIG)
  set(config_args --config "${CONFIG}")
  set(ctest_config_args -C "${CONFIG}")
endif()
# ExoML is installed first in integrated builds; standalone imported SDKs are
# resolved through PREFIX. Move the staging prefix to check target relocation.
file(REMOVE_RECURSE "${WORK}/stage" "${WORK}/prefix" "${WORK}/consumer")
if(EXTRACT)
  run("${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${WORK}/stage" --component ExoML ${config_args})
endif()
run("${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${WORK}/stage" --component Exotract ${config_args})
file(RENAME "${WORK}/stage" "${WORK}/prefix")
# Escape the list so run() passes CMAKE_PREFIX_PATH as a single argument.
string(REPLACE ";" "\\;" prefixes "${WORK}/prefix;${PREFIX}")
run("${CMAKE_COMMAND}" -S "${SOURCE}" -B "${WORK}/consumer"
  "-DCMAKE_PREFIX_PATH=${prefixes}" "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_CXX_FLAGS=${FLAGS}"
  "-DTEST_EXTRACT=${EXTRACT}" "-DTEST_RESOURCES=${RESOURCES}" "-DTEST_COMPILER=${CC}")
run("${CMAKE_COMMAND}" --build "${WORK}/consumer" ${config_args})
run("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/consumer" ${ctest_config_args} --output-on-failure)
