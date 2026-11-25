cmake_minimum_required(VERSION 3.20)
project(Cephyr VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(CEPHYR_BUILD_TESTS "Build and register Cephyr tests" ON)
option(CEPHYR_INSTALL_PYTHON "Install the in-process Python bindings" ON)
option(CEPHYR_ENABLE_MOONSTONE "Enable optional Moonstone compiler integrations" OFF)
set(CEPHYR_ARCHVIRT_EXECUTABLE "" CACHE FILEPATH "ArchVirt executable for optional guest tests")
if(CEPHYR_BUILD_TESTS)
  enable_testing()
endif()
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The Cephyr compiler driver currently requires a Linux host")
endif()

get_filename_component(cephyr_repo "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(cephyr_vendor "${cephyr_repo}/third_party")
set(cephyr_archon "${cephyr_repo}/cornerstone/Archon")
# QML uses only Weighstone's dependency-free machine-word mask helper.
set(cephyr_weighstone "${cephyr_repo}/weighstone")
if(NOT TARGET Sandstone::sandslab)
  function(cephyr_add_sandslab)
    set(SANDSTONE_BUILD_TOOLS OFF)
    set(SANDSTONE_BUILD_TESTS OFF)
    add_subdirectory("${cephyr_repo}/sandstone/sandslab" sandslab EXCLUDE_FROM_ALL)
  endfunction()
  cephyr_add_sandslab()
endif()
if(NOT TARGET Blackstone::blackstone)
  if(DEFINED LIBSTONE_BUILD_BLACKSTONE AND NOT LIBSTONE_BUILD_BLACKSTONE)
    message(FATAL_ERROR "Building Cephyr in the libstone root requires LIBSTONE_BUILD_BLACKSTONE")
  endif()
  function(cephyr_add_blackstone)
    set(BLACKSTONE_BUILD_TESTS OFF)
    set(BLACKSTONE_BUILD_TOOLS OFF)
    # This is a private link dependency. Standalone Cephyr does not ship a
    # separately consumable Blackstone SDK or its Lua kernel catalog.
    add_subdirectory("${cephyr_repo}/blackstone" blackstone EXCLUDE_FROM_ALL)
    if(BUILD_SHARED_LIBS)
      install(TARGETS blackstone LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    endif()
  endfunction()
  cephyr_add_blackstone()
endif()
if(CEPHYR_ENABLE_MOONSTONE AND NOT TARGET Moonstone::Core)
  if(DEFINED LIBSTONE_BUILD_MOONSTONE AND NOT LIBSTONE_BUILD_MOONSTONE)
    message(FATAL_ERROR "CEPHYR_ENABLE_MOONSTONE requires LIBSTONE_BUILD_MOONSTONE in a root build")
  endif()
  function(cephyr_add_moonstone)
    # Only Cephyr's tests/tools are part of this standalone build by default.
    # Root builds reuse their existing Moonstone configuration and targets.
    if(NOT DEFINED MOONSTONE_BUILD_TESTS)
      set(MOONSTONE_BUILD_TESTS OFF)
    endif()
    if(NOT DEFINED MOONSTONE_BUILD_TOOLS)
      set(MOONSTONE_BUILD_TOOLS OFF)
    endif()
    if(NOT DEFINED EXOTRACT_BUILD_TESTS)
      set(EXOTRACT_BUILD_TESTS OFF)
    endif()
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${cephyr_repo}/moonstone" moonstone)
  endfunction()
  cephyr_add_moonstone()
endif()
include("${cephyr_repo}/cmake/LibstoneLua.cmake")
# The Archon generator is build-time Python; installed Cephyr checks the
# reviewed spec files directly and needs neither its C++ SDK nor libpython.
install(DIRECTORY "${cephyr_archon}/stdspec" DESTINATION
  "${CMAKE_INSTALL_DATADIR}/archon/cornerstone/Archon" FILES_MATCHING PATTERN "*.arch")
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
# Keep catalog parsing out of the CLI startup path. No runtime data path.
set(cephyr_x86_isa "${cephyr_repo}/sandstone/isa/x86.isa")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sandslab_catalog.hpp"
  COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/cmake/project_sandslab.py"
    "${cephyr_x86_isa}" "${CMAKE_CURRENT_BINARY_DIR}/sandslab_catalog.hpp"
  DEPENDS cmake/project_sandslab.py "${cephyr_x86_isa}" "${cephyr_repo}/sandstone/isa/simple_isa.py"
  VERBATIM)
find_package(Perl REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CEPHYR_YAML REQUIRED IMPORTED_TARGET yaml-0.1)
if(NOT EXISTS "${cephyr_vendor}/valijson/include/valijson/validator.hpp")
  message(FATAL_ERROR "Cephyr requires third_party/valijson; run scripts/get-deps.sh valijson")
endif()
find_path(CEPHYR_JSON_INCLUDE_DIR nlohmann/json.hpp HINTS "${cephyr_vendor}/nlohmann-json/include"
  "${cephyr_vendor}/libglr/third_party/libshell/third_party/taskflow/3rd-party")
if(NOT CEPHYR_JSON_INCLUDE_DIR)
  message(FATAL_ERROR "Cephyr requires nlohmann/json.hpp; run scripts/get-deps.sh nlohmann-json")
endif()

set(cephyr_c11_schema "${CMAKE_CURRENT_SOURCE_DIR}/contracts/C11Syntax.schema.json")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${cephyr_c11_schema}")
file(READ "${cephyr_c11_schema}" cephyr_c11_schema_json)
configure_file(cmake/C11SyntaxSchema.hpp.in C11SyntaxSchema.hpp @ONLY)

set(cephyr_llsynth "${cephyr_repo}/cornerstone/LLSynth/scripts")
set(cephyr_qml "${CMAKE_CURRENT_SOURCE_DIR}/qml")
set(cephyr_qml_build "${CMAKE_CURRENT_BINARY_DIR}/qml")
set(cephyr_qml_frontend "${cephyr_qml_build}/qml_frontend.py")
set(cephyr_qml_compiler "${cephyr_qml_build}/compile_rules.py")
set(cephyr_qml_bundle "${cephyr_qml_build}/builtin.qmb")
set(cephyr_qml_builtin
  "${cephyr_qml}/NativeCanonicalize.qpass"
  "${cephyr_qml}/NativeStrengthReduce.qpass")
add_custom_command(OUTPUT "${cephyr_qml_frontend}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${cephyr_qml_build}"
  COMMAND "${PERL_EXECUTABLE}" "${cephyr_llsynth}/llsynth.pl"
    --mode=interpret -T python3 --driver-mode=library -o "${cephyr_qml_frontend}"
    --depfile "${cephyr_qml_frontend}.d" "${CMAKE_CURRENT_SOURCE_DIR}/QML.llsynth"
  DEPENDS QML.llsynth "${cephyr_llsynth}/llsynth.pl"
    "${cephyr_llsynth}/LLSynthCompiler.pm" "${cephyr_llsynth}/LLSynthEmit.pm"
    "${cephyr_llsynth}/LLSynthMeta.pm" "${cephyr_llsynth}/LLSynthModules.pm"
    "${cephyr_llsynth}/LLSynthCompanions.pm"
  DEPFILE "${cephyr_qml_frontend}.d"
  VERBATIM)
add_custom_command(OUTPUT "${cephyr_qml_compiler}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${cephyr_qml_build}"
  COMMAND "${CMAKE_COMMAND}" -E copy "${cephyr_qml}/compile_rules.py" "${cephyr_qml_compiler}"
  DEPENDS "${cephyr_qml}/compile_rules.py"
  VERBATIM)
add_custom_command(OUTPUT "${cephyr_qml_bundle}"
  COMMAND "${Python3_EXECUTABLE}" -I -B "${cephyr_qml_compiler}"
    --frontend "${cephyr_qml_frontend}" --output "${cephyr_qml_bundle}" ${cephyr_qml_builtin}
  DEPENDS "${cephyr_qml_frontend}" "${cephyr_qml_compiler}" ${cephyr_qml_builtin}
  VERBATIM)
add_custom_target(cephyr_qml_assets DEPENDS "${cephyr_qml_bundle}")

set(cephyr_mir "${CMAKE_CURRENT_SOURCE_DIR}/mir")
set(cephyr_acsem "${cephyr_repo}/scripts/acsem")
set(cephyr_mir_model "${CMAKE_CURRENT_BINARY_DIR}/CephyrMIRModel.hpp")
set(cephyr_mir_json "${CMAKE_CURRENT_BINARY_DIR}/mir_model.json")
file(GLOB cephyr_acsem_modules CONFIGURE_DEPENDS "${cephyr_acsem}/Acsem/*.pm")
add_custom_command(OUTPUT "${cephyr_mir_model}"
  BYPRODUCTS "${cephyr_mir_json}"
  COMMAND "${PERL_EXECUTABLE}" "${cephyr_acsem}/acsem.pl" --emit json
    "${CMAKE_CURRENT_SOURCE_DIR}/MIR.acsem" -o "${cephyr_mir_json}"
  COMMAND "${Python3_EXECUTABLE}" -I -B "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_mir_model.py"
    "${cephyr_mir_json}" "${cephyr_mir_model}"
  DEPENDS MIR.acsem cmake/generate_mir_model.py "${cephyr_acsem}/acsem.pl" ${cephyr_acsem_modules}
  VERBATIM)
file(GLOB cephyr_mir_catalog_sources CONFIGURE_DEPENDS
  "${cephyr_mir}/*.mirpass" "${cephyr_mir}/*.mirlower")
set(cephyr_mir_catalog "${CMAKE_CURRENT_BINARY_DIR}/mir_catalog.hpp")
add_custom_command(OUTPUT "${cephyr_mir_catalog}"
  COMMAND "${Python3_EXECUTABLE}" -I -B "${cephyr_mir}/compile_catalog.py"
    --catalog "${cephyr_mir}" --output "${cephyr_mir_catalog}"
  DEPENDS "${cephyr_mir}/compile_catalog.py" ${cephyr_mir_catalog_sources}
  VERBATIM)
add_custom_target(cephyr_mir_assets DEPENDS "${cephyr_mir_catalog}" "${cephyr_mir_model}")

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
target_compile_definitions(cephyr_ucpp PRIVATE UCPP_STDC_VERSION="201112L" UCPP_C11_MINIMAL_FEATURES)
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

set_target_properties(cephyr_cparser cephyr_ucpp cephyr_tomlc99 cephyr_diagnostics
  PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(cephyr_native "${cephyr_archon}/archon/irgen/native")
set(cephyr_satie "${cephyr_vendor}/satie")
add_library(cephyr_compiler SHARED
  src/main.cpp src/process.cpp src/config.cpp src/target.cpp src/host_target.cpp src/cache.cpp
  src/preprocess.cpp src/parser.cpp src/syntax.cpp src/interface.cpp src/semantics.cpp src/lower.cpp
  src/lower_globals.cpp src/lower_expr.cpp src/lower_stmt.cpp src/mir_bridge.cpp src/backend.cpp src/sdk.cpp
  src/embedding.cpp src/moonstone_tools.cpp src/qml_rewrite.cpp src/blackstone_opt.cpp
  extension/lua_extensions.cpp extension/native_plugins.cpp
  src/mir_verify.c "${cephyr_native}/MIRText.c" "${cephyr_native}/NativeVerify.c"
  "${cephyr_native}/Legalizer.c"
  "${cephyr_repo}/novostone/Datalog/Datalog.cpp" "${cephyr_satie}/src/SatieMemory.cpp")
add_library(cephyr_literal_parser STATIC stdlib/src/strtofp.c stdlib/src/real_bits.c src/quad_fold.cpp)
target_compile_features(cephyr_literal_parser PRIVATE cxx_std_20)
set_target_properties(cephyr_literal_parser PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 11)
target_compile_definitions(cephyr_literal_parser PRIVATE CEPHYR_LITERAL_PARSER=1)
target_link_libraries(cephyr_compiler PRIVATE cephyr_literal_parser)
add_library(Cephyr::Cephyr ALIAS cephyr_compiler)
target_sources(cephyr_compiler PRIVATE src/sandslab_opt.cpp "${CMAKE_CURRENT_BINARY_DIR}/sandslab_catalog.hpp")
target_include_directories(cephyr_compiler PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(cephyr_compiler PRIVATE Sandstone::sandslab)
add_dependencies(cephyr_compiler cephyr_qml_assets cephyr_mir_assets)
target_compile_features(cephyr_compiler PRIVATE cxx_std_20 INTERFACE cxx_std_17)
set_target_properties(cephyr_compiler PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF CXX_EXTENSIONS OFF
  OUTPUT_NAME cephyr EXPORT_NAME Cephyr
  VERSION "${PROJECT_VERSION}" SOVERSION 1)
target_include_directories(cephyr_compiler PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(cephyr_compiler PRIVATE -Wall -Wextra)
endif()
# Keep vendor and implementation symbols out of the library's link interface.
set(cephyr_exports "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Cephyr.map")
target_link_options(cephyr_compiler PRIVATE "LINKER:--version-script=${cephyr_exports}" "LINKER:-z,defs")
set_property(TARGET cephyr_compiler APPEND PROPERTY LINK_DEPENDS "${cephyr_exports}")
target_include_directories(cephyr_compiler PRIVATE "${cephyr_native}" "${cephyr_weighstone}")
target_include_directories(cephyr_compiler SYSTEM PRIVATE
  "${cephyr_repo}"
  "${cephyr_vendor}/valijson/include"
  "${CEPHYR_JSON_INCLUDE_DIR}"
  "${cephyr_satie}/include" "${cephyr_satie}/third_party/memtkx/include")
target_link_libraries(cephyr_compiler PRIVATE cephyr_diagnostics cephyr_cparser cephyr_ucpp cephyr_tomlc99
  libstone_kaguya Blackstone::blackstone PkgConfig::CEPHYR_YAML ${CMAKE_DL_LIBS})
if(TARGET Wetstone::wetstone)
  target_link_libraries(cephyr_compiler PRIVATE Wetstone::wetstone)
endif()

set(cephyr_moonstone_test_args)
function(cephyr_moonstone_tool key dependency)
  string(TOUPPER "${key}" macro)
  if(CEPHYR_ENABLE_MOONSTONE AND TARGET "${dependency}")
    target_link_libraries(cephyr_compiler PRIVATE "${dependency}")
    target_compile_definitions(cephyr_compiler PRIVATE "CEPHYR_HAVE_${macro}=1")
    list(APPEND cephyr_moonstone_test_args --have "${key}")
    set(cephyr_moonstone_test_args "${cephyr_moonstone_test_args}" PARENT_SCOPE)
  else()
    target_compile_definitions(cephyr_compiler PRIVATE "CEPHYR_HAVE_${macro}=0")
  endif()
endfunction()
cephyr_moonstone_tool(prolific Moonstone::Prolific)
cephyr_moonstone_tool(plaid Moonstone::Plaid)
cephyr_moonstone_tool(exoml Moonstone::ExoML)
cephyr_moonstone_tool(exotract Moonstone::Exotract)
# These Moonstone libraries can also be built in the LibStone parent, where
# the standalone Cephyr PIC default does not apply.
if(CEPHYR_ENABLE_MOONSTONE)
  foreach(dependency IN ITEMS moonstone_exoml_runtime moonstone_exoml_extract moonstone_exotract
      moonstone_qmr_compiler moonstone_qmr)
    if(TARGET "${dependency}")
      set_property(TARGET "${dependency}" PROPERTY POSITION_INDEPENDENT_CODE ON)
    endif()
  endforeach()
endif()
cephyr_moonstone_tool(qmr Moonstone::QmrCompiler)
if(CEPHYR_ENABLE_MOONSTONE AND TARGET Moonstone::Chisel AND TARGET Moonstone::Syren
    AND TARGET Moonstone::Supersonic AND TARGET Nerotypical::Sequent)
  target_sources(cephyr_compiler PRIVATE src/moonstone_analysis.cpp)
  target_link_libraries(cephyr_compiler PRIVATE Moonstone::Chisel Moonstone::Syren
    Moonstone::Supersonic Nerotypical::Sequent)
  target_compile_definitions(cephyr_compiler PRIVATE CEPHYR_WITH_MOONSTONE_ANALYSIS=1)
  set(cephyr_has_moonstone_analysis ON)
endif()
set(cephyr_test_prolific_artifact)
if(CEPHYR_ENABLE_MOONSTONE AND TARGET Moonstone::Prolific)
  set(cephyr_test_prolific_artifact "$<TARGET_SONAME_FILE:Moonstone::Prolific>")
endif()

add_executable(cephyr src/cli.cpp)
target_link_libraries(cephyr PRIVATE Cephyr::Cephyr)
target_compile_features(cephyr PRIVATE cxx_std_20)
set_target_properties(cephyr PROPERTIES CXX_EXTENSIONS OFF)

set(cephyr_data_dir "${CMAKE_INSTALL_DATADIR}/cephyr")
install(FILES "${cephyr_c11_schema}" DESTINATION "${cephyr_data_dir}/contracts")
include(cmake/NativePackages.cmake)
include(cmake/NativeTools.cmake)
file(RELATIVE_PATH cephyr_lib_relative_resources
  "${CMAKE_INSTALL_FULL_LIBDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/cephyr")
file(RELATIVE_PATH cephyr_lib_relative_archon
  "${CMAKE_INSTALL_FULL_LIBDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/archon/cornerstone/Archon")
file(RELATIVE_PATH cephyr_bin_relative_lib
  "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
set_target_properties(cephyr PROPERTIES INSTALL_RPATH "$ORIGIN/${cephyr_bin_relative_lib}")
get_target_property(cephyr_blackstone_type Blackstone::blackstone TYPE)
if(cephyr_blackstone_type STREQUAL "SHARED_LIBRARY")
  set(cephyr_test_blackstone_artifact "$<TARGET_SONAME_FILE:Blackstone::blackstone>")
endif()
if(CEPHYR_ENABLE_MOONSTONE OR cephyr_blackstone_type STREQUAL "SHARED_LIBRARY")
  # Cephyr and its shared dependencies install beside one another.
  set_property(TARGET cephyr_compiler PROPERTY INSTALL_RPATH "$ORIGIN")
endif()
target_compile_definitions(cephyr_compiler PRIVATE
  AN_GENERIC_MIR_VERIFY=1
  CEPHYR_VERSION="${PROJECT_VERSION}"
  CEPHYR_BUILD_LIBDIR="$<TARGET_FILE_DIR:cephyr_compiler>"
  CEPHYR_SOURCE_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}"
  CEPHYR_SOURCE_ARCHON="${cephyr_archon}"
  CEPHYR_BUILD_QML="${cephyr_qml_build}"
  CEPHYR_BUILD_PYTHON="${Python3_EXECUTABLE}"
  CEPHYR_LIB_RELATIVE_RESOURCES="${cephyr_lib_relative_resources}"
  CEPHYR_LIB_RELATIVE_ARCHON="${cephyr_lib_relative_archon}")

set(cephyr_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/Cephyr")
configure_package_config_file(cmake/CephyrConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/CephyrConfig.cmake"
  INSTALL_DESTINATION "${cephyr_package_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/CephyrConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(TARGETS cephyr RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(TARGETS cephyr_compiler EXPORT CephyrTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(EXPORT CephyrTargets NAMESPACE Cephyr:: DESTINATION "${cephyr_package_dir}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CephyrConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/CephyrConfigVersion.cmake" DESTINATION "${cephyr_package_dir}")
install(FILES include/cephyr/Cephyr.h include/cephyr/Cephyr.hpp include/cephyr/CephyrSDK.h
  include/cephyr/CephyrPlugin.h
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/cephyr")
if(CEPHYR_INSTALL_PYTHON)
  install(DIRECTORY python/cephyr DESTINATION "${CMAKE_INSTALL_LIBDIR}/cephyr/python"
    FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)
endif()
install(DIRECTORY stdlib triples DESTINATION "${cephyr_data_dir}" USE_SOURCE_PERMISSIONS)
install(FILES "${cephyr_qml_compiler}" "${cephyr_qml_frontend}" "${cephyr_qml_bundle}"
  "${cephyr_qml}/README.md" ${cephyr_qml_builtin}
  DESTINATION "${cephyr_data_dir}/qml")
install(FILES benchmark/README.md benchmark/run.py benchmark/workloads.py benchmark/benchmark.py benchmark/manifest.json
  DESTINATION "${cephyr_data_dir}/benchmark")
install(FILES benchmark/tools/time_sandslab.py DESTINATION "${cephyr_data_dir}/benchmark/tools")
install(DIRECTORY benchmark/fixtures DESTINATION "${cephyr_data_dir}/benchmark")
install(FILES Cephyr.toml README.md C11.md EMBEDDING.md SDK.md
  DESTINATION "${cephyr_data_dir}")
install(FILES MOONSTONE.md MOONSTONE_ANALYSIS.md DESTINATION "${cephyr_data_dir}" OPTIONAL)
install(FILES extension/README.md extension/CONFIGURATION.md DESTINATION "${cephyr_data_dir}/extension")
install(FILES docs/moonstone-tools.md docs/NATIVE_FP_WIRING.md docs/FactIR.md docs/SandSLAB.md
  docs/MIRModel.md docs/C11_ATOMICS.md docs/C11_VLA.md
  DESTINATION "${cephyr_data_dir}/docs")
install(FILES mir/README.md DESTINATION "${cephyr_data_dir}/mir")
install(FILES examples/sdk_desugar.c DESTINATION "${cephyr_data_dir}/examples")

if(CEPHYR_BUILD_TESTS)
  add_test(NAME cephyr.sandslab COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_sandslab.py" "$<TARGET_FILE:cephyr>")
  add_executable(cephyr-diagnostics-test tests/diagnostics.cpp)
  target_link_libraries(cephyr-diagnostics-test PRIVATE cephyr_diagnostics)
  add_test(NAME cephyr.diagnostics COMMAND cephyr-diagnostics-test)
  add_executable(cephyr-preprocess-test tests/preprocess.cpp src/preprocess.cpp)
  target_link_libraries(cephyr-preprocess-test PRIVATE cephyr_diagnostics cephyr_ucpp)
  add_test(NAME cephyr.preprocess COMMAND cephyr-preprocess-test)
  add_executable(cephyr-parser-test tests/parser.cpp src/parser.cpp src/preprocess.cpp)
  target_link_libraries(cephyr-parser-test PRIVATE cephyr_diagnostics cephyr_ucpp cephyr_cparser)
  add_test(NAME cephyr.parser COMMAND cephyr-parser-test)
  add_executable(cephyr-floating-literal-test tests/floating_literal.cpp)
  target_link_libraries(cephyr-floating-literal-test PRIVATE cephyr_literal_parser)
  target_compile_features(cephyr-floating-literal-test PRIVATE cxx_std_20)
  add_test(NAME cephyr.floating-literal COMMAND cephyr-floating-literal-test)
  add_test(NAME cephyr.real-bits COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_real_bits.py")
  add_executable(cephyr-cache-test tests/cache.cpp src/cache.cpp)
  target_link_libraries(cephyr-cache-test PRIVATE cephyr_diagnostics)
  add_test(NAME cephyr.cache COMMAND cephyr-cache-test)
  add_executable(cephyr-host-target-test
    tests/host_target.cpp src/host_target.cpp src/target.cpp src/process.cpp src/config.cpp)
  add_dependencies(cephyr-host-target-test cephyr_mir_assets)
  target_include_directories(cephyr-host-target-test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(cephyr-host-target-test PRIVATE
    cephyr_diagnostics libstone_kaguya cephyr_tomlc99)
  add_test(NAME cephyr.host-target COMMAND cephyr-host-target-test
    "${CMAKE_CURRENT_SOURCE_DIR}" "${cephyr_archon}")
  find_package(Threads REQUIRED)
  add_executable(cephyr-c-api-test tests/embedding.c)
  target_compile_definitions(cephyr-c-api-test PRIVATE CEPHYR_TEST_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
  set_target_properties(cephyr-c-api-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
  target_link_libraries(cephyr-c-api-test PRIVATE Cephyr::Cephyr)
  add_test(NAME cephyr.c-api COMMAND cephyr-c-api-test)
  add_executable(cephyr-cpp-api-test tests/embedding.cpp)
  target_compile_features(cephyr-cpp-api-test PRIVATE cxx_std_20)
  target_link_libraries(cephyr-cpp-api-test PRIVATE Cephyr::Cephyr Threads::Threads)
  add_test(NAME cephyr.cpp-api COMMAND cephyr-cpp-api-test)
  add_test(NAME cephyr.c11-ast COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_c11_ast.py" "$<TARGET_FILE:cephyr>")
  add_test(NAME cephyr.c11-interface COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_c11_interface.py" "$<TARGET_FILE:cephyr>")
  set_tests_properties(cephyr.c-api cephyr.cpp-api PROPERTIES TIMEOUT 60)
  add_executable(cephyr-sdk-test tests/sdk.c)
  set_target_properties(cephyr-sdk-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
  target_link_libraries(cephyr-sdk-test PRIVATE Cephyr::Cephyr)
  add_test(NAME cephyr.sdk COMMAND cephyr-sdk-test "${CMAKE_CURRENT_SOURCE_DIR}" "${cephyr_archon}")
  add_executable(cephyr-lua-api-test tests/lua_extensions.c)
  set_target_properties(cephyr-lua-api-test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
  target_link_libraries(cephyr-lua-api-test PRIVATE Cephyr::Cephyr)
  add_test(NAME cephyr.lua-api COMMAND cephyr-lua-api-test)
  add_test(NAME cephyr.lua-driver COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_extensions.py" "$<TARGET_FILE:cephyr>")
  set_tests_properties(cephyr.lua-api cephyr.lua-driver PROPERTIES TIMEOUT 30)
  add_test(NAME cephyr.config-extensions COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_config_extensions.py" "$<TARGET_FILE:cephyr>"
    "${CMAKE_C_COMPILER}")
  set_tests_properties(cephyr.config-extensions PROPERTIES TIMEOUT 60)
  set(cephyr_profile_test_args)
  if(cephyr_has_moonstone_analysis)
    list(APPEND cephyr_profile_test_args --enabled)
  endif()
  add_test(NAME cephyr.profiles COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_profiles.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_profile_test_args})
  set_tests_properties(cephyr.profiles PROPERTIES TIMEOUT 120)
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
  add_test(NAME cephyr.mir-graph COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mir_graph.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.mir-graph PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.mir-model COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
    "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mir_model.py")
  add_test(NAME cephyr.mir-catalog COMMAND "${CMAKE_COMMAND}" -E env "CXX=${CMAKE_CXX_COMPILER}"
    "${Python3_EXECUTABLE}" -B "${cephyr_mir}/test_catalog.py")
  set_tests_properties(cephyr.mir-model cephyr.mir-catalog PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.driver COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_cli_regression.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.driver PROPERTIES TIMEOUT 300)
  set(cephyr_qml_test_args)
  if(cephyr_test_prolific_artifact)
    list(APPEND cephyr_qml_test_args --prolific)
  endif()
  add_test(NAME cephyr.qml-driver COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qml_driver.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_qml_test_args} ${cephyr_test_args})
  set_tests_properties(cephyr.qml-driver PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.factir COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_factir.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.factir PROPERTIES TIMEOUT 120)
  add_executable(cephyr-factir-proof-test tests/factir_proof.cpp "${cephyr_satie}/src/SatieMemory.cpp")
  target_compile_features(cephyr-factir-proof-test PRIVATE cxx_std_20)
  target_include_directories(cephyr-factir-proof-test PRIVATE include "${cephyr_native}")
  target_include_directories(cephyr-factir-proof-test SYSTEM PRIVATE
    "${cephyr_repo}"
    "${cephyr_satie}/include" "${cephyr_satie}/third_party/memtkx/include")
  add_test(NAME cephyr.factir-proof COMMAND cephyr-factir-proof-test)
  add_test(NAME cephyr.qml-cfg COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_qml_cfg.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.qml-cfg PROPERTIES TIMEOUT 120)
  if(CEPHYR_ARCHVIRT_EXECUTABLE)
    add_test(NAME cephyr.archvirt COMMAND "${Python3_EXECUTABLE}" -B
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_archvirt.py" "$<TARGET_FILE:cephyr>"
      --archvirt "${CEPHYR_ARCHVIRT_EXECUTABLE}")
    set_tests_properties(cephyr.archvirt PROPERTIES TIMEOUT 180)
  endif()
  add_test(NAME cephyr.qml-frontend COMMAND "${Python3_EXECUTABLE}" -B -m unittest
    discover -s "${cephyr_qml}/tests")
  set_tests_properties(cephyr.qml-frontend PROPERTIES TIMEOUT 60)
  add_executable(cephyr-qml-rewrite-test tests/qml_rewrite.cpp src/qml_rewrite.cpp
    src/mir_verify.c "${cephyr_native}/MIRText.c" "${cephyr_native}/NativeVerify.c"
    "${cephyr_native}/Legalizer.c")
  target_compile_features(cephyr-qml-rewrite-test PRIVATE c_std_99 cxx_std_20)
  target_compile_definitions(cephyr-qml-rewrite-test PRIVATE AN_GENERIC_MIR_VERIFY=1)
  target_include_directories(cephyr-qml-rewrite-test PRIVATE "${cephyr_native}" "${cephyr_weighstone}")
  target_link_libraries(cephyr-qml-rewrite-test PRIVATE cephyr_diagnostics)
  add_test(NAME cephyr.qml-rewrite COMMAND cephyr-qml-rewrite-test)
  add_executable(cephyr-blackstone-test tests/blackstone_opt.cpp src/blackstone_opt.cpp
    src/mir_verify.c "${cephyr_native}/MIRText.c" "${cephyr_native}/NativeVerify.c"
    "${cephyr_native}/Legalizer.c")
  target_compile_features(cephyr-blackstone-test PRIVATE c_std_99 cxx_std_20)
  target_compile_definitions(cephyr-blackstone-test PRIVATE AN_GENERIC_MIR_VERIFY=1)
  target_include_directories(cephyr-blackstone-test PRIVATE "${cephyr_native}")
  target_link_libraries(cephyr-blackstone-test PRIVATE Blackstone::blackstone cephyr_diagnostics)
  add_test(NAME cephyr.blackstone COMMAND cephyr-blackstone-test)
  add_test(NAME cephyr.c11-sample COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_c11_sample.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.c11-sample PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-records COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_records.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_test_args})
  set_tests_properties(cephyr.c11-records PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-atomics COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_atomics.py" "$<TARGET_FILE:cephyr>"
    --cc "${CMAKE_C_COMPILER}" ${cephyr_test_args})
  set_tests_properties(cephyr.c11-atomics PROPERTIES TIMEOUT 120)
  foreach(feature IN ITEMS anonymous array_parameters bitfields compound_literals frontend strings vla volatile volatile_aggregates)
    string(REPLACE "_" "-" test_feature "${feature}")
    add_test(NAME cephyr.c11-${test_feature} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_${feature}.py" "$<TARGET_FILE:cephyr>"
      ${cephyr_test_args})
    set_tests_properties(cephyr.c11-${test_feature} PROPERTIES TIMEOUT 120)
  endforeach()
  foreach(feature IN ITEMS stdarg long_double)
    string(REPLACE "_" "-" test_feature "${feature}")
    add_test(NAME cephyr.c11-amd64-${test_feature} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_amd64_${feature}.py"
      "$<TARGET_FILE:cephyr>" --cc "${CMAKE_C_COMPILER}")
    set_tests_properties(cephyr.c11-amd64-${test_feature} PROPERTIES TIMEOUT 120)
  endforeach()
  set(cephyr_record_abi_test_args)
  if(CEPHYR_QEMU_EXECUTABLE)
    list(APPEND cephyr_record_abi_test_args --qemu "${CEPHYR_QEMU_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.c11-rv64-stdarg COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_rv64_stdarg.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_record_abi_test_args})
  set_tests_properties(cephyr.c11-rv64-stdarg PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-record-abi COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_record_abi.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_record_abi_test_args})
  set_tests_properties(cephyr.c11-record-abi PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-rv64-soft-fp COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_rv64_soft_fp.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_record_abi_test_args})
  set_tests_properties(cephyr.c11-rv64-soft-fp PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-rv64-long-double COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_rv64_long_double.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_record_abi_test_args})
  set_tests_properties(cephyr.c11-rv64-long-double PROPERTIES TIMEOUT 180)
  set_tests_properties(cephyr.c11-rv64-soft-fp cephyr.c11-rv64-long-double PROPERTIES
    ENVIRONMENT "CEPHYR_TEST_CC_RV64=${CEPHYR_PACKAGE_CC_RISCV64IM}")
  set(cephyr_aarch64_stdarg_test_args)
  if(CEPHYR_QEMU_AARCH64_EXECUTABLE)
    list(APPEND cephyr_aarch64_stdarg_test_args --qemu "${CEPHYR_QEMU_AARCH64_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.c11-aarch64-stdarg COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_aarch64_stdarg.py"
    "$<TARGET_FILE:cephyr>" ${cephyr_aarch64_stdarg_test_args})
  set_tests_properties(cephyr.c11-aarch64-stdarg PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-aarch64-long-double COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_aarch64_long_double.py"
    "$<TARGET_FILE:cephyr>" ${cephyr_aarch64_stdarg_test_args})
  set_tests_properties(cephyr.c11-aarch64-long-double PROPERTIES TIMEOUT 180)
  set_tests_properties(cephyr.c11-aarch64-long-double PROPERTIES
    ENVIRONMENT "CEPHYR_TEST_CC_AARCH64=${CEPHYR_PACKAGE_CC_AArch64}")
  set(cephyr_linkage_test_args)
  if(CEPHYR_QEMU_EXECUTABLE)
    list(APPEND cephyr_linkage_test_args --qemu-riscv64 "${CEPHYR_QEMU_EXECUTABLE}")
  endif()
  if(CEPHYR_QEMU_AARCH64_EXECUTABLE)
    list(APPEND cephyr_linkage_test_args --qemu-aarch64 "${CEPHYR_QEMU_AARCH64_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.c11-linkage COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_linkage.py"
    "$<TARGET_FILE:cephyr>" ${cephyr_linkage_test_args})
  set_tests_properties(cephyr.c11-linkage PROPERTIES TIMEOUT 120)
  foreach(feature IN ITEMS alignment enums)
    add_test(NAME cephyr.c11-${feature} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_${feature}.py" "$<TARGET_FILE:cephyr>"
      ${cephyr_test_args})
    set_tests_properties(cephyr.c11-${feature} PROPERTIES TIMEOUT 120)
  endforeach()
  set(cephyr_fp_test_args)
  if(CEPHYR_QEMU_AARCH64_EXECUTABLE)
    list(APPEND cephyr_fp_test_args --qemu-aarch64 "${CEPHYR_QEMU_AARCH64_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.c11-floating COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_floating.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_fp_test_args})
  set_tests_properties(cephyr.c11-floating PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.c11-static-fp COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_static_fp.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_fp_test_args})
  set_tests_properties(cephyr.c11-static-fp PROPERTIES TIMEOUT 120)
  foreach(feature IN ITEMS fenv printf_float scanf_float signals strtofp wide_printf)
    string(REPLACE "_" "-" test_feature "${feature}")
    add_test(NAME cephyr.c11-${test_feature} COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cephyr_c11_${feature}.py"
      --cephyr "$<TARGET_FILE:cephyr>" ${cephyr_test_args})
    set_tests_properties(cephyr.c11-${test_feature} PROPERTIES TIMEOUT 120)
  endforeach()
  add_test(NAME cephyr.moonstone-tools COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_moonstone_tools.py" "$<TARGET_FILE:cephyr>"
    --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}" ${cephyr_moonstone_test_args})
  set_tests_properties(cephyr.moonstone-tools PROPERTIES TIMEOUT 120)
  set(cephyr_analysis_test_args)
  if(NOT cephyr_has_moonstone_analysis)
    list(APPEND cephyr_analysis_test_args --disabled)
  endif()
  add_test(NAME cephyr.moonstone-analysis COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_moonstone_analysis.py" "$<TARGET_FILE:cephyr>"
    ${cephyr_analysis_test_args})
  set_tests_properties(cephyr.moonstone-analysis PROPERTIES TIMEOUT 120)
  set(cephyr_libc_test_environment "AR=${CEPHYR_PACKAGE_AR}"
    "CEPHYR_TEST_CC_AMD64=${CEPHYR_PACKAGE_CC_AMD64}"
    "CEPHYR_TEST_CC_AARCH64=${CEPHYR_PACKAGE_CC_AArch64}"
    "CEPHYR_TEST_CC_RV64=${CEPHYR_PACKAGE_CC_RISCV64IM}")
  if(CEPHYR_QEMU_EXECUTABLE)
    list(APPEND cephyr_libc_test_environment "CEPHYR_TEST_QEMU=${CEPHYR_QEMU_EXECUTABLE}")
  endif()
  if(CEPHYR_QEMU_AARCH64_EXECUTABLE)
    list(APPEND cephyr_libc_test_environment "CEPHYR_TEST_QEMU_AARCH64=${CEPHYR_QEMU_AARCH64_EXECUTABLE}")
  endif()
  add_test(NAME cephyr.libc COMMAND "${CMAKE_COMMAND}" -E env
    --unset=CEPHYR_LIBC_TARGET_OPTION --unset=CEPHYR_LIBC_CFLAGS
    sh "${CMAKE_CURRENT_SOURCE_DIR}/stdlib/tests/run.sh" "${CMAKE_CURRENT_BINARY_DIR}/libc-test")
  set_tests_properties(cephyr.libc PROPERTIES
    TIMEOUT 120 ENVIRONMENT "${cephyr_libc_test_environment}")
  add_test(NAME cephyr.python-api COMMAND "${CMAKE_COMMAND}" -E env
    "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/python"
    "CEPHYR_LIBRARY=$<TARGET_FILE:cephyr_compiler>"
    "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_python_api.py")
  set_tests_properties(cephyr.python-api PROPERTIES TIMEOUT 120)
  add_test(NAME cephyr.installed-api COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}"
    "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/installed-api-test"
    "-DTEST_CONFIG=$<CONFIG>"
    "-DTEST_CTEST=${CMAKE_CTEST_COMMAND}"
    "-DINSTALL_BINDIR=${CMAKE_INSTALL_BINDIR}"
    "-DINSTALL_DATADIR=${CMAKE_INSTALL_DATADIR}"
    "-DINSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
    "-DTEST_PYTHON=${Python3_EXECUTABLE}"
    "-DTEST_MOONSTONE_PROLIFIC=${cephyr_test_prolific_artifact}"
    "-DTEST_BLACKSTONE=${cephyr_test_blackstone_artifact}"
    "-DTEST_PYTHON_BINDINGS=${CEPHYR_INSTALL_PYTHON}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_installed_api.cmake")
  set_tests_properties(cephyr.installed-api PROPERTIES TIMEOUT 180)
endif()
