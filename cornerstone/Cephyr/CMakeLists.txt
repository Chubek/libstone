cmake_minimum_required(VERSION 3.20)
project(Cephyr VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
option(CEPHYR_BUILD_TESTS "Build and register Cephyr tests" ON)
if(CEPHYR_BUILD_TESTS)
  enable_testing()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The Cephyr compiler driver currently requires a Linux host")
endif()

get_filename_component(cephyr_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(cephyr_vendor "${cephyr_repo}/third_party")
set(cephyr_archon "${cephyr_repo}/cornerstone/Archon")
include("${cephyr_repo}/cmake/LibstoneLua.cmake")
if(NOT TARGET Archon::Archon)
  add_subdirectory("${cephyr_archon}" archon)
endif()
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
find_path(CEPHYR_LMDB_INCLUDE_DIR NAMES lmdb.h REQUIRED)
find_library(CEPHYR_LMDB_LIBRARY NAMES lmdb REQUIRED)

add_library(cephyr_cparser STATIC)
foreach(source IN ITEMS AbstractSyntaxTree ASTNode CLexer CParser GenCVisitor Lexer
                        Parser PrintTreeVisitor SymbolTable TreeVisitor)
  target_sources(cephyr_cparser PRIVATE "${cephyr_vendor}/cparser-iso11/src/${source}.cpp")
endforeach()
target_compile_features(cephyr_cparser PRIVATE cxx_std_14)
target_include_directories(cephyr_cparser SYSTEM PUBLIC "${cephyr_vendor}/cparser-iso11/include")

add_library(cephyr_ucpp STATIC)
foreach(source IN ITEMS mem nhash cpp lexer assert macro eval)
  target_sources(cephyr_ucpp PRIVATE "${cephyr_vendor}/ucpp/${source}.c")
endforeach()
set_target_properties(cephyr_ucpp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES)
target_compile_definitions(cephyr_ucpp PUBLIC UCPP_CONFIG NO_UCPP_ERROR_FUNCTIONS)
target_compile_definitions(cephyr_ucpp PRIVATE UCPP_STDC_VERSION="201112L")
target_include_directories(cephyr_ucpp SYSTEM PUBLIC "${cephyr_vendor}/ucpp")

add_library(cephyr_tomlc99 STATIC "${cephyr_vendor}/tomlc99/toml.c")
set_target_properties(cephyr_tomlc99 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES)
target_include_directories(cephyr_tomlc99 SYSTEM PUBLIC "${cephyr_vendor}/tomlc99")

add_library(cephyr_diagnostics STATIC src/diagnostics.cpp)
target_compile_features(cephyr_diagnostics PUBLIC cxx_std_20)
target_include_directories(cephyr_diagnostics PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(TARGET Moonstone::Syren)
  target_link_libraries(cephyr_diagnostics PRIVATE Moonstone::Syren)
else()
  target_include_directories(cephyr_diagnostics SYSTEM PRIVATE
    "${cephyr_repo}/moonstone" "${cephyr_repo}/moonstone/syren/include")
endif()

add_executable(cephyr
  src/main.cpp src/process.cpp src/config.cpp src/target.cpp src/host_target.cpp src/cache.cpp
  src/preprocess.cpp src/parser.cpp src/semantics.cpp src/lower.cpp
  src/lower_globals.cpp src/lower_expr.cpp src/lower_stmt.cpp src/backend.cpp)
target_compile_features(cephyr PRIVATE cxx_std_20)
set_target_properties(cephyr PROPERTIES CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cephyr PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra>)
endif()
target_include_directories(cephyr PRIVATE "${cephyr_archon}/archon/irgen/native")
target_include_directories(cephyr SYSTEM PRIVATE
  "${cephyr_repo}" "${cephyr_vendor}/lmdbxx" "${CEPHYR_LMDB_INCLUDE_DIR}")
target_link_libraries(cephyr PRIVATE cephyr_diagnostics cephyr_cparser cephyr_ucpp cephyr_tomlc99
  libstone_kaguya Archon::Archon "${CEPHYR_LMDB_LIBRARY}" ${CMAKE_DL_LIBS})
if(TARGET Wetstone::wetstone)
  target_link_libraries(cephyr PRIVATE Wetstone::wetstone)
endif()

set(cephyr_data_dir "${CMAKE_INSTALL_DATADIR}/cephyr")
file(RELATIVE_PATH cephyr_installed_resources
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/cephyr")
file(RELATIVE_PATH cephyr_installed_archon
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/archon/cornerstone/Archon")
target_compile_definitions(cephyr PRIVATE
  CEPHYR_VERSION="${PROJECT_VERSION}"
  CEPHYR_SOURCE_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}"
  CEPHYR_SOURCE_ARCHON="${cephyr_archon}"
  CEPHYR_INSTALLED_RESOURCES="${cephyr_installed_resources}"
  CEPHYR_INSTALLED_ARCHON="${cephyr_installed_archon}")
install(TARGETS cephyr RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY stdlib triples DESTINATION "${cephyr_data_dir}" USE_SOURCE_PERMISSIONS)
install(FILES Cephyr.toml README.md DESTINATION "${cephyr_data_dir}")

if(CEPHYR_BUILD_TESTS)
  add_executable(cephyr-diagnostics-test tests/diagnostics.cpp)
  target_link_libraries(cephyr-diagnostics-test PRIVATE cephyr_diagnostics)
  add_test(NAME cephyr.diagnostics COMMAND cephyr-diagnostics-test)
  add_executable(cephyr-preprocess-test tests/preprocess.cpp src/preprocess.cpp)
  target_link_libraries(cephyr-preprocess-test PRIVATE cephyr_diagnostics cephyr_ucpp)
  add_test(NAME cephyr.preprocess COMMAND cephyr-preprocess-test)
  add_executable(cephyr-parser-test tests/parser.cpp src/parser.cpp src/preprocess.cpp)
  target_link_libraries(cephyr-parser-test PRIVATE cephyr_diagnostics cephyr_ucpp cephyr_cparser)
  add_test(NAME cephyr.parser COMMAND cephyr-parser-test)
  add_executable(cephyr-cache-test tests/cache.cpp src/cache.cpp)
  target_include_directories(cephyr-cache-test SYSTEM PRIVATE
    "${cephyr_vendor}/lmdbxx" "${CEPHYR_LMDB_INCLUDE_DIR}")
  target_link_libraries(cephyr-cache-test PRIVATE cephyr_diagnostics "${CEPHYR_LMDB_LIBRARY}")
  add_test(NAME cephyr.cache COMMAND cephyr-cache-test)
  add_executable(cephyr-host-target-test
    tests/host_target.cpp src/host_target.cpp src/target.cpp src/process.cpp src/config.cpp)
  target_link_libraries(cephyr-host-target-test PRIVATE
    cephyr_diagnostics libstone_kaguya cephyr_tomlc99)
  add_test(NAME cephyr.host-target COMMAND cephyr-host-target-test
    "${CMAKE_CURRENT_SOURCE_DIR}" "${cephyr_archon}")
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    foreach(test IN ITEMS cephyr-diagnostics-test cephyr-preprocess-test cephyr-cache-test cephyr-host-target-test)
      target_compile_options(${test} PRIVATE -UNDEBUG)
    endforeach()
  endif()
  find_program(CEPHYR_QEMU_EXECUTABLE NAMES qemu-riscv64 qemu-riscv64-static)
  find_program(CEPHYR_QEMU_AARCH64_EXECUTABLE NAMES qemu-aarch64 qemu-aarch64-static)
  set(cephyr_test_args)
  if(CEPHYR_QEMU_EXECUTABLE)
    list(APPEND cephyr_test_args --qemu "${CEPHYR_QEMU_EXECUTABLE}")
  endif()
  if(CEPHYR_QEMU_AARCH64_EXECUTABLE)
    list(APPEND cephyr_test_args --qemu-aarch64 "${CEPHYR_QEMU_AARCH64_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.driver COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_cli_regression.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.driver PROPERTIES TIMEOUT 300)
endif()
