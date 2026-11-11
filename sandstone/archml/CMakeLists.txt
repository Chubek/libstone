cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ArchML VERSION 1.0.0 LANGUAGES C CXX)
  include(../../cmake/LibstoneBuild.cmake)
  option(SANDSTONE_BUILD_TOOLS "Build ArchML CLI" ON)
  option(SANDSTONE_BUILD_TESTS "Build ArchML tests" "${BUILD_TESTING}")
endif()
find_package(Perl REQUIRED)
set(ARCHML_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${ARCHML_GENERATED}")
get_filename_component(ARCHML_SCRIPTS "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts" ABSOLUTE)
add_custom_command(
  OUTPUT "${ARCHML_GENERATED}/ArchMLAST.h"
  COMMAND "${PERL_EXECUTABLE}" "${ARCHML_SCRIPTS}/asdl.pl"
          --header "${ARCHML_GENERATED}/ArchMLAST.h" "${CMAKE_CURRENT_SOURCE_DIR}/ArchML.asdl"
  DEPENDS ArchML.asdl "${ARCHML_SCRIPTS}/asdl.pl"
  VERBATIM)
add_custom_command(
  OUTPUT "${ARCHML_GENERATED}/ArchMLParser.c"
  COMMAND "${CMAKE_COMMAND}" "-DPERL=${PERL_EXECUTABLE}"
          "-DGENERATOR=${ARCHML_SCRIPTS}/aurocks.pl"
          "-DGRAMMAR=${CMAKE_CURRENT_SOURCE_DIR}/ArchML.g"
          "-DOUTPUT=${ARCHML_GENERATED}/ArchMLParser.c"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateParser.cmake"
  DEPENDS ArchML.g Parser.h GenerateParser.cmake "${ARCHML_SCRIPTS}/aurocks.pl"
  VERBATIM)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Runtime.hpp" ARCHML_RUNTIME_SOURCE)
configure_file(targets/RuntimeSource.hpp.in "${ARCHML_GENERATED}/RuntimeSource.hpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS Runtime.hpp)
add_library(sandstone-archml STATIC
  ArchML.cpp CAPI.cpp targets/Target.cpp targets/Cpp.cpp targets/Data.cpp
  "${ARCHML_GENERATED}/ArchMLParser.c" "${ARCHML_GENERATED}/ArchMLAST.h")
add_library(Sandstone::ArchML ALIAS sandstone-archml)
target_compile_features(sandstone-archml PUBLIC cxx_std_20 PRIVATE c_std_11)
set_target_properties(sandstone-archml PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(sandstone-archml
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
         "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sandstone/archml>"
  PRIVATE "${ARCHML_GENERATED}")
find_package(Threads REQUIRED)
target_link_libraries(sandstone-archml PUBLIC Threads::Threads)
libstone_warnings(sandstone-archml)
if(SANDSTONE_BUILD_TOOLS)
  add_executable(archml CLI.cpp)
  target_link_libraries(archml PRIVATE sandstone-archml)
  libstone_warnings(archml)
  install(TARGETS archml RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()
install(TARGETS sandstone-archml ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES ArchML.h ArchML.hpp Target.hpp Runtime.hpp
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sandstone/archml")
install(DIRECTORY stdspec/ DESTINATION "${CMAKE_INSTALL_DATADIR}/sandstone/archml/stdspec")
install(FILES README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/sandstone/archml")
if(SANDSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(archml-test tests/ArchMLTest.cpp tests/CAPI.c)
  target_link_libraries(archml-test PRIVATE sandstone-archml)
  target_compile_definitions(archml-test PRIVATE ARCHML_STDSPEC="${CMAKE_CURRENT_SOURCE_DIR}/stdspec")
  add_test(NAME archml-profiles COMMAND archml-test)
  if(SANDSTONE_BUILD_TOOLS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME archml-codegen COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/codegen.py" "$<TARGET_FILE:archml>"
      "${CMAKE_CXX_COMPILER}" "${CMAKE_CURRENT_SOURCE_DIR}/stdspec"
      "${CMAKE_CURRENT_BINARY_DIR}/codegen-test")
  endif()
endif()
