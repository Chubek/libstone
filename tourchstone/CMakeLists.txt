cmake_minimum_required(VERSION 3.20)
project(Tourchstone VERSION 0.1.0 LANGUAGES C CXX)

include(../cmake/LibstoneBuild.cmake)

if(BUILD_SHARED_LIBS AND UNIX AND NOT DEFINED CMAKE_INSTALL_RPATH)
  file(RELATIVE_PATH _tourchstone_lib_from_bin
    "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(APPLE)
    set(CMAKE_INSTALL_RPATH "@loader_path;@loader_path/${_tourchstone_lib_from_bin}")
  else()
    set(CMAKE_INSTALL_RPATH "$ORIGIN;$ORIGIN/${_tourchstone_lib_from_bin}")
  endif()
endif()

option(TOURCHSTONE_BUILD_XBIND "Build the XBind generator and tool" ON)
set(_tourchstone_native_default OFF)
if(TARGET qbfd::qbfd OR EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../yellowstone/qbfd/CMakeLists.txt")
  set(_tourchstone_native_default ON)
endif()
option(TOURCHSTONE_BUILD_DYNLOAD "Build the dynload loader/inspection library and tool" "${_tourchstone_native_default}")
option(TOURCHSTONE_BUILD_DYNINVOKE "Build the dyninvoke foreign-call library and tool" "${_tourchstone_native_default}")
if((TOURCHSTONE_BUILD_DYNLOAD OR TOURCHSTONE_BUILD_DYNINVOKE) AND NOT _tourchstone_native_default)
  message(FATAL_ERROR "Tourchstone native loader/invocation needs the active Yellowstone qBFD component. Disable TOURCHSTONE_BUILD_DYNLOAD and TOURCHSTONE_BUILD_DYNINVOKE for XBind-only builds.")
endif()

if(TOURCHSTONE_BUILD_DYNLOAD)
  add_subdirectory(dynload)
endif()

if(TOURCHSTONE_BUILD_DYNINVOKE)
  add_subdirectory(dyninvoke)
endif()
if(TOURCHSTONE_BUILD_XBIND)
  add_subdirectory(xbind)
endif()
if(TARGET Tourchstone::dynload OR TARGET Tourchstone::dyninvoke)
  install(EXPORT TourchstoneTargets NAMESPACE Tourchstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tourchstone)
  install(FILES cmake/TourchstoneConfig.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Tourchstone)
endif()

if(XBIND_BUILD_TESTS AND TARGET xbind_cli AND TARGET tdynload_fixture AND TARGET tdyninvoke)
  find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
  add_test(NAME tourchstone.bindings
    COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/xbind/tests/test_codegen.py
      --root ${CMAKE_CURRENT_SOURCE_DIR}/xbind --cli $<TARGET_FILE:xbind_cli>
      --cc ${CMAKE_C_COMPILER} --cxx ${CMAKE_CXX_COMPILER}
      --work ${CMAKE_CURRENT_BINARY_DIR}
      --native-lib $<TARGET_FILE:tdynload> --native-lib $<TARGET_FILE:tdyninvoke>
      --native-lib $<TARGET_FILE:xbind>
      "--native-flags=${CMAKE_CXX_FLAGS}"
      --fixture $<TARGET_FILE:tdynload_fixture>
      Codegen.test_tourchstone_lua_bindings_execute)
  set_tests_properties(tourchstone.bindings PROPERTIES TIMEOUT 180)
  if(XBIND_BUILD_DISCOVERY)
    add_library(interop_hello SHARED xbind/examples/hello/hello.c)
    set_target_properties(interop_hello PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    add_executable(tourchstone_interop_test xbind/tests/test_interop.cpp)
    target_link_libraries(tourchstone_interop_test PRIVATE XBind::QaMRppSpec
      Tourchstone::dynload Tourchstone::dyninvoke)
    libstone_warnings(tourchstone_interop_test)
    add_test(NAME tourchstone.interop COMMAND tourchstone_interop_test
      $<TARGET_FILE:interop_hello> "${CMAKE_CURRENT_SOURCE_DIR}/xbind/examples/hello/hello.xbind")
    set_tests_properties(tourchstone.interop PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()

if(LIBSTONE_BUILD_TESTS AND TARGET xbind_cli AND TARGET tdynload AND TARGET tdyninvoke
    AND TARGET ArchBFD::archbfd AND NOT CMAKE_CROSSCOMPILING
    AND NOT IS_ABSOLUTE "${CMAKE_INSTALL_BINDIR}" AND NOT IS_ABSOLUTE "${CMAKE_INSTALL_DATADIR}")
  add_test(NAME tourchstone.installed
    COMMAND ${CMAKE_COMMAND}
      "-DBUILD=${CMAKE_BINARY_DIR}"
      "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/xbind"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/installed-test/$<CONFIG>"
      "-DCONFIG=$<CONFIG>"
      "-DBINDIR=${CMAKE_INSTALL_BINDIR}" "-DDATADIR=${CMAKE_INSTALL_DATADIR}"
      "-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
      "-DCC=${CMAKE_C_COMPILER}" "-DCXX=${CMAKE_CXX_COMPILER}"
      "-DCFLAGS=${CMAKE_C_FLAGS}" "-DCXXFLAGS=${CMAKE_CXX_FLAGS}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/xbind/tests/test_installed.cmake")
  set_tests_properties(tourchstone.installed PROPERTIES TIMEOUT 180)
endif()
