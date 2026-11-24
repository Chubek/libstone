# Sandstone — SymAS-driven assembler framework.
#
# Layout:
#   decoder/    SymAS databank library (lib `sandstone`, magic 0xCEEB09)
#   include/    public C/C++ headers (sandstone.h/.hpp + domsymas shims)
#   compiler/   spec compiler: JSON documents -> .symas banks
#   dsymspec/   LLSynth spec compiler: .dsymspec -> C11 macro includes
#   assembler/  assembler framework + `sandstone-as` CLI
#   bindings/   SWIG interface (built via bindings/generate-bindings.sh)
#   isa/        small `.isa` sources + loaders (Python/C++)
#   docs/       Doxygen + prose documentation
#   scripts/    spec-compiler.py (.dsymspec -> .symas driver) and helpers
cmake_minimum_required(VERSION 3.20)

project(Sandstone VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(../cmake/LibstoneBuild.cmake)
include(CMakePackageConfigHelpers)
set(CMAKE_CXX_EXTENSIONS OFF)
option(SANDSTONE_BUILD_TOOLS "Build Sandstone command-line tools" "${LIBSTONE_BUILD_TOOLS}")

option(SANDSTONE_BUILD_COMPILER "Build the spec compiler" ON)
option(SANDSTONE_BUILD_DSYMSPEC "Generate standalone DSymspec assembler includes" OFF)
option(SANDSTONE_BUILD_ASSEMBLER "Build the assembler framework and CLI" ON)
option(SANDSTONE_BUILD_TESTS "Build and register tests" "${LIBSTONE_BUILD_TESTS}")
option(SANDSTONE_BUILD_DOCS "Build documentation (needs Doxygen)" OFF)
option(SANDSTONE_BUILD_SANDSLAB "Build the machine-near C++ code generator and Forth example" ON)
option(SANDSTONE_BUILD_ARCHML "Build the ArchML architecture compiler" ON)

if(SANDSTONE_BUILD_TESTS)
  enable_testing()
endif()
if(SANDSTONE_BUILD_SANDSLAB)
  add_subdirectory(sandslab)
endif()
if(SANDSTONE_BUILD_ARCHML AND NOT TARGET Sandstone::ArchML)
  add_subdirectory(archml)
endif()

# ---- core databank library ----
include(cmake/SandASSources.cmake)
add_library(sandstone STATIC ${sandstone_reader_sources})
# Historical target name kept as an alias.
add_library(domsymas ALIAS sandstone)
add_library(Sandstone::sandstone ALIAS sandstone)

target_include_directories(sandstone
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/decoder
)
target_compile_features(sandstone PUBLIC cxx_std_20)
libstone_warnings(sandstone)

# zlib carries the default SymAS payload compression; require it so the
# produced banks stay readable everywhere.
find_package(ZLIB REQUIRED)
target_link_libraries(sandstone PUBLIC ZLIB::ZLIB)

# zstd is optional: without it, zstd banks fail with a clean diagnostic
# instead of silently producing unreadable files.
find_library(SANDSTONE_ZSTD_LIB zstd)
find_path(SANDSTONE_ZSTD_INCLUDE zstd.h)
if(SANDSTONE_ZSTD_LIB AND SANDSTONE_ZSTD_INCLUDE)
  target_link_libraries(sandstone PUBLIC ${SANDSTONE_ZSTD_LIB})
  target_include_directories(sandstone PRIVATE ${SANDSTONE_ZSTD_INCLUDE})
  target_compile_definitions(sandstone PUBLIC SANDSTONE_HAS_ZSTD=1)
endif()

find_package(Threads QUIET)
if(Threads_FOUND)
  target_link_libraries(sandstone PUBLIC Threads::Threads)
endif()

install(TARGETS sandstone EXPORT SandstoneTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/sandstone.h include/sandstone.hpp
              include/domsymas.h include/domsymas.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY isa/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/sandstone/isa
  FILES_MATCHING PATTERN "*.isa" PATTERN "*.py" PATTERN "*.hpp"
                 PATTERN "*.md")

if(SANDSTONE_BUILD_COMPILER)
  add_subdirectory(compiler)
endif()

if(SANDSTONE_BUILD_DSYMSPEC)
  add_subdirectory(dsymspec)
endif()

if(SANDSTONE_BUILD_ASSEMBLER)
  add_subdirectory(assembler)
endif()
install(EXPORT SandstoneTargets NAMESPACE Sandstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sandstone)
configure_package_config_file(cmake/SandstoneConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/SandstoneConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sandstone)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/SandstoneConfig.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Sandstone)

if(SANDSTONE_BUILD_DOCS)
  add_subdirectory(docs)
endif()

# SWIG bindings are script-driven (see bindings/generate-bindings.sh);
# expose them as a convenience target when SWIG is present.
find_program(SWIG_EXECUTABLE swig)
if(SWIG_EXECUTABLE)
  add_custom_target(sandstone-bindings
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/bindings/generate-bindings.sh
            --all --out ${CMAKE_CURRENT_BINARY_DIR}/bindings
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    COMMENT "Generating Sandstone SWIG bindings"
  )
endif()

if(SANDSTONE_BUILD_TESTS)
  add_executable(sandstone_smoke tests/smoke.cpp)
  target_link_libraries(sandstone_smoke PRIVATE sandstone)
  libstone_warnings(sandstone_smoke)
  add_test(NAME sandstone_smoke COMMAND sandstone_smoke)

  # End-to-end: compile a shipped spec to .symas with the scripted
  # pipeline, then assemble a sample with the freshly built CLI.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND AND SANDSTONE_BUILD_COMPILER
     AND SANDSTONE_BUILD_ASSEMBLER)
    add_test(NAME sandstone_spec_pipeline
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pipeline.py
              --build-dir ${CMAKE_CURRENT_BINARY_DIR}
              --source-dir ${CMAKE_CURRENT_SOURCE_DIR}
    )
    add_test(NAME sandstone_native_objects COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_objects.py --build-dir ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(sandstone_native_objects PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME sandstone_rv_gas COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/tests/rv_gas.py --build-dir ${CMAKE_CURRENT_BINARY_DIR})
    if(SANDSTONE_DSL_DIRECTORY)
      set_tests_properties(sandstone_spec_pipeline sandstone_native_objects sandstone_rv_gas PROPERTIES
        ENVIRONMENT "SANDSTONE_DSL_DIRECTORY=${SANDSTONE_DSL_DIRECTORY}")
    endif()
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME sandstone_simple_isa
      COMMAND ${Python3_EXECUTABLE} -m unittest discover
              -s ${CMAKE_CURRENT_SOURCE_DIR}/tests -p "test_simple_isa.py"
    )
  endif()
  # DSLtk is supplied by the shared MetaTk checkout.
  if(EXISTS "${MLVM_METATK_SOURCE_DIR}/DSLtk/DSLtk.hpp")
    add_executable(sandstone_simple_isa_cpp tests/simple_isa.cpp)
    target_include_directories(sandstone_simple_isa_cpp PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/isa "${MLVM_METATK_SOURCE_DIR}/DSLtk")
    target_compile_definitions(sandstone_simple_isa_cpp PRIVATE
      SIMPLE_ISA_TEST_DIR="${CMAKE_CURRENT_SOURCE_DIR}/isa")
    add_test(NAME sandstone_simple_isa_cpp COMMAND sandstone_simple_isa_cpp)
  endif()
endif()
