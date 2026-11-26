include(CMakePackageConfigHelpers)
include("${CMAKE_CURRENT_LIST_DIR}/VendoredLua.cmake")

set(ASDL_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
set(ASDL_STDLIB_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/asdl/stdlib")
set(asdl_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/ASDL")

set(asdl_execution_lua "${CMAKE_CURRENT_SOURCE_DIR}/scripts/asdl_execution.lua")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asdl_execution_lua}")
file(READ "${asdl_execution_lua}" ASDL_EXECUTION_SOURCE)
configure_file(cmake/asdl_execution_source.hpp.in asdl_execution_source.hpp @ONLY)

asdl_add_vendored_lua()
add_library(asdl_runtime
  src/schema.cpp src/parser.cpp src/resolver.cpp
  src/codegen.cpp src/CAPI.cpp src/Dot.cpp
  src/lua.cpp src/execution.cpp runtime/asdl_loader.c
  $<TARGET_OBJECTS:asdl_lua_vendor>)
add_library(ASDL::runtime ALIAS asdl_runtime)
set_target_properties(asdl_runtime PROPERTIES EXPORT_NAME runtime OUTPUT_NAME asdl
  POSITION_INDEPENDENT_CODE ON)
target_compile_features(asdl_runtime PUBLIC cxx_std_17 PRIVATE c_std_11 cxx_std_20)
target_include_directories(asdl_runtime
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/runtime"
          "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(asdl_runtime SYSTEM PRIVATE
  "${ASDL_THIRD_PARTY_DIR}/lua" "${ASDL_THIRD_PARTY_DIR}/kaguya/include")
target_compile_options(asdl_runtime PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-fexceptions>)
if(UNIX)
  target_link_libraries(asdl_runtime PRIVATE m)
endif()
if(BUILD_SHARED_LIBS)
  target_compile_definitions(asdl_runtime PUBLIC ASDL_SHARED PRIVATE ASDL_BUILDING_LIBRARY)
endif()

if(ASDL_BUILD_TOOLS)
  add_executable(asdlc src/cli.cpp)
  target_link_libraries(asdlc PRIVATE ASDL::runtime)
  add_executable(asdl-run src/runtime_cli.cpp)
  target_link_libraries(asdl-run PRIVATE ASDL::runtime)
  target_include_directories(asdl-run PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
  target_include_directories(asdl-run SYSTEM PRIVATE "${ASDL_THIRD_PARTY_DIR}/lua")

  file(RELATIVE_PATH ASDL_BIN_TO_STDLIB "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_DATADIR}/asdl/stdlib")
  configure_file(cmake/asdl_runtime_paths.hpp.in asdl_runtime_paths.hpp @ONLY)
  add_custom_target(asdl_run_stdlib
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/std"
      "$<TARGET_FILE_DIR:asdl-run>/asdl-stdlib/std"
    VERBATIM)
  add_dependencies(asdl-run asdl_run_stdlib)

  file(RELATIVE_PATH asdl_bin_to_lib "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(BUILD_SHARED_LIBS AND APPLE)
    set_target_properties(asdlc asdl-run PROPERTIES INSTALL_RPATH "@loader_path/${asdl_bin_to_lib}")
  elseif(BUILD_SHARED_LIBS AND UNIX)
    set_target_properties(asdlc asdl-run PROPERTIES INSTALL_RPATH "$ORIGIN/${asdl_bin_to_lib}")
  endif()
  install(TARGETS asdlc asdl-run EXPORT ASDLTargets RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

install(TARGETS asdl_runtime EXPORT ASDLTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY include/asdl DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY stdlib/std DESTINATION "${ASDL_STDLIB_INSTALL_DIR}")
install(FILES stdlib/API.md stdlib/NATIVE.md stdlib/README.md DESTINATION "${ASDL_STDLIB_INSTALL_DIR}")
install(FILES README.md GRAMMAR.md embedding.md DESTINATION "${CMAKE_INSTALL_DOCDIR}")
install(FILES runtime/LOADER.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/runtime")
install(FILES scripts/execution.md DESTINATION "${CMAKE_INSTALL_DOCDIR}/scripts")
install(FILES "${ASDL_THIRD_PARTY_DIR}/lua/lua.h"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/asdl/licenses" RENAME Lua-Copyright.h)
install(FILES "${ASDL_THIRD_PARTY_DIR}/kaguya/LICENSE_1_0.txt"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/asdl/licenses" RENAME Kaguya-LICENSE.txt)
configure_package_config_file(cmake/ASDLConfig.cmake.in ASDLConfig.cmake
  INSTALL_DESTINATION "${asdl_package_dir}"
  PATH_VARS ASDL_STDLIB_INSTALL_DIR)
write_basic_package_version_file(ASDLConfigVersion.cmake COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ASDLConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/ASDLConfigVersion.cmake"
  DESTINATION "${asdl_package_dir}")
install(EXPORT ASDLTargets NAMESPACE ASDL:: DESTINATION "${asdl_package_dir}")

if(ASDL_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  add_executable(asdl_schema_model_test tests/schema_model.cpp)
  target_link_libraries(asdl_schema_model_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.schema-model COMMAND asdl_schema_model_test)

  add_executable(asdl_grammar_test tests/grammar.cpp)
  target_link_libraries(asdl_grammar_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.grammar COMMAND asdl_grammar_test "${CMAKE_CURRENT_SOURCE_DIR}")

  add_executable(asdl_dot_test tests/dot_model.cpp)
  target_link_libraries(asdl_dot_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.dot COMMAND asdl_dot_test)

  add_executable(asdl_lua_test tests/lua_native.cpp)
  target_include_directories(asdl_lua_test SYSTEM PRIVATE "${ASDL_THIRD_PARTY_DIR}/lua")
  target_link_libraries(asdl_lua_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.lua COMMAND asdl_lua_test)

  add_executable(asdl_abi_test tests/abi_consumer.c)
  target_compile_features(asdl_abi_test PRIVATE c_std_11)
  target_link_libraries(asdl_abi_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.abi COMMAND asdl_abi_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calculator.asdl"
    "${CMAKE_CURRENT_BINARY_DIR}/missing.asdl")

  add_executable(asdl_loader_test tests/asdl_loader_test.c)
  target_compile_features(asdl_loader_test PRIVATE c_std_11)
  target_include_directories(asdl_loader_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime")
  target_include_directories(asdl_loader_test SYSTEM PRIVATE "${ASDL_THIRD_PARTY_DIR}/lua")
  target_link_libraries(asdl_loader_test PRIVATE ASDL::runtime)
  add_test(NAME asdl.loader COMMAND asdl_loader_test
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/asdl_loader_test.lua"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/loader"
    "${CMAKE_CURRENT_SOURCE_DIR}/stdlib")

  if(ASDL_BUILD_TOOLS)
    if(PERL_FOUND)
      add_test(NAME asdl.compiler
        COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}" "CXX=${CMAKE_CXX_COMPILER}"
          "${PERL_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_cli.t"
          "$<TARGET_FILE:asdlc>")
    endif()
    add_test(NAME asdl.stdlib-native COMMAND asdl-run
      --stdlib "${CMAKE_CURRENT_SOURCE_DIR}/stdlib"
      "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/tests/native.asdl"
      "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/tests/native.lua")
    foreach(example IN ITEMS Mini MiniML MiniRuby)
      add_test(NAME "asdl.logistics-${example}"
        COMMAND "${CMAKE_COMMAND}" -E env "LIBSTONE_ASDL_PATH=${CMAKE_CURRENT_SOURCE_DIR}/examples"
          "$<TARGET_FILE:asdl-run>" "${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.asdl"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/logistics_examples.lua" "${example}")
    endforeach()

    set(bootstrap "${CMAKE_CURRENT_BINARY_DIR}/bootstrap.hpp")
    add_custom_command(OUTPUT "${bootstrap}"
      BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/bootstrap.h"
      COMMAND asdlc --header "${CMAKE_CURRENT_BINARY_DIR}/bootstrap.h" --cpp-header "${bootstrap}"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/Bootstrap.asdl"
      DEPENDS asdlc examples/Bootstrap.asdl
      VERBATIM)
    add_executable(asdl_bootstrap_test tests/bootstrap_consumer.cpp "${bootstrap}")
    target_compile_features(asdl_bootstrap_test PRIVATE cxx_std_20)
    target_include_directories(asdl_bootstrap_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME asdl.bootstrap COMMAND asdl_bootstrap_test)
  endif()

  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    set(asdl_package_test_tools)
    if(ASDL_BUILD_TOOLS)
      add_test(NAME asdl.run-cli
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_runtime.py"
          --tool "$<TARGET_FILE:asdl-run>" --work-dir "${CMAKE_CURRENT_BINARY_DIR}"
          --schema "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/calculator.asdl")
      set(asdl_package_test_tools --tools)
      if(TEST asdl.downstream-legacy)
        add_test(NAME asdl.downstream-native
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/downstream.py"
            --root "${CMAKE_CURRENT_SOURCE_DIR}/../.." --work-dir "${CMAKE_CURRENT_BINARY_DIR}"
            --perl "${PERL_EXECUTABLE}" --cc "${CMAKE_C_COMPILER}" --native "$<TARGET_FILE:asdlc>")
        set_tests_properties(asdl.downstream-native PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
      endif()
    endif()
    add_test(NAME asdl.install
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_package.py"
        --cmake "${CMAKE_COMMAND}" --build-dir "${CMAKE_CURRENT_BINARY_DIR}"
        --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}" --generator "${CMAKE_GENERATOR}"
        --bindir "${CMAKE_INSTALL_BINDIR}" --package-dir "${asdl_package_dir}"
        "--cflags=${CMAKE_C_FLAGS}" "--cxxflags=${CMAKE_CXX_FLAGS}" "--config=$<CONFIG>"
        --tests "${CMAKE_CURRENT_SOURCE_DIR}/tests" ${asdl_package_test_tools})
    set_tests_properties(asdl.install PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endif()
endif()
