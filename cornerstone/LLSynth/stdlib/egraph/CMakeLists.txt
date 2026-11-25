cmake_minimum_required(VERSION 3.18)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(LLSynthEGraph LANGUAGES CXX)
endif()

set(LLSYNTH_EQUINOX_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../third_party/equinox-ng"
    CACHE PATH "Directory containing the vendored EquinoxNG.hpp")
if(NOT EXISTS "${LLSYNTH_EQUINOX_DIR}/EquinoxNG.hpp")
    message(FATAL_ERROR "Set LLSYNTH_EQUINOX_DIR to the EquinoxNG source directory")
endif()

add_library(llsynth_egraph SHARED bridge.cpp)
add_library(LLSynth::egraph ALIAS llsynth_egraph)
target_compile_features(llsynth_egraph PRIVATE cxx_std_20)
target_include_directories(llsynth_egraph SYSTEM PRIVATE "${LLSYNTH_EQUINOX_DIR}")
set_target_properties(llsynth_egraph PROPERTIES CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
install(TARGETS llsynth_egraph
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
