cmake_minimum_required(VERSION 3.20)
project(GateML VERSION 0.1.0 LANGUAGES CXX)
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/LibstoneBuild.cmake")
option(GATEML_BUILD_TESTS "Build GateML tests" ${LIBSTONE_BUILD_TESTS})
option(GATEML_BUILD_TOOLS "Build the GateML CLI" ${LIBSTONE_BUILD_TOOLS})
libstone_require_files(LIBSTONE_METATK_SOURCE_DIR MetaChain/MetaChain.hpp DSLtk/DSLtk.hpp)
set(GATEML_STDLIB_SOURCE "")
foreach(module IN ITEMS Prelude Int Bool Pair Option Result String List IO)
  set(file "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/${module}.gml")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${file}")
  file(READ "${file}" source)
  list(APPEND GATEML_STDLIB_FILES "${file}")
  string(APPEND GATEML_STDLIB_SOURCE "\n${source}\n")
endforeach()
configure_file(stdlib/Source.hpp.in generated/StdlibSource.hpp @ONLY)
add_library(gateml STATIC AST.cpp Preprocessor.cpp Types.cpp Sema.cpp Value.cpp
  Compiler.cpp Optimizer.cpp IR.cpp Memory.cpp FFI.cpp Loader.cpp IO.cpp Prelude.cpp
  stdlib/stdlib.cpp Assembler.cpp CLI.cpp)
add_library(Gatestone::GateML ALIAS gateml)
set_target_properties(gateml PROPERTIES EXPORT_NAME GateML)
target_compile_features(gateml PUBLIC cxx_std_20)
target_include_directories(gateml PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/gatestone/gateml>")
target_include_directories(gateml SYSTEM PRIVATE
  "$<BUILD_INTERFACE:${LIBSTONE_METATK_SOURCE_DIR}/MetaChain>"
  "$<BUILD_INTERFACE:${LIBSTONE_METATK_SOURCE_DIR}>")
target_include_directories(gateml PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
libstone_warnings(gateml)
if(GATEML_BUILD_TOOLS OR GATEML_BUILD_TESTS)
  add_executable(gateml-cli Main.cpp)
  target_link_libraries(gateml-cli PRIVATE gateml)
endif()
if(GATEML_BUILD_TESTS)
  enable_testing()
  add_executable(gateml-tests tests/Tests.cpp)
  target_link_libraries(gateml-tests PRIVATE gateml)
  add_test(NAME gateml-language COMMAND gateml-tests)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME gateml-cli COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/CLI.py" $<TARGET_FILE:gateml-cli>)
endif()
install(TARGETS gateml EXPORT GateMLTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT GateMLTargets NAMESPACE Gatestone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GateML)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/GateMLConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/GateMLConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/GateML")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/GateMLConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/GateMLConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/GateMLConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/GateML)
if(GATEML_BUILD_TOOLS)
  install(TARGETS gateml-cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES GateML.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/gatestone/gateml)
install(FILES ${GATEML_STDLIB_FILES} stdlib/stdlib.gml stdlib/README.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/gatestone/gateml/stdlib)
