cmake_minimum_required(VERSION 3.24)
project(archvirt VERSION 0.3.0 LANGUAGES C CXX)

include(GNUInstallDirs)

# Locate installed libraries and data relative to the executable/shared library.
file(RELATIVE_PATH ARCHVIRT_BIN_TO_LIB "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
file(RELATIVE_PATH ARCHVIRT_BIN_TO_DATA "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/archvirt")
file(RELATIVE_PATH ARCHVIRT_LIB_TO_DATA "${CMAKE_INSTALL_FULL_LIBDIR}" "${CMAKE_INSTALL_FULL_DATADIR}/archvirt")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DOMVIRT_THIRD_PARTY_DIR "${PROJECT_SOURCE_DIR}/third_party")
set(LIBSTONE_DIR "${DOMVIRT_THIRD_PARTY_DIR}/libstone")
set(ARCHML_DIR "${LIBSTONE_DIR}/sandstone/archml")
include("${LIBSTONE_DIR}/cmake/LibstoneBuild.cmake")
set(SANDSTONE_BUILD_TOOLS ON)
set(SANDSTONE_BUILD_TESTS OFF)
add_subdirectory("${ARCHML_DIR}" archml)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

option(DOMVIRT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(DOMVIRT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(DOMVIRT_ENABLE_LTO "Enable Link-Time Optimization" OFF)
option(DOMVIRT_BUILD_TESTS "Build test suite" ON)
option(DOMVIRT_BUILD_BENCHMARKS "Build benchmark harness" OFF)
option(DOMWEAVE_EMBED_DOMVIRT "Embed DomIOS and Domix images into the Domvirt binary" ON)

function(domvirt_set_compiler_flags TARGET)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${TARGET} PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wno-unused-parameter -Wno-missing-field-initializers
    )
    if(DOMVIRT_ENABLE_ASAN)
      target_compile_options(${TARGET} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
      target_link_options(${TARGET} PRIVATE -fsanitize=address)
    endif()
    if(DOMVIRT_ENABLE_UBSAN)
      target_compile_options(${TARGET} PRIVATE -fsanitize=undefined)
      target_link_options(${TARGET} PRIVATE -fsanitize=undefined)
    endif()
    if(DOMVIRT_ENABLE_LTO)
      target_compile_options(${TARGET} PRIVATE -flto -fno-semantic-interposition)
      target_link_options(${TARGET} PRIVATE -flto)
    endif()
  endif()
endfunction()

function(domvirt_set_release_flags TARGET)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${TARGET} PRIVATE $<$<CONFIG:Release>:-O3>)
  endif()
endfunction()

function(domvirt_set_debug_flags TARGET)
  if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${TARGET} PRIVATE -g3 -O0)
  endif()
endfunction()

if(NOT TARGET tomlplusplus)
  add_library(tomlplusplus INTERFACE)
  target_include_directories(tomlplusplus INTERFACE
      "${LIBSTONE_DIR}/third_party/metatk/third_party/tomlplusplus/include")
endif()

if(NOT TARGET sol2)
  add_library(sol2 INTERFACE)
  target_include_directories(sol2 INTERFACE
      "${LIBSTONE_DIR}/third_party/jiterati/third_party/sol2/include")
endif()

if(NOT TARGET dynalo)
  add_library(dynalo INTERFACE)
  target_include_directories(dynalo INTERFACE
      "${LIBSTONE_DIR}/third_party/libglr/third_party/dynalo/include")
  if(UNIX)
    target_link_libraries(dynalo INTERFACE dl)
  elseif(WIN32)
    target_link_libraries(dynalo INTERFACE kernel32)
  endif()
endif()

if(NOT TARGET nlohmann_json)
  add_library(nlohmann_json INTERFACE)
  target_include_directories(nlohmann_json INTERFACE
      "${LIBSTONE_DIR}/third_party/libglr/third_party/libshell/third_party/taskflow/3rd-party")
endif()

pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
set(IMGUI_DIR "${DOMVIRT_THIRD_PARTY_DIR}/imgui")
add_library(archvirt_imgui STATIC
    "${IMGUI_DIR}/imgui.cpp" "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp" "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl2.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdlrenderer2.cpp")
target_include_directories(archvirt_imgui PUBLIC "${IMGUI_DIR}" "${IMGUI_DIR}/backends")
target_link_libraries(archvirt_imgui PUBLIC PkgConfig::SDL2)

if(NOT TARGET Zydis::Zydis)
  find_package(zydis CONFIG REQUIRED)
endif()

if(NOT TARGET lualib)
  pkg_check_modules(LUA54 REQUIRED IMPORTED_TARGET lua54)
  add_library(lualib INTERFACE)
  target_link_libraries(lualib INTERFACE PkgConfig::LUA54)
endif()

# dompybind's CMake package expects the canonical FindPython targets
# (Python::Module/Python::Python), so discover Python through FindPython rather
# than the legacy FindPython3 module.
find_package(Python 3.10 REQUIRED COMPONENTS Interpreter Development Development.Module)

if(NOT TARGET bindlets)
  add_subdirectory(bindlet EXCLUDE_FROM_ALL)
endif()

add_subdirectory(domios)
add_subdirectory(domix)
add_subdirectory(bios)
add_subdirectory(src)

set_target_properties(domvirt_cli domvirt_shell PROPERTIES
    INSTALL_RPATH "$ORIGIN/${ARCHVIRT_BIN_TO_LIB}")
set_target_properties(domvirt PROPERTIES INSTALL_RPATH "$ORIGIN")
target_compile_definitions(domvirt_core PRIVATE
    ARCHVIRT_BIN_TO_DATA="${ARCHVIRT_BIN_TO_DATA}"
    ARCHVIRT_LIB_TO_DATA="${ARCHVIRT_LIB_TO_DATA}")
target_link_libraries(domvirt_core PRIVATE ${CMAKE_DL_LIBS})

install(TARGETS domvirt_cli domvirt_shell domvirt
    RUNTIME_DEPENDENCY_SET archvirt_runtime
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
# Bundle shared dependencies (SDL2, Lua, Python, Zydis, etc.), but use the
# destination host's C runtime and dynamic loader.
install(RUNTIME_DEPENDENCY_SET archvirt_runtime
    PRE_EXCLUDE_REGEXES "^linux-vdso" "^ld-linux" "^ld64"
    POST_EXCLUDE_REGEXES "/lib(c|m|pthread|dl|rt|resolv)\\.so(\\..*)?$"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES include/archvirt.h include/archvirt_boot.h include/domvirt_types.hpp
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PROJECT_BINARY_DIR}/domios/domios.img"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/archvirt/domios")
install(FILES "${PROJECT_BINARY_DIR}/bios/archvirt-bios.bin"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/archvirt/bios")
foreach(ARCH IN ITEMS x86_64 aarch64 riscv64)
    install(FILES "${PROJECT_BINARY_DIR}/domix/${ARCH}/domix.img"
                  "${PROJECT_BINARY_DIR}/domix/${ARCH}/kernel"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/archvirt/domix/${ARCH}")
endforeach()
install(DIRECTORY "${PROJECT_BINARY_DIR}/src/generated/machines/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/archvirt/machines"
    FILES_MATCHING PATTERN "*.aml")
install(FILES "${IMGUI_DIR}/misc/fonts/Cousine-Regular.ttf"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/archvirt/fonts")

if(DOMVIRT_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
