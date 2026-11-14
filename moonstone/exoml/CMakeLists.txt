find_package(Perl REQUIRED)
find_package(Threads REQUIRED)
set(exoml_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${exoml_generated}")
set(exoml_scripts "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts")
add_custom_command(
  OUTPUT "${exoml_generated}/exoml_parser.c" "${exoml_generated}/exoml_ast.h" "${exoml_generated}/exoml_visitor.h"
  COMMAND "${CMAKE_COMMAND}" "-DPERL=${PERL_EXECUTABLE}" "-DSCRIPTS=${exoml_scripts}"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}" "-DOUTPUT=${exoml_generated}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateParser.cmake"
  DEPENDS ExoML.g ExoML.asdl cmake/GenerateParser.cmake
    "${exoml_scripts}/aurocks.pl" "${exoml_scripts}/asdl.pl"
  VERBATIM)
add_library(moonstone_exoml_runtime STATIC src/Interpreter.cpp src/Parser.cpp src/StaticFFI.cpp src/CDecl.cpp
  src/Contract.cpp src/JsonReader.cpp
  stdlib/StaticFFI.cpp "${exoml_generated}/exoml_parser.c")
add_library(Moonstone::ExoML ALIAS moonstone_exoml)
target_compile_features(moonstone_exoml_runtime PUBLIC cxx_std_20)
target_link_libraries(moonstone_exoml_runtime PUBLIC moonstone_core Threads::Threads)
target_include_directories(moonstone_exoml_runtime PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE "${exoml_generated}" src)
set_target_properties(moonstone_exoml_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(moonstone_exoml INTERFACE moonstone_exoml_runtime)
if(MOONSTONE_BUILD_TOOLS)
  add_executable(exoml tools/exoml.cpp)
  target_link_libraries(exoml PRIVATE moonstone_exoml)
  install(TARGETS exoml RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ExoML)
endif()
set_target_properties(moonstone_exoml_runtime PROPERTIES EXPORT_NAME ExoMLRuntime)
set_target_properties(moonstone_exoml PROPERTIES EXPORT_NAME ExoML)
set_target_properties(moonstone_core PROPERTIES EXPORT_NAME Core)
install(TARGETS moonstone_core moonstone_exoml moonstone_exoml_runtime EXPORT MoonstoneExoMLTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ExoML)
install(EXPORT MoonstoneExoMLTargets NAMESPACE Moonstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoML COMPONENT ExoML)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT ExoML FILES_MATCHING PATTERN "*.hpp")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../core/" DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonstone/core
  COMPONENT ExoML FILES_MATCHING PATTERN "*.hpp")
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/MoonstoneExoMLConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoML)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoML COMPONENT ExoML)

# Developer-only C11 discovery: no libclang/crypto dependency in ExoMLRuntime.
option(MOONSTONE_EXOML_EXTRACT "Build libclang C11 discovery library and tool" OFF)
set(MOONSTONE_EXOML_MULIB_ROOT "" CACHE PATH "Canonical MuLIB checkout for opt-in discovery tests")
if(MOONSTONE_EXOML_EXTRACT)
  find_path(MOONSTONE_EXOML_LIBCLANG_INCLUDE_DIR clang-c/Index.h REQUIRED)
  find_library(MOONSTONE_EXOML_LIBCLANG_LIBRARY NAMES clang libclang REQUIRED)
  find_package(OpenSSL REQUIRED COMPONENTS Crypto)
  set(MOONSTONE_EXOML_JSON_INCLUDE_DIR
    "${MOONSTONE_VENDOR_ROOT}/metatk/third_party/tomlplusplus/vendor" CACHE PATH
    "Directory containing the existing nlohmann single-header json.hpp (developer-only)")
  if(NOT EXISTS "${MOONSTONE_EXOML_JSON_INCLUDE_DIR}/json.hpp")
    message(FATAL_ERROR "C11 compilation-database ingestion requires nlohmann json.hpp; set MOONSTONE_EXOML_JSON_INCLUDE_DIR")
  endif()
  add_library(Moonstone::ExoMLClang UNKNOWN IMPORTED GLOBAL)
  set_target_properties(Moonstone::ExoMLClang PROPERTIES
    IMPORTED_LOCATION "${MOONSTONE_EXOML_LIBCLANG_LIBRARY}")
  # Source identity is part of every cache key; source edits also trigger reconfigure.
  set(exoml_extract_identity "${CMAKE_CXX_COMPILER_ID};${CMAKE_CXX_COMPILER_VERSION};${CMAKE_CXX_FLAGS};${CMAKE_BUILD_TYPE}")
  foreach(source IN ITEMS src/Extract.cpp src/ExtractCache.cpp src/ExtractCache.hpp src/CacheCodec.cpp
      src/CDecl.cpp include/exoml/Extract.hpp include/exoml/CDecl.hpp ../core/Core.hpp
      tools/exoml-bind.cpp src/CompileOptions.cpp src/CompileOptions.hpp
      cmake/ExtractorBuild.hpp.in CMakeLists.txt)
    set(identity_source "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${identity_source}")
    file(SHA256 "${identity_source}" identity_hash)
    string(APPEND exoml_extract_identity ";${source}:${identity_hash}")
  endforeach()
  file(SHA256 "${MOONSTONE_EXOML_JSON_INCLUDE_DIR}/json.hpp" exoml_json_hash)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${MOONSTONE_EXOML_JSON_INCLUDE_DIR}/json.hpp")
  string(APPEND exoml_extract_identity ";json:${exoml_json_hash}")
  string(SHA256 exoml_extract_build_id "${exoml_extract_identity}")
  configure_file(cmake/ExtractorBuild.hpp.in "${exoml_generated}/ExtractorBuild.hpp" @ONLY)
  add_library(moonstone_exoml_extract STATIC src/Extract.cpp src/ExtractCache.cpp src/CacheCodec.cpp src/CompileOptions.cpp)
  add_library(Moonstone::ExoMLExtract ALIAS moonstone_exoml_extract)
  set_target_properties(moonstone_exoml_extract PROPERTIES EXPORT_NAME ExoMLExtract)
  target_link_libraries(moonstone_exoml_extract PUBLIC moonstone_exoml
    PRIVATE Moonstone::ExoMLClang OpenSSL::Crypto)
  target_include_directories(moonstone_exoml_extract PRIVATE
    "${MOONSTONE_EXOML_LIBCLANG_INCLUDE_DIR}" "${exoml_generated}")
  target_include_directories(moonstone_exoml_extract SYSTEM PRIVATE "${MOONSTONE_EXOML_JSON_INCLUDE_DIR}")
  if(MSVC)
    target_compile_options(moonstone_exoml_extract PRIVATE /W4 /WX)
  else()
    target_compile_options(moonstone_exoml_extract PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
  if(MOONSTONE_BUILD_TOOLS)
    add_executable(exoml-bind tools/exoml-bind.cpp)
    target_link_libraries(exoml-bind PRIVATE moonstone_exoml_extract)
    if(MSVC)
      target_compile_options(exoml-bind PRIVATE /W4 /WX)
    else()
      target_compile_options(exoml-bind PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    install(TARGETS exoml-bind RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT ExoML)
  endif()
  install(TARGETS moonstone_exoml_extract EXPORT MoonstoneExoMLExtractTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT ExoML)
  install(EXPORT MoonstoneExoMLExtractTargets NAMESPACE Moonstone::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoMLExtract COMPONENT ExoML)
  configure_package_config_file(cmake/MoonstoneExoMLExtractConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLExtractConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoMLExtract)
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/MoonstoneExoMLExtractConfig.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonstoneExoMLExtract COMPONENT ExoML)
  install(FILES cmake/nlohmann-json-LICENSE.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/MoonstoneExoMLExtract COMPONENT ExoML)
endif()
if(MOONSTONE_BUILD_TESTS)
  add_library(exoml_test_fixture STATIC tests/fixture.c)
  set_target_properties(exoml_test_fixture PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
  add_executable(exoml_test_generator tests/generate.cpp)
  target_link_libraries(exoml_test_generator PRIVATE moonstone_exoml)
  set(fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/fixture")
  file(MAKE_DIRECTORY "${fixture_dir}")
  set(fixture_outputs)
  foreach(file IN ITEMS bindings.hpp checks.cpp probe.cpp manifest.json bad_signature.cpp bad_layout.cpp)
    list(APPEND fixture_outputs "${fixture_dir}/${file}")
  endforeach()
  add_custom_command(OUTPUT ${fixture_outputs}
    COMMAND exoml_test_generator "${fixture_dir}"
    DEPENDS exoml_test_generator tests/fixture.h VERBATIM)
  add_custom_target(exoml_fixture_codegen DEPENDS ${fixture_outputs})
  add_executable(exoml_test_generated tests/generated.cpp "${fixture_dir}/bindings.hpp" "${fixture_dir}/checks.cpp")
  add_dependencies(exoml_test_generated exoml_fixture_codegen)
  target_include_directories(exoml_test_generated PRIVATE "${fixture_dir}" tests)
  target_link_libraries(exoml_test_generated PRIVATE moonstone_exoml exoml_test_fixture)
  add_executable(exoml_test_probe "${fixture_dir}/probe.cpp")
  add_dependencies(exoml_test_probe exoml_fixture_codegen)
  target_include_directories(exoml_test_probe PRIVATE tests)
  target_compile_features(exoml_test_probe PRIVATE cxx_std_20)
  add_test(NAME moonstone.exoml_generated COMMAND exoml_test_generated)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(MOONSTONE_EXOML_EXTRACT)
    find_program(MOONSTONE_EXOML_CLANG_EXECUTABLE NAMES clang REQUIRED)
    execute_process(COMMAND "${MOONSTONE_EXOML_CLANG_EXECUTABLE}" -print-resource-dir
      OUTPUT_VARIABLE exoml_clang_resources OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
    add_executable(exoml_test_extractor tests/extractor.cpp)
    target_link_libraries(exoml_test_extractor PRIVATE moonstone_exoml_extract)
    target_compile_definitions(exoml_test_extractor PRIVATE
      EXOML_DISCOVERY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.h"
      EXOML_CLANG_RESOURCES="${exoml_clang_resources}")
    if(MSVC)
      target_compile_options(exoml_test_extractor PRIVATE /UNDEBUG /W4 /WX)
    else()
      target_compile_options(exoml_test_extractor PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME moonstone.exoml_extractor COMMAND exoml_test_extractor)
    set_tests_properties(moonstone.exoml_extractor PROPERTIES TIMEOUT 60)
    add_executable(exoml_test_cache_codec tests/cache_codec.cpp)
    target_link_libraries(exoml_test_cache_codec PRIVATE moonstone_exoml_extract)
    target_include_directories(exoml_test_cache_codec PRIVATE src "${MOONSTONE_EXOML_LIBCLANG_INCLUDE_DIR}")
    target_compile_definitions(exoml_test_cache_codec PRIVATE
      EXOML_DISCOVERY_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.h"
      EXOML_CLANG_RESOURCES="${exoml_clang_resources}")
    if(MSVC)
      target_compile_options(exoml_test_cache_codec PRIVATE /UNDEBUG /W4 /WX)
    else()
      target_compile_options(exoml_test_cache_codec PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME moonstone.exoml_cache_codec COMMAND exoml_test_cache_codec)
    if(Python3_Interpreter_FOUND AND MOONSTONE_BUILD_TOOLS)
      add_test(NAME moonstone.exoml_discovery_cli COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.py" $<TARGET_FILE:exoml-bind>
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.h" "${MOONSTONE_EXOML_MULIB_ROOT}"
        "${exoml_clang_resources}" "${CMAKE_C_COMPILER}")
      set_tests_properties(moonstone.exoml_discovery_cli PROPERTIES TIMEOUT 90)
      add_test(NAME moonstone.exoml_discovery_context COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery_context.py" $<TARGET_FILE:exoml-bind>
        "${exoml_clang_resources}" "${CMAKE_C_COMPILER}")
      set_tests_properties(moonstone.exoml_discovery_context PROPERTIES TIMEOUT 90)
      if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME moonstone.exoml_cache COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/cache.py" $<TARGET_FILE:exoml-bind>
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/discovery.h" "${exoml_clang_resources}"
          "${MOONSTONE_EXOML_MULIB_ROOT}")
        set_tests_properties(moonstone.exoml_cache PROPERTIES TIMEOUT 90)
      endif()
    endif()
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME moonstone.exoml_codegen COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_codegen.py"
      "${CMAKE_CXX_COMPILER}" "${CMAKE_CXX_COMPILER_ID}" "${fixture_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/tests" $<TARGET_FILE:exoml_test_probe>)
    set_tests_properties(moonstone.exoml_codegen PROPERTIES TIMEOUT 60)
    if(MOONSTONE_BUILD_TOOLS)
      add_test(NAME moonstone.exoml_cli COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.py"
        $<TARGET_FILE:exoml> "${CMAKE_CURRENT_SOURCE_DIR}/examples/fixture.exo")
      set_tests_properties(moonstone.exoml_cli PROPERTIES TIMEOUT 30)
    endif()
  endif()
  add_test(NAME moonstone.exoml_package COMMAND "${CMAKE_COMMAND}"
    "-DBUILD=${CMAKE_BINARY_DIR}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/package-test"
    "-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/consumer" "-DCXX=${CMAKE_CXX_COMPILER}"
    "-DFLAGS=${CMAKE_CXX_FLAGS}" "-DCONFIG=$<CONFIG>" "-DEXTRACT=${MOONSTONE_EXOML_EXTRACT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package.cmake")
  set_tests_properties(moonstone.exoml_package PROPERTIES TIMEOUT 60)
  if(MOONSTONE_BUILD_TOOLS)
    add_custom_command(OUTPUT "${fixture_dir}/script_bindings.hpp"
      COMMAND "${CMAKE_COMMAND}" "-DEXOML=$<TARGET_FILE:exoml>" "-DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/examples/fixture.exo"
        "-DOUTPUT=${fixture_dir}/script_bindings.hpp" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Emit.cmake"
      DEPENDS exoml examples/fixture.exo cmake/Emit.cmake VERBATIM)
    add_executable(exoml_test_script tests/script_generated.cpp "${fixture_dir}/script_bindings.hpp")
    target_include_directories(exoml_test_script PRIVATE "${fixture_dir}" tests)
    target_link_libraries(exoml_test_script PRIVATE moonstone_exoml exoml_test_fixture)
    add_test(NAME moonstone.exoml_script COMMAND exoml_test_script)
  endif()
  foreach(target IN ITEMS exoml_test_generated exoml_test_script)
    if(TARGET ${target})
      if(MSVC)
        target_compile_options(${target} PRIVATE /UNDEBUG /W4 /WX)
      else()
        target_compile_options(${target} PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
      endif()
    endif()
  endforeach()
  add_executable(exoml_test_contract tests/contract.cpp)
  target_link_libraries(exoml_test_contract PRIVATE moonstone_exoml)
  if(MSVC)
    target_compile_options(exoml_test_contract PRIVATE /UNDEBUG /W4 /WX)
  else()
    target_compile_options(exoml_test_contract PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic -Werror)
  endif()
  add_test(NAME moonstone.exoml_contract COMMAND exoml_test_contract)
  set(header_sources)
  foreach(header IN ITEMS ExoML Value AutoBind StaticFFI CDecl Extract Contract)
    set(source "${CMAKE_CURRENT_BINARY_DIR}/header-${header}.cpp")
    file(GENERATE OUTPUT "${source}" CONTENT "#include <exoml/${header}.hpp>\n")
    list(APPEND header_sources "${source}")
  endforeach()
  add_library(moonstone_exoml_header_checks OBJECT ${header_sources})
  target_link_libraries(moonstone_exoml_header_checks PRIVATE moonstone_exoml)
  if(MSVC)
    target_compile_options(moonstone_exoml_header_checks PRIVATE /W4 /WX)
  else()
    target_compile_options(moonstone_exoml_header_checks PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(moonstone_exoml_runtime PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Wpedantic;-Werror>)
  endif()
endif()
