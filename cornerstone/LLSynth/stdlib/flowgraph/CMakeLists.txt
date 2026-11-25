cmake_minimum_required(VERSION 3.18)
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(LLSynthFlowgraph LANGUAGES CXX)
endif()

set(LLSYNTH_WETSTONE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../../wetstone"
    CACHE PATH "Directory containing the Wetstone dataflow-analysis headers")
if(NOT EXISTS "${LLSYNTH_WETSTONE_DIR}/dataflow-analysis/DataFlowLattice.hpp")
    message(FATAL_ERROR "Set LLSYNTH_WETSTONE_DIR to the Wetstone source directory")
endif()

add_library(llsynth_flowgraph SHARED Flowgraph.cpp)
target_compile_features(llsynth_flowgraph PRIVATE cxx_std_20)
target_include_directories(llsynth_flowgraph SYSTEM PRIVATE "${LLSYNTH_WETSTONE_DIR}")
set_target_properties(llsynth_flowgraph PROPERTIES CXX_EXTENSIONS OFF)
include(GNUInstallDirs)
install(TARGETS llsynth_flowgraph
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
