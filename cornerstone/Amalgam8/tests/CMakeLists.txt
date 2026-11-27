function(amalgam8_test name source)
  add_executable(amalgam8_${name}_test "${source}")
  target_link_libraries(amalgam8_${name}_test PRIVATE Amalgam8::compiler)
  set_target_properties(amalgam8_${name}_test PROPERTIES CXX_EXTENSIONS OFF)
  libstone_warnings(amalgam8_${name}_test)
  add_test(NAME amalgam8.${name} COMMAND amalgam8_${name}_test ${ARGN})
endfunction()

amalgam8_test(embedding acceptance_embed.cpp)
amalgam8_test(frontend W1Frontend.cpp "${CMAKE_CURRENT_SOURCE_DIR}/W1Fixtures")
amalgam8_test(expansion W2Expansion.cpp)
amalgam8_test(semantic W3Semantic.cpp)
amalgam8_test(native_registry W5_registry.cpp)

if(TARGET amalgam8 AND NOT CMAKE_CROSSCOMPILING)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  function(amalgam8_script_test name runner script)
    add_test(NAME amalgam8.${name} COMMAND ${CMAKE_COMMAND} -E env
      "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
      "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}"
      "${runner}" "${CMAKE_CURRENT_SOURCE_DIR}/${script}" ${ARGN})
    set_tests_properties(amalgam8.${name} PROPERTIES TIMEOUT 120)
  endfunction()
  amalgam8_script_test(grammar "${Python3_EXECUTABLE}" W1Grammar.py "$<TARGET_FILE:amalgam8>")
  amalgam8_script_test(emitter sh W4.sh)
  amalgam8_script_test(examples "${Python3_EXECUTABLE}" W5_examples.py "$<TARGET_FILE:amalgam8>")
  amalgam8_script_test(packaging "${Python3_EXECUTABLE}" W6_standalone.py "--amalgam8" "$<TARGET_FILE:amalgam8>")
  add_executable(amalgam8_interop_fixture W7.cpp)
  target_link_libraries(amalgam8_interop_fixture PRIVATE Amalgam8::compiler)
  set_target_properties(amalgam8_interop_fixture PROPERTIES CXX_EXTENSIONS OFF)
  libstone_warnings(amalgam8_interop_fixture)
  amalgam8_script_test(interop "${Python3_EXECUTABLE}" W7.py --driver "$<TARGET_FILE:amalgam8_interop_fixture>")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${LIBSTONE_SOURCE_DIR}/.git")
    amalgam8_script_test(catalog "${Python3_EXECUTABLE}" "../tools/catalog_audit.py")
  endif()

  add_test(NAME amalgam8.acceptance COMMAND ${CMAKE_COMMAND}
    -DAMALGAM8=$<TARGET_FILE:amalgam8>
    "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}"
    "-DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/../examples"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/acceptance"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/acceptance.cmake")
endif()

if(NOT CMAKE_CROSSCOMPILING)
  add_test(NAME amalgam8.install COMMAND ${CMAKE_COMMAND}
    "-DBUILD=${PROJECT_BINARY_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/installed"
    "-DCXX=${CMAKE_CXX_COMPILER}" "-DCONFIG=$<CONFIG>"
    "-DEMBED=${CMAKE_CURRENT_SOURCE_DIR}/acceptance_embed.cpp"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/acceptance_install.cmake")
endif()
