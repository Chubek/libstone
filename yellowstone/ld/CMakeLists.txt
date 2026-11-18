# ld - qobjld linker (Unix-style static/shared/relocatable link + script).
#
# The core is a static library (qobjld) built from the per-area translation
# units; the CLI (qobjld) links it. ld.c exposes the C API and is compiled
# as C++ (LANGUAGE CXX) so it can use the header-only readers in ../qbfd/.
# ld.hpp is the C++ wrapper over that C API.

find_package(Perl REQUIRED)
find_package(Threads REQUIRED)
set(_qld_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_custom_command(
  OUTPUT "${_qld_generated}/ldscript_expression.c" "${_qld_generated}/ldscript_ast.h"
  COMMAND "${CMAKE_COMMAND}" "-DPERL=${PERL_EXECUTABLE}"
    "-DSCRIPTS=${CMAKE_CURRENT_SOURCE_DIR}/../../scripts"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUT=${_qld_generated}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/GenerateFrontend.cmake"
  DEPENDS ldscript.g ldscript.asdl GenerateFrontend.cmake
    ../../scripts/aurocks.pl ../../scripts/asdl.pl VERBATIM)
add_custom_target(qld-frontend DEPENDS "${_qld_generated}/ldscript_expression.c"
  "${_qld_generated}/ldscript_ast.h")

add_library(qld STATIC
  Expression.c
  Expression.cpp
  archive.cpp
  binarizer.cpp
  discover_symbols.cpp
  driver.cpp
  ElfBackend.cpp
  Backend.cpp
  BackendManifest.cpp
  PeBackend.cpp
  MachOBackend.cpp
  NativeOptions.cpp
  NativeImage.cpp
  DynamicElf.cpp
  ld.c
  ld.cpp
  ldscript.cpp
  ldscript_parser.cpp
  loader.cpp
  position_dependenc.cpp
  relocate.cpp
  shared.cpp
  static.cpp
  symbol.cpp
  ScriptSDK.cpp
)
add_library(qobjld::qobjld ALIAS qld)
set_target_properties(qld PROPERTIES EXPORT_NAME qobjld)
add_dependencies(qld qld-frontend)
target_include_directories(qld PRIVATE "${_qld_generated}")
target_compile_features(qld PRIVATE c_std_11)

set_source_files_properties(ld.c PROPERTIES LANGUAGE CXX)

target_compile_features(qld PUBLIC cxx_std_20)
option(YELLOWSTONE_ENABLE_ELF_BACKEND "Enable the explicit external ELF compatibility bridge" OFF)
set(YELLOWSTONE_ELF_LINKER "ld.bfd" CACHE STRING "Default GNU-compatible ELF linker executable (path or PATH name)")
if(YELLOWSTONE_ENABLE_ELF_BACKEND AND NOT UNIX)
  message(FATAL_ERROR "The ELF toolchain backend currently requires POSIX process APIs")
endif()
option(YELLOWSTONE_ENABLE_PE_BACKEND "Enable the explicit external PE compatibility bridge" OFF)
option(YELLOWSTONE_ENABLE_MACHO_BACKEND "Enable the explicit external Mach-O compatibility bridge" OFF)
set(YELLOWSTONE_PE_LINKER "lld-link" CACHE STRING "Default PE/COFF toolchain linker")
set(YELLOWSTONE_MACHO_LINKER "ld64.lld" CACHE STRING "Default Mach-O toolchain linker")
if(YELLOWSTONE_ENABLE_PE_BACKEND)
  target_compile_definitions(qld PRIVATE QLD_ENABLE_PE_BACKEND=1
    QLD_DEFAULT_PE_LINKER="${YELLOWSTONE_PE_LINKER}")
endif()
if(YELLOWSTONE_ENABLE_MACHO_BACKEND)
  target_compile_definitions(qld PRIVATE QLD_ENABLE_MACHO_BACKEND=1
    QLD_DEFAULT_MACHO_LINKER="${YELLOWSTONE_MACHO_LINKER}")
endif()
if(YELLOWSTONE_ENABLE_ELF_BACKEND)
  target_compile_definitions(qld PRIVATE QLD_ENABLE_ELF_BACKEND=1
    QLD_DEFAULT_ELF_LINKER="${YELLOWSTONE_ELF_LINKER}")
endif()
target_include_directories(qld PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/qobjld>
)
target_link_libraries(qld PUBLIC qbfd::qbfd)
target_link_libraries(qld PRIVATE Threads::Threads)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LibstoneLua.cmake")
target_link_libraries(qld PRIVATE
  $<BUILD_INTERFACE:libstone_kaguya> $<INSTALL_INTERFACE:LibstoneLua::libstone_script_lua>)
install(FILES ScriptSDK.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qobjld)

target_compile_options(qld PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QOBJFILE_WERROR)
  target_compile_options(qld PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(qld-frontend-test tests/frontend.cpp)
  target_link_libraries(qld-frontend-test PRIVATE qld Threads::Threads)
  add_test(NAME qld-frontend COMMAND qld-frontend-test)
  set_tests_properties(qld-frontend PROPERTIES TIMEOUT 30)
  if(YELLOWSTONE_BUILD_TOOLS)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(qld-native-api-test tests/native_api.cpp)
    target_link_libraries(qld-native-api-test PRIVATE qld)
    add_executable(qld-native-dynamic-api-test tests/native_dynamic_api.cpp)
    target_link_libraries(qld-native-dynamic-api-test PRIVATE qld)
    add_executable(qld-native-c-example examples/native-sdk.c)
    target_compile_features(qld-native-c-example PRIVATE c_std_11)
    target_compile_options(qld-native-c-example PRIVATE
      $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>)
    target_link_libraries(qld-native-c-example PRIVATE qld)
    add_executable(qld-native-dynamic-c-example examples/dynamic-sdk.c)
    target_compile_features(qld-native-dynamic-c-example PRIVATE c_std_11)
    target_compile_options(qld-native-dynamic-c-example PRIVATE
      $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>)
    target_link_libraries(qld-native-dynamic-c-example PRIVATE qld)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      add_test(NAME qld-native COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native.py" "$<TARGET_FILE:qobjld>"
        "${CMAKE_C_COMPILER}" "$<TARGET_FILE:qld-native-api-test>"
        "$<TARGET_FILE:qld-native-c-example>")
      set_tests_properties(qld-native PROPERTIES TIMEOUT 60
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
      add_test(NAME qld-native-dynamic COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_dynamic.py"
        "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}"
        "$<TARGET_FILE:qld-native-dynamic-api-test>"
        "$<TARGET_FILE:qld-native-dynamic-c-example>")
      set_tests_properties(qld-native-dynamic PROPERTIES TIMEOUT 60
        ENVIRONMENT "TMPDIR=${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_executable(qld-formats-api-test tests/formats_api.cpp)
    target_link_libraries(qld-formats-api-test PRIVATE qld)
    add_executable(qld-formats-c-example examples/formats-sdk.c)
    target_compile_features(qld-formats-c-example PRIVATE c_std_11)
    target_compile_options(qld-formats-c-example PRIVATE
      $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>)
    target_link_libraries(qld-formats-c-example PRIVATE qld)
    add_test(NAME qld-formats COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/formats.py" "$<TARGET_FILE:qobjld>"
      "$<TARGET_FILE:qld-formats-api-test>" "$<TARGET_FILE:qld-formats-c-example>"
      "${YELLOWSTONE_ENABLE_PE_BACKEND}" "${YELLOWSTONE_ENABLE_MACHO_BACKEND}")
    set_tests_properties(qld-formats PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
  endif()
  add_executable(qld-script-sdk-test tests/script_sdk.cpp)
  target_link_libraries(qld-script-sdk-test PRIVATE qld)
  add_test(NAME qld-script-sdk COMMAND qld-script-sdk-test)
  add_executable(qld-backend-manifest-test tests/backend_manifest.c)
  target_compile_features(qld-backend-manifest-test PRIVATE c_std_11)
  target_compile_options(qld-backend-manifest-test PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Werror;-pedantic>)
  target_link_libraries(qld-backend-manifest-test PRIVATE qld)
  add_test(NAME qld-backend-manifest COMMAND qld-backend-manifest-test)
  set_tests_properties(qld-backend-manifest PROPERTIES TIMEOUT 10)
  if(YELLOWSTONE_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME qld-script-executable COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts.py" "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}")
    if(YELLOWSTONE_ENABLE_ELF_BACKEND)
      add_test(NAME qld-backend-hardening COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/backend_hardening.py" "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}")
      add_executable(qld-elf-api-test tests/elf_api.cpp)
      target_link_libraries(qld-elf-api-test PRIVATE qld)
      add_test(NAME qld-dynamic COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/dynamic.py" "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}" "$<TARGET_FILE:qld-elf-api-test>" "$<TARGET_FILE:qld-native-dynamic-c-example>")
      set_tests_properties(qld-dynamic PROPERTIES TIMEOUT 120)
    endif()
  endif()
endif()

if(YELLOWSTONE_BUILD_TOOLS)
add_executable(qobjld main.cpp)
target_link_libraries(qobjld PRIVATE qobjld::qobjld qobjcommon)
target_include_directories(qobjld PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(qobjld PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QOBJFILE_WERROR)
  target_compile_options(qobjld PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

endif()
