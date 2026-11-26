cmake_minimum_required(VERSION 3.20)
project(XBind VERSION 0.1.0 LANGUAGES C CXX)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../cmake/LibstoneBuild.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/LibstoneBuild.cmake")
else()
  include(GNUInstallDirs)
  include(CTest)
  option(LIBSTONE_BUILD_TESTS "Build tests" "${BUILD_TESTING}")
  option(LIBSTONE_ENABLE_WARNINGS "Enable compiler warnings" ON)
  function(libstone_warnings target)
    if(LIBSTONE_ENABLE_WARNINGS)
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
    endif()
  endfunction()
endif()

option(XBIND_BUILD_TESTS "Build XBind contract and generated-binding tests" "${LIBSTONE_BUILD_TESTS}")
option(XBIND_BUILD_DOMBIND "Build the DomBind runtime subproject" OFF)
option(XBIND_BUILD_QAMRPP_BINDING "Build the QaMRpp Lua binding for XBind" ON)
set(_xbind_discovery_default OFF)
if(TARGET qbfd::qbfd)
  set(_xbind_discovery_default ON)
endif()
option(XBIND_BUILD_DISCOVERY "Build offline binary discovery using qBFD" "${_xbind_discovery_default}")
if(XBIND_BUILD_DISCOVERY AND NOT TARGET qbfd::qbfd)
  find_package(qBFD CONFIG REQUIRED)
endif()

include(cmake/XBindQaMRpp.cmake)

add_library(xbind xbind.c)
set_target_properties(xbind PROPERTIES POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
add_library(XBind::xbind ALIAS xbind)
target_compile_features(xbind PUBLIC c_std_11)
target_include_directories(xbind PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/xbind>)
target_compile_definitions(xbind PRIVATE XBIND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
libstone_warnings(xbind)

add_library(xbind_qamrpp STATIC xbind_qamrpp.cpp xbind_codegen.cpp xbind_discovery.cpp)
set_target_properties(xbind_qamrpp PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME QaMRppSpec)
add_library(XBind::QaMRppSpec ALIAS xbind_qamrpp)
target_compile_features(xbind_qamrpp PUBLIC cxx_std_20)
target_include_directories(xbind_qamrpp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/xbind>)
target_link_libraries(xbind_qamrpp PUBLIC XBind::xbind
  PRIVATE $<BUILD_INTERFACE:XBind::QaMRpp> ${CMAKE_DL_LIBS})
target_compile_definitions(xbind_qamrpp PRIVATE XBIND_HAVE_DISCOVERY=$<BOOL:${XBIND_BUILD_DISCOVERY}>)
if(XBIND_BUILD_DISCOVERY)
  # qBFD is header-only and is not part of the installed XBind API.
  target_link_libraries(xbind_qamrpp PRIVATE $<BUILD_INTERFACE:qbfd::qbfd>)
endif()
libstone_warnings(xbind_qamrpp)

add_executable(xbind_cli tools/xbind_tool.cpp)
set_target_properties(xbind_cli PROPERTIES OUTPUT_NAME xbind)
target_compile_features(xbind_cli PRIVATE cxx_std_20)
target_include_directories(xbind_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
file(RELATIVE_PATH _xbind_relative_data "${CMAKE_INSTALL_FULL_BINDIR}"
  "${CMAKE_INSTALL_FULL_DATADIR}/xbind")
target_compile_definitions(xbind_cli PRIVATE
  XBIND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  XBIND_INSTALL_DATA_DIR="${CMAKE_INSTALL_FULL_DATADIR}/xbind"
  XBIND_RELATIVE_DATA_DIR="${_xbind_relative_data}")
target_link_libraries(xbind_cli PRIVATE XBind::xbind XBind::QaMRppSpec)
if(UNIX AND BUILD_SHARED_LIBS AND NOT DEFINED CMAKE_INSTALL_RPATH)
  file(RELATIVE_PATH _xbind_relative_lib "${CMAKE_INSTALL_FULL_BINDIR}"
    "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(APPLE)
    set_property(TARGET xbind_cli PROPERTY INSTALL_RPATH "@loader_path/${_xbind_relative_lib}")
  else()
    set_property(TARGET xbind_cli PROPERTY INSTALL_RPATH "$ORIGIN/${_xbind_relative_lib}")
  endif()
endif()
libstone_warnings(xbind_cli)

if(XBIND_BUILD_QAMRPP_BINDING)
  add_library(qamrpp_xbind MODULE xbind_qamrpp.cpp lxbind/qamrpp_xbind.cpp)
  # QaMRpp.hpp defines its C plugin API (non-inline). Keep its users in one
  # translation unit while sharing the exact constructor/validation source.
  set_target_properties(qamrpp_xbind PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 2)
  target_compile_features(qamrpp_xbind PRIVATE cxx_std_20)
  target_include_directories(qamrpp_xbind PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(qamrpp_xbind PRIVATE XBind::xbind XBind::QaMRpp)
  set_target_properties(qamrpp_xbind PROPERTIES
    OUTPUT_NAME qamrpp_xbind
    PREFIX "lib")
  if(UNIX)
    # Modules are installed one level below the native libraries.
    if(APPLE)
      set_property(TARGET qamrpp_xbind APPEND PROPERTY INSTALL_RPATH "@loader_path/..")
    else()
      set_property(TARGET qamrpp_xbind APPEND PROPERTY INSTALL_RPATH "$ORIGIN/..")
    endif()
  endif()
  target_compile_definitions(qamrpp_xbind PRIVATE XBIND_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  libstone_warnings(qamrpp_xbind)
endif()

if(XBIND_BUILD_DOMBIND)
  add_subdirectory(dombind)
endif()

install(TARGETS xbind xbind_qamrpp EXPORT XBindTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(EXPORT XBindTargets FILE XBindConfig.cmake NAMESPACE XBind::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/XBind)
install(TARGETS xbind_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET qamrpp_xbind)
  install(TARGETS qamrpp_xbind LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/qamrpp)
endif()
install(FILES xbind.h xbind.hpp xbind_qamrpp.hpp xbind_codegen.hpp xbind_discovery.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xbind)
install(FILES INTEROPERABILITY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/xbind)
install(DIRECTORY lxbind/ DESTINATION ${CMAKE_INSTALL_DATADIR}/xbind/lxbind)
install(DIRECTORY stdlib/ DESTINATION ${CMAKE_INSTALL_DATADIR}/xbind/stdlib)
install(DIRECTORY typedefs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/xbind/typedefs)
install(FILES XFeats.schema.json DESTINATION ${CMAKE_INSTALL_DATADIR}/xbind)

if(XBIND_BUILD_TESTS)
  enable_testing()
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  add_executable(xbind_core_tests tests/test_core.c)
  target_link_libraries(xbind_core_tests PRIVATE XBind::xbind)
  libstone_warnings(xbind_core_tests)
  add_test(NAME xbind.core
    COMMAND xbind_core_tests ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello)
  add_test(NAME xbind.layout
    COMMAND xbind_cli --check-layout ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME xbind.regimen
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/regimen.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}
      --cli $<TARGET_FILE:xbind_cli>)
  add_test(NAME xbind.spec
    COMMAND xbind_cli --check-spec hello ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello)
  add_test(NAME xbind.codegen
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_codegen.py
      --root ${CMAKE_CURRENT_SOURCE_DIR} --cli $<TARGET_FILE:xbind_cli>
      --cc ${CMAKE_C_COMPILER} --cxx ${CMAKE_CXX_COMPILER}
      --work ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(xbind.codegen PROPERTIES TIMEOUT 180)
  add_test(NAME xbind.discovery
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_discovery.py
      --cli $<TARGET_FILE:xbind_cli> --cxx ${CMAKE_CXX_COMPILER}
      --work ${CMAKE_CURRENT_BINARY_DIR} --enabled $<BOOL:${XBIND_BUILD_DISCOVERY}>)
  set_tests_properties(xbind.discovery PROPERTIES TIMEOUT 180)
  if(TARGET qamrpp_xbind)
    add_executable(xbind_qamrpp_module_test tests/qamrpp_module.cpp)
    target_link_libraries(xbind_qamrpp_module_test PRIVATE XBind::QaMRpp)
    target_compile_features(xbind_qamrpp_module_test PRIVATE cxx_std_20)
    libstone_warnings(xbind_qamrpp_module_test)
    add_test(NAME xbind.qamrpp-module
      COMMAND xbind_qamrpp_module_test
        $<TARGET_FILE:qamrpp_xbind>
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/hello/hello.xbind)
  endif()
  set_tests_properties(xbind.layout xbind.regimen xbind.spec PROPERTIES TIMEOUT 30)
  if(TEST xbind.qamrpp-module)
    set_tests_properties(xbind.qamrpp-module PROPERTIES TIMEOUT 30)
  endif()
endif()
