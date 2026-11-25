cmake_minimum_required(VERSION 3.20)
project(LLSynthNative LANGUAGES C CXX)

# The Perl generator and portable standard libraries have no native build.
# This standalone project builds only the optional LLSynth runtime bridges.
if(NOT DEFINED CMAKE_LIBRARY_OUTPUT_DIRECTORY)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/lib")
endif()
if(NOT DEFINED CMAKE_RUNTIME_OUTPUT_DIRECTORY)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
endif()

set(llsynth_modules ipc shell egraph assembler isa flowgraph)
foreach(module IN LISTS llsynth_modules)
  string(TOUPPER "${module}" key)
  option(LLSYNTH_BUILD_${key} "Build the std.${module} native bridge" ON)
endforeach()

# Reuse an existing/installed SDK or build the relevant Sandstone libraries
# from this checkout. Keep the settings local to that dependency build.
if(LLSYNTH_BUILD_ASSEMBLER AND NOT TARGET Sandstone::assembler)
  find_package(Sandstone CONFIG QUIET)
  if(NOT TARGET Sandstone::assembler AND NOT TARGET Sandstone::sandstone)
    function(llsynth_add_sandstone)
      set(SANDSTONE_BUILD_TOOLS OFF)
      set(SANDSTONE_BUILD_TESTS OFF)
      set(SANDSTONE_BUILD_COMPILER OFF)
      set(SANDSTONE_BUILD_SANDSLAB OFF)
      set(SANDSTONE_BUILD_ARCHML "${LLSYNTH_BUILD_ISA}")
      add_subdirectory("../../sandstone" sandstone)
    endfunction()
    llsynth_add_sandstone()
  endif()
endif()

foreach(module IN LISTS llsynth_modules)
  string(TOUPPER "${module}" key)
  if(LLSYNTH_BUILD_${key})
    add_subdirectory("stdlib/${module}")
  endif()
endforeach()
