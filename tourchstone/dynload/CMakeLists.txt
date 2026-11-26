cmake_minimum_required(VERSION 3.20)
project(TourchstoneDynload VERSION 0.1.0 LANGUAGES C CXX)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../cmake/LibstoneBuild.cmake")
  include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/LibstoneBuild.cmake")
else()
  include(GNUInstallDirs)
  include(CTest)
  option(LIBSTONE_BUILD_TESTS "Build tests" "${BUILD_TESTING}")
  option(LIBSTONE_BUILD_TOOLS "Build tools" ON)
  option(LIBSTONE_ENABLE_WARNINGS "Enable compiler warnings" ON)
  function(libstone_warnings target)
    if(LIBSTONE_ENABLE_WARNINGS)
      target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
    endif()
  endfunction()
endif()

option(DYNLOAD_BUILD_TOOLS "Build the tdynload command-line tool" "${LIBSTONE_BUILD_TOOLS}")
option(DYNLOAD_BUILD_TESTS "Build dynload tests" "${LIBSTONE_BUILD_TESTS}")

if(NOT TARGET qbfd::qbfd)
  if(NOT DEFINED QBFD_BUILD_TESTS)
    set(QBFD_BUILD_TESTS "${DYNLOAD_BUILD_TESTS}" CACHE BOOL
      "Build qBFD regression tests")
  endif()
  add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../yellowstone/qbfd"
                   "${CMAKE_CURRENT_BINARY_DIR}/yellowstone-qbfd")
endif()

add_library(tdynload dynload.cpp)
add_library(Tourchstone::dynload ALIAS tdynload)
target_compile_features(tdynload PUBLIC cxx_std_20)
target_include_directories(tdynload PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(tdynload PRIVATE qbfd::qbfd)
target_compile_definitions(tdynload PUBLIC
  $<$<BOOL:${BUILD_SHARED_LIBS}>:TDYNLOAD_SHARED>)
if(CMAKE_DL_LIBS)
  target_link_libraries(tdynload PRIVATE ${CMAKE_DL_LIBS})
endif()
if(NOT WIN32)
  include(CheckCXXSymbolExists)
  include(CheckCXXSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_DL_LIBS})
  check_cxx_symbol_exists(dlvsym "dlfcn.h" TDYN_HAVE_DLVSYM)
  check_cxx_source_compiles(
    "#include <dlfcn.h>
     #include <link.h>
     int main() {
       struct link_map* map = nullptr;
       return dlinfo(nullptr, RTLD_DI_LINKMAP, &map);
     }"
    TDYN_HAVE_LINKMAP)
  if(TDYN_HAVE_LINKMAP)
    check_cxx_source_compiles(
      "#include <dlfcn.h>
       #include <link.h>
       int main() {
         const ElfW(Phdr)* headers = nullptr;
         return dlinfo(nullptr, RTLD_DI_PHDR, &headers);
       }"
      TDYN_HAVE_DL_PHDR)
  endif()
  cmake_pop_check_state()
  if(TDYN_HAVE_DLVSYM)
    target_compile_definitions(tdynload PRIVATE TDYN_HAVE_DLVSYM)
  endif()
  if(TDYN_HAVE_LINKMAP)
    target_compile_definitions(tdynload PRIVATE TDYN_HAVE_LINKMAP)
  endif()
  if(TDYN_HAVE_DL_PHDR)
    target_compile_definitions(tdynload PRIVATE TDYN_HAVE_DL_PHDR)
  endif()
endif()
set_target_properties(tdynload PROPERTIES POSITION_INDEPENDENT_CODE ON EXPORT_NAME dynload)
libstone_warnings(tdynload)

if(DYNLOAD_BUILD_TOOLS)
  add_executable(tdynload_cli tools/tdynload.cpp)
  set_target_properties(tdynload_cli PROPERTIES OUTPUT_NAME tdynload)
  target_compile_features(tdynload_cli PRIVATE cxx_std_20)
  target_include_directories(tdynload_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/../../yellowstone/common)
  target_link_libraries(tdynload_cli PRIVATE Tourchstone::dynload)
  libstone_warnings(tdynload_cli)
endif()

install(TARGETS tdynload EXPORT TourchstoneTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(TARGET tdynload_cli)
  install(TARGETS tdynload_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES dynload.h dynload.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dynload)

if(DYNLOAD_BUILD_TESTS)
  enable_testing()
  add_library(tdynload_fixture SHARED tests/fixture.c)
  target_compile_definitions(tdynload_fixture PRIVATE TDYNLOAD_FIXTURE_BUILD)
  set_target_properties(tdynload_fixture PROPERTIES OUTPUT_NAME tdynload_fixture)
  if(WIN32)
    target_sources(tdynload_fixture PRIVATE tests/fixture.def)
  endif()

  add_executable(tdynload_tests tests/test_dynload.cpp)
  target_compile_features(tdynload_tests PRIVATE cxx_std_20)
  target_link_libraries(tdynload_tests PRIVATE Tourchstone::dynload)
  libstone_warnings(tdynload_tests)

  if(TDYN_HAVE_DLVSYM AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(tdynload_fixture PRIVATE TDYNLOAD_TEST_ELF_VERSIONS)
    target_compile_definitions(tdynload_tests PRIVATE TDYNLOAD_TEST_ELF_VERSIONS)
    set(_versions "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.versions")
    target_link_options(tdynload_fixture PRIVATE "LINKER:--version-script,${_versions}")
    set_property(TARGET tdynload_fixture APPEND PROPERTY LINK_DEPENDS "${_versions}")
    find_package(Threads REQUIRED)
    target_link_libraries(tdynload_tests PRIVATE Threads::Threads)
  endif()
  if(TDYN_HAVE_DL_PHDR)
    target_compile_definitions(tdynload_tests PRIVATE TDYNLOAD_TEST_DL_PHDR)
  endif()

  add_test(NAME dynload.open COMMAND tdynload_tests native $<TARGET_FILE:tdynload_fixture>)
  set_tests_properties(dynload.open PROPERTIES LABELS "native")
  add_test(NAME dynload.offline COMMAND tdynload_tests offline $<TARGET_FILE:tdynload_fixture>)
  set_tests_properties(dynload.offline PROPERTIES LABELS "offline")
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # No RUNPATH, and the test does not put the dependency on the native
    # search path. -L (tdyn_env_add_search_path) only resolves the root.
    add_library(tdynload_missing_fixture_dependency SHARED tests/dependency.c)
    add_library(tdynload_dependent_fixture SHARED tests/dependent.c)
    target_link_libraries(tdynload_dependent_fixture PRIVATE tdynload_missing_fixture_dependency)
    set_target_properties(tdynload_dependent_fixture PROPERTIES SKIP_BUILD_RPATH TRUE)
    add_test(NAME dynload.dependency-diagnostic
      COMMAND tdynload_tests missing-dependency $<TARGET_FILE:tdynload_dependent_fixture>)
    set_tests_properties(dynload.dependency-diagnostic PROPERTIES
      LABELS "native" ENVIRONMENT "LD_LIBRARY_PATH=;LD_PRELOAD=")
  endif()
  if(TARGET tdynload_cli)
    add_test(NAME dynload.cli-help COMMAND tdynload_cli --help)
    add_test(NAME dynload.cli-inspect
      COMMAND tdynload_cli --inspect-only --inspect --needed $<TARGET_FILE:tdynload_fixture>)
    set_tests_properties(dynload.cli-inspect PROPERTIES LABELS "offline"
      PASS_REGULAR_EXPRESSION "native-loaded: no")
    add_test(NAME dynload.cli-manifest COMMAND tdynload_cli --manifest $<TARGET_FILE:tdynload_fixture>)
    set_tests_properties(dynload.cli-manifest PROPERTIES LABELS "offline"
      PASS_REGULAR_EXPRESSION "tdynload-inspection-v1")
    add_test(NAME dynload.cli-resolve
      COMMAND tdynload_cli --resolve tdynload_fixture_add $<TARGET_FILE:tdynload_fixture>)
    set_tests_properties(dynload.cli-resolve PROPERTIES LABELS "native"
      PASS_REGULAR_EXPRESSION "tdynload_fixture_add = 0x")
  endif()
endif()
