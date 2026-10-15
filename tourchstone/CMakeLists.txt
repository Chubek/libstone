cmake_minimum_required(VERSION 3.20)
project(Tourchstone VERSION 0.1.0 LANGUAGES C CXX)

include(../cmake/LibstoneBuild.cmake)

option(TOURCHSTONE_BUILD_XBIND "Build the XBind generator and tool" ON)
option(TOURCHSTONE_BUILD_DYNLOAD "Build the dynload loader/inspection library and tool" ON)
option(TOURCHSTONE_BUILD_DYNINVOKE "Build the dyninvoke foreign-call library and tool" ON)

if(TOURCHSTONE_BUILD_XBIND)
  add_subdirectory(xbind)
endif()

if(TOURCHSTONE_BUILD_DYNLOAD)
  add_subdirectory(dynload)
endif()

if(TOURCHSTONE_BUILD_DYNINVOKE)
  add_subdirectory(dyninvoke)
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
endif()
