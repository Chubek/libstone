# ld - qobjld linker (Unix-style static/shared/relocatable link + script).
#
# The core is a static library (qobjld) built from the per-area translation
# units; the CLI (qobjld) links it. ld.c exposes the C API and is compiled
# as C++ (LANGUAGE CXX) so it can use the header-only readers in ../qbfd/.
# ld.hpp is the C++ wrapper over that C API.

add_library(qld STATIC
  archive.cpp
  binarizer.cpp
  discover_symbols.cpp
  driver.cpp
  ElfBackend.cpp
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

set_source_files_properties(ld.c PROPERTIES LANGUAGE CXX)

target_compile_features(qld PUBLIC cxx_std_20)
option(YELLOWSTONE_ENABLE_ELF_BACKEND "Enable the GNU-compatible ELF linker backend" ${UNIX})
set(YELLOWSTONE_ELF_LINKER "ld.bfd" CACHE STRING "Default GNU-compatible ELF linker executable (path or PATH name)")
if(YELLOWSTONE_ENABLE_ELF_BACKEND AND NOT UNIX)
  message(FATAL_ERROR "The ELF toolchain backend currently requires POSIX process APIs")
endif()
if(YELLOWSTONE_ENABLE_ELF_BACKEND)
  target_compile_definitions(qld PRIVATE QLD_ENABLE_ELF_BACKEND=1
    QLD_DEFAULT_ELF_LINKER="${YELLOWSTONE_ELF_LINKER}")
endif()
target_include_directories(qld PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../qbfd
)
target_link_libraries(qld PUBLIC qbfd::qbfd)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LibstoneLua.cmake")
target_link_libraries(qld PRIVATE libstone_kaguya)
install(FILES ScriptSDK.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qobjld)

target_compile_options(qld PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QOBJFILE_WERROR)
  target_compile_options(qld PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(qld-script-sdk-test tests/script_sdk.cpp)
  target_link_libraries(qld-script-sdk-test PRIVATE qld)
  add_test(NAME qld-script-sdk COMMAND qld-script-sdk-test)
  if(YELLOWSTONE_BUILD_TOOLS AND CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME qld-script-executable COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts.py" "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}")
    if(YELLOWSTONE_ENABLE_ELF_BACKEND)
      add_executable(qld-elf-api-test tests/elf_api.cpp)
      target_link_libraries(qld-elf-api-test PRIVATE qld)
      add_executable(qld-elf-c-example examples/dynamic-sdk.c)
      target_compile_features(qld-elf-c-example PRIVATE c_std_11)
      target_compile_options(qld-elf-c-example PRIVATE -Wall -Wextra -Werror -pedantic)
      target_link_libraries(qld-elf-c-example PRIVATE qld)
      add_test(NAME qld-dynamic COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/dynamic.py" "$<TARGET_FILE:qobjld>" "${CMAKE_C_COMPILER}" "$<TARGET_FILE:qld-elf-api-test>" "$<TARGET_FILE:qld-elf-c-example>")
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
