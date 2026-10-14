# Sandstone spec compiler: JSON spec documents -> SymAS (.symas) banks.
#
# Layers:
#   sandstone_symspec       header-only validated spec access (Boost.JSON)
#   sandstone_spec_core     validated spec -> domsymas::Builder
#   sandstone-spec-compiler JSON / Lua source / backend module CLI
#   domsymascc              legacy DomMune-embedded CLI (main.cpp), built only
#                           when SANDSTONE_WITH_MUNE is ON and the DomMune /
#                           DomCLI sources are provided (they are not part of
#                           this repository).
#
# DSL evaluation uses an external Lua runtime and dsl.lua module. Generated
# backend modules and emitted JSON load without an interpreter.

option(SANDSTONE_WITH_MUNE "Build the legacy DomMune-embedded domsymascc CLI" OFF)

find_package(Boost CONFIG REQUIRED COMPONENTS json)
add_library(sandstone_symspec INTERFACE)
target_compile_features(sandstone_symspec INTERFACE cxx_std_20)
target_include_directories(sandstone_symspec INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(sandstone_symspec INTERFACE Boost::json)
set(SANDSTONE_DSL_DIRECTORY "" CACHE PATH "Directory containing the external dsl.lua runtime")

# ---- spec core (Symspec document -> Builder), no interpreter required ----
add_library(sandstone_spec_core STATIC
  src/spec_document.cpp
)
target_include_directories(sandstone_spec_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(sandstone_spec_core PUBLIC sandstone sandstone_symspec)
libstone_warnings(sandstone_spec_core)
# Backwards-compatible alias for the historical target name.
add_library(domsymascc_core ALIAS sandstone_spec_core)

# ---- CLI (Lua needed only for original DSL sources) ----
if(SANDSTONE_BUILD_TOOLS OR SANDSTONE_BUILD_TESTS)
  add_executable(sandstone-spec-compiler src/spec_compiler.cpp)
  target_link_libraries(sandstone-spec-compiler PRIVATE sandstone_spec_core)
  libstone_warnings(sandstone-spec-compiler)
endif()

if(SANDSTONE_BUILD_TOOLS)
  install(TARGETS sandstone-spec-compiler RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS sandstone_spec_core
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/domsymascc/domsymascc.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/domsymascc
)
install(FILES Symspec.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(SANDSTONE_BUILD_TESTS)
  add_executable(sandstone_spec_document_test tests/spec_document_test.cc)
  target_link_libraries(sandstone_spec_document_test PRIVATE sandstone_spec_core)
  add_test(NAME sandstone_spec_document COMMAND sandstone_spec_document_test)
  add_executable(sandstone_symspec_test tests/symspec_test.cc)
  target_link_libraries(sandstone_symspec_test PRIVATE sandstone_symspec)
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/symspec-test")
  find_program(SANDSTONE_LUA_EXECUTABLE NAMES lua lua5.4 lua5.3)
  set(_symspec_lua "")
  if(SANDSTONE_LUA_EXECUTABLE)
    set(_symspec_lua "${SANDSTONE_LUA_EXECUTABLE}")
  endif()
  add_test(NAME sandstone_symspec COMMAND sandstone_symspec_test
    "${CMAKE_CURRENT_SOURCE_DIR}/stdspec" "${SANDSTONE_DSL_DIRECTORY}"
    "${CMAKE_CURRENT_BINARY_DIR}/symspec-test" "${_symspec_lua}")
endif()
install(DIRECTORY stdspec/
  DESTINATION ${CMAKE_INSTALL_DATADIR}/sandstone/stdspec
  FILES_MATCHING PATTERN "*.dsymspec" PATTERN "*.lua" PATTERN "README.md"
)

# ---- legacy DomMune-embedded driver (optional, needs external sources) ----
if(SANDSTONE_WITH_MUNE)
  set(SANDSTONE_MUNE_SOURCE_DIR "${MLVM_THIRD_PARTY_DIR}/dommune" CACHE PATH "Optional DomMune sources")
  set(SANDSTONE_DOMCLI_SOURCE_DIR "${MLVM_THIRD_PARTY_DIR}/domcli" CACHE PATH "Optional DomCLI headers")
  if(NOT TARGET mune_core)
    libstone_require_files(SANDSTONE_MUNE_SOURCE_DIR CMakeLists.txt)
    add_subdirectory("${SANDSTONE_MUNE_SOURCE_DIR}"
                     "${CMAKE_CURRENT_BINARY_DIR}/dommune" EXCLUDE_FROM_ALL)
  endif()
  add_executable(domsymascc src/main.cpp)
  target_include_directories(domsymascc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SANDSTONE_DOMCLI_SOURCE_DIR}
  )
  target_link_libraries(domsymascc PRIVATE sandstone_spec_core mune_core)
  libstone_warnings(domsymascc)
  install(TARGETS domsymascc RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
