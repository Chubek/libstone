cmake_minimum_required(VERSION 3.20)
project(Amalgam8 VERSION 0.1.0 LANGUAGES C CXX)

include(../../cmake/LibstoneBuild.cmake)
include(CMakePackageConfigHelpers)
option(AMALGAM8_BUILD_TESTS "Build Amalgam8 tests" "${LIBSTONE_BUILD_TESTS}")
option(AMALGAM8_BUILD_TOOLS "Build the Amalgam8 command-line compiler" "${LIBSTONE_BUILD_TOOLS}")

add_library(amalgam8_compiler INTERFACE)
add_library(Amalgam8::compiler ALIAS amalgam8_compiler)
set_target_properties(amalgam8_compiler PROPERTIES EXPORT_NAME compiler)
target_compile_features(amalgam8_compiler INTERFACE cxx_std_20)
target_include_directories(amalgam8_compiler INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../firestone/libdsl>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/amalgam8>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/firestone/libdsl>)

if(AMALGAM8_BUILD_TOOLS)
  if(NOT UNIX)
    message(FATAL_ERROR "The amalgam8 executable requires POSIX atomic publication; set AMALGAM8_BUILD_TOOLS=OFF to build the embeddable compiler only")
  endif()
  add_executable(amalgam8 Amalgam8Base.cpp)
  target_link_libraries(amalgam8 PRIVATE Amalgam8::compiler)
  set_target_properties(amalgam8 PROPERTIES CXX_EXTENSIONS OFF)
  libstone_warnings(amalgam8)
  install(TARGETS amalgam8 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(TARGETS amalgam8_compiler EXPORT Amalgam8Targets)
install(FILES
  Amalgam8Base.hpp Catalog.hpp Contracts.hpp Emitter.hpp Expansion.hpp Frontend.hpp
  IR.hpp Native.hpp NativeLibraries.hpp Packaging.hpp Semantic.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/amalgam8)
install(DIRECTORY catalog/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/amalgam8/catalog
  FILES_MATCHING PATTERN "*.inc")
install(DIRECTORY catalog/ DESTINATION ${CMAKE_INSTALL_DATADIR}/amalgam8/catalog
  FILES_MATCHING PATTERN "*.tsv")
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DATADIR}/amalgam8)
install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/amalgam8/docs
  FILES_MATCHING PATTERN "*.md")
install(FILES ../../firestone/libdsl/LibDSL.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/firestone/libdsl)
install(EXPORT Amalgam8Targets NAMESPACE Amalgam8:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Amalgam8)
configure_package_config_file(Amalgam8Config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/Amalgam8Config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Amalgam8)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/Amalgam8ConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/Amalgam8Config.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/Amalgam8ConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Amalgam8)

if(AMALGAM8_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
