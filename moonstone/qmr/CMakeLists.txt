cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(MoonstoneQMR VERSION 0.2.0 LANGUAGES C CXX)
  include(CTest)
  include(GNUInstallDirs)
endif()

set(_qmr_default_vendor "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party")
if(NOT DEFINED MOONSTONE_VENDOR_ROOT)
  set(MOONSTONE_VENDOR_ROOT "${_qmr_default_vendor}" CACHE PATH "LibStone third_party directory")
endif()
option(QMR_BUILD_TOOLS "Build qmr executable" ON)
option(QMR_BUILD_TESTS "Build QMR tests" ON)
option(QMR_SANITIZE "Build QMR targets and tests with AddressSanitizer/UndefinedBehaviorSanitizer" OFF)

add_library(moonstone_qmr_compiler STATIC src/Compiler.cpp)
add_library(Moonstone::QmrCompiler ALIAS moonstone_qmr_compiler)
set_target_properties(moonstone_qmr_compiler PROPERTIES EXPORT_NAME QmrCompiler)
target_compile_features(moonstone_qmr_compiler PUBLIC cxx_std_20)
target_include_directories(moonstone_qmr_compiler PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

set(_qmr_lua_root "${MOONSTONE_VENDOR_ROOT}/lua")
set(_qmr_kaguya_root "${MOONSTONE_VENDOR_ROOT}/kaguya/include")
if(NOT EXISTS "${_qmr_lua_root}/lua.h" OR
   NOT EXISTS "${_qmr_kaguya_root}/kaguya/kaguya.hpp")
  message(FATAL_ERROR "QMR requires LibStone's third_party/lua and third_party/kaguya. Set MOONSTONE_VENDOR_ROOT appropriately.")
endif()

set(_qmr_lua_target "")
if(TARGET moonstone_prolific_lua_vendor)
  set(_qmr_lua_target moonstone_prolific_lua_vendor)
else()
  # Build the actual vendored Lua C sources, not a system-Lua ABI lookalike.
  set(_qmr_lua_sources lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject
    lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib
    ldblib liolib lmathlib loslib ltablib lstrlib lutf8lib loadlib lcorolib linit)
  list(TRANSFORM _qmr_lua_sources PREPEND "${_qmr_lua_root}/")
  list(TRANSFORM _qmr_lua_sources APPEND ".c")
  add_library(moonstone_qmr_lua_vendor STATIC ${_qmr_lua_sources})
  set_target_properties(moonstone_qmr_lua_vendor PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED YES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(moonstone_qmr_lua_vendor SYSTEM PUBLIC "$<BUILD_INTERFACE:${_qmr_lua_root}>")
  if(UNIX)
    target_compile_definitions(moonstone_qmr_lua_vendor PRIVATE LUA_USE_POSIX)
    target_link_libraries(moonstone_qmr_lua_vendor PUBLIC m ${CMAKE_DL_LIBS})
  endif()
  set(_qmr_lua_target moonstone_qmr_lua_vendor)
endif()

add_library(moonstone_qmr STATIC src/Engine.cpp src/Abi.cpp)
add_library(Moonstone::Qmr ALIAS moonstone_qmr)
set_target_properties(moonstone_qmr PROPERTIES EXPORT_NAME Qmr)
target_compile_features(moonstone_qmr PUBLIC cxx_std_20)
target_link_libraries(moonstone_qmr PUBLIC moonstone_qmr_compiler PRIVATE ${_qmr_lua_target})
target_include_directories(moonstone_qmr SYSTEM PRIVATE "${_qmr_lua_root}" "${_qmr_kaguya_root}")
target_include_directories(moonstone_qmr PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

if(MSVC)
  target_compile_options(moonstone_qmr_compiler PRIVATE /W4)
  target_compile_options(moonstone_qmr PRIVATE /W4)
else()
  target_compile_options(moonstone_qmr_compiler PRIVATE -Wall -Wextra -Wpedantic)
  target_compile_options(moonstone_qmr PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QMR_SANITIZE)
  foreach(_qmr_target moonstone_qmr_compiler moonstone_qmr moonstone_qmr_lua_vendor)
    if(TARGET ${_qmr_target})
      target_compile_options(${_qmr_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${_qmr_target} PRIVATE -fsanitize=address,undefined)
    endif()
  endforeach()
endif()

if(QMR_BUILD_TOOLS AND (NOT DEFINED MOONSTONE_BUILD_TOOLS OR MOONSTONE_BUILD_TOOLS))
  # The CLI's JSON import uses the vendored klib kson parser (contract-checked).
  set(_qmr_klib_root "${MOONSTONE_VENDOR_ROOT}/klib")
  if(NOT EXISTS "${_qmr_klib_root}/kson.c")
    message(FATAL_ERROR "qmr CLI JSON import requires the vendored klib at third_party/klib.")
  endif()
  add_executable(qmr tools/qmr.cpp tools/json_import.cpp "${_qmr_klib_root}/kson.c")
  target_include_directories(qmr SYSTEM PRIVATE "${_qmr_klib_root}")
  target_link_libraries(qmr PRIVATE moonstone_qmr)
  # kson.c is vendored and must not be modified; silence its known GCC
  # -Wstringop-truncation false positive instead of editing the file.
  if(NOT MSVC AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    set_source_files_properties("${_qmr_klib_root}/kson.c" PROPERTIES
      COMPILE_OPTIONS "-Wno-stringop-truncation")
  endif()
  if(NOT MSVC)
    target_compile_options(qmr PRIVATE -Wall -Wextra)
  endif()
  if(QMR_SANITIZE)
    target_compile_options(qmr PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(qmr PRIVATE -fsanitize=address,undefined)
  endif()
  install(TARGETS qmr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(QMR_BUILD_TESTS AND (NOT DEFINED MOONSTONE_BUILD_TESTS OR MOONSTONE_BUILD_TESTS))
  enable_testing()
  add_executable(qmr_compiler_tests tests/compiler.cpp)
  target_link_libraries(qmr_compiler_tests PRIVATE moonstone_qmr_compiler)
  add_test(NAME moonstone.qmr_compiler COMMAND qmr_compiler_tests)
  add_executable(qmr_runtime_tests tests/runtime.cpp)
  target_link_libraries(qmr_runtime_tests PRIVATE moonstone_qmr)
  add_test(NAME moonstone.qmr_runtime COMMAND qmr_runtime_tests)
  add_executable(qmr_lexer_fuzz tests/lexer_fuzz.cpp)
  target_link_libraries(qmr_lexer_fuzz PRIVATE moonstone_qmr_compiler)
  add_test(NAME moonstone.qmr_lexer_fuzz COMMAND qmr_lexer_fuzz)
  set_tests_properties(moonstone.qmr_lexer_fuzz PROPERTIES TIMEOUT 300)
  # The ABI test is compiled as C to validate the header against a real C compiler.
  add_executable(qmr_abi_tests tests/abi.c)
  set_target_properties(qmr_abi_tests PROPERTIES LANGUAGE C)
  target_link_libraries(qmr_abi_tests PRIVATE moonstone_qmr)
  target_compile_features(qmr_abi_tests PRIVATE c_std_11)
  if(NOT MSVC)
    target_compile_options(qmr_abi_tests PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME moonstone.qmr_abi COMMAND qmr_abi_tests)
  if(QMR_SANITIZE)
    foreach(_qmr_test qmr_compiler_tests qmr_runtime_tests qmr_lexer_fuzz qmr_abi_tests)
      target_compile_options(${_qmr_test} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(${_qmr_test} PRIVATE -fsanitize=address,undefined)
    endforeach()
  endif()
  if(TARGET qmr)
    add_test(NAME moonstone.qmr_cli COMMAND qmr "${CMAKE_CURRENT_SOURCE_DIR}/examples/greeting.qmr"
      --define name=QMR)
    set_tests_properties(moonstone.qmr_cli PROPERTIES PASS_REGULAR_EXPRESSION "Hello, QMR")
    add_test(NAME moonstone.qmr_cli_data COMMAND qmr "${CMAKE_CURRENT_SOURCE_DIR}/examples/layout.qmr"
      --data "${CMAKE_CURRENT_SOURCE_DIR}/examples/data.json" --root "${CMAKE_CURRENT_SOURCE_DIR}/examples")
    set_tests_properties(moonstone.qmr_cli_data PROPERTIES
      PASS_REGULAR_EXPRESSION "<li>Ada Lovelace</li>")
    add_test(NAME moonstone.qmr_cli_json_codec COMMAND qmr "${CMAKE_CURRENT_SOURCE_DIR}/examples/greeting.qmr"
      --data "${CMAKE_CURRENT_SOURCE_DIR}/examples/data.json" --codec json)
    set_tests_properties(moonstone.qmr_cli_json_codec PROPERTIES
      PASS_REGULAR_EXPRESSION "Hello, \"Grace Hopper\"!")
  endif()
endif()

if(NOT TARGET moonstone_core)
  install(TARGETS moonstone_qmr moonstone_qmr_compiler moonstone_qmr_lua_vendor
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY include/qmr DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
