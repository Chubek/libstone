cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(LLSynthShell LANGUAGES CXX)
endif()

if(NOT UNIX)
  message(FATAL_ERROR "std.shell requires POSIX process primitives")
endif()

include(GNUInstallDirs)

if(NOT TARGET libsh)
  get_filename_component(_libshell_default
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/libglr/third_party/libshell" ABSOLUTE)
  set(LLSYNTH_LIBSHELL_SOURCE_DIR "${_libshell_default}"
    CACHE PATH "Source directory of the LibShell vendor snapshot")
  foreach(_file IN ITEMS CMakeLists.txt cli/libsh-cli.cpp
      third_party/QaMRpp/include/QaMRpp.hpp
      third_party/QaMRpp/third_party/SerdeTk/include/SerdeTk.hpp)
    if(NOT EXISTS "${LLSYNTH_LIBSHELL_SOURCE_DIR}/${_file}")
      message(FATAL_ERROR
        "std.shell: missing ${LLSYNTH_LIBSHELL_SOURCE_DIR}/${_file}. "
        "Set -DLLSYNTH_LIBSHELL_SOURCE_DIR=${_libshell_default} or point it at a complete local LibShell snapshot.")
    endif()
  endforeach()
  # Use the upstream CLI with its own parser, executor, and scripting backend.
  # Do not introduce its examples, kernels, tests, or docs into this build.
  set(LIBSH_BUILD_CLI ON)
  set(LIBSH_BUILD_TESTS OFF)
  set(LIBSH_BUILD_EXAMPLES OFF)
  set(LIBSH_BUILD_STDKERN OFF)
  set(LIBSHELL_GENERATE_DOCS OFF)
  add_subdirectory("${LLSYNTH_LIBSHELL_SOURCE_DIR}" libshell EXCLUDE_FROM_ALL)
  install(TARGETS libsh RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

add_library(llsynth_shell SHARED ShellBridge.cpp)
target_compile_features(llsynth_shell PRIVATE cxx_std_17)
# The LLSynth native declaration uses this filename on all Unix hosts.
set_target_properties(llsynth_shell PROPERTIES PREFIX "lib" SUFFIX ".so"
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
add_dependencies(llsynth_shell libsh)
install(TARGETS llsynth_shell LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
