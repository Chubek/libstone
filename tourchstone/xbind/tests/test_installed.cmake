function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "${status}: ${ARGV}")
  endif()
endfunction()

set(config_arg)
set(ctest_config_arg)
if(NOT CONFIG STREQUAL "")
  set(config_arg --config "${CONFIG}")
  set(ctest_config_arg -C "${CONFIG}")
endif()
set(prefix "${WORK}/prefix")
run("${CMAKE_COMMAND}" --install "${BUILD}" --prefix "${prefix}" ${config_arg})
run("${CMAKE_COMMAND}" -S "${SOURCE}/tests/consumer" -B "${WORK}/consumer"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_C_COMPILER=${CC}"
    "-DCMAKE_CXX_COMPILER=${CXX}" "-DCMAKE_C_FLAGS=${CFLAGS}"
    "-DCMAKE_CXX_FLAGS=${CXXFLAGS}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
run("${CMAKE_COMMAND}" --build "${WORK}/consumer" ${config_arg})
if(WIN32)
  set(ENV{PATH} "${prefix}/${BINDIR};$ENV{PATH}")
endif()
run("${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/consumer"
    --output-on-failure ${ctest_config_arg})

if(NOT EXISTS "${prefix}/${DATADIR}/xbind/typedefs/Lua-Default.sexpr")
  message(FATAL_ERROR "XBind Lua data not installed")
endif()
set(cli "${prefix}/${BINDIR}/xbind${EXE_SUFFIX}")
run("${cli}" --capabilities)
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=XBIND_DATA_DIR
    "${cli}" --emit-lua "${SOURCE}/examples/hello/hello.xbind"
    RESULT_VARIABLE status OUTPUT_VARIABLE lua)
if(NOT status STREQUAL "0" OR NOT lua MATCHES "luaopen_hello")
  message(FATAL_ERROR "Installed CLI could not emit hello Lua bindings (${status})")
endif()
