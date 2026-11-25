# Shared by standalone component builds and add_subdirectory(libstone).
include("${CMAKE_CURRENT_LIST_DIR}/MLVMDependencies.cmake")
include(GNUInstallDirs)
include(CTest)

get_filename_component(LIBSTONE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(LIBSTONE_SOURCE_DIR "${LIBSTONE_SOURCE_DIR}" CACHE PATH
  "Root of the separated libstone source tree")

set(_libstone_tests_default "${BUILD_TESTING}")
set(_libstone_tools_default ON)
set(_libstone_warnings_default ON)
if(DEFINED MLVM_BUILD_TESTS)
  set(_libstone_tests_default "${MLVM_BUILD_TESTS}")
endif()
if(DEFINED MLVM_BUILD_TOOLS)
  set(_libstone_tools_default "${MLVM_BUILD_TOOLS}")
endif()
if(DEFINED MLVM_ENABLE_WARNINGS)
  set(_libstone_warnings_default "${MLVM_ENABLE_WARNINGS}")
endif()
option(LIBSTONE_BUILD_TESTS "Build libstone tests" "${_libstone_tests_default}")
option(LIBSTONE_BUILD_TOOLS "Build libstone command-line tools and examples" "${_libstone_tools_default}")
option(LIBSTONE_ENABLE_WARNINGS "Enable libstone compiler warnings" "${_libstone_warnings_default}")
set(LIBSTONE_AUROCKS_EXECUTABLE "" CACHE FILEPATH "Prebuilt host Aurocks parser generator")

function(libstone_aurocks_command command dependency)
  if(LIBSTONE_AUROCKS_EXECUTABLE)
    if(NOT EXISTS "${LIBSTONE_AUROCKS_EXECUTABLE}")
      message(FATAL_ERROR "Host Aurocks not found: ${LIBSTONE_AUROCKS_EXECUTABLE}")
    endif()
    set(_cmd "${LIBSTONE_AUROCKS_EXECUTABLE}")
    set(_dep "${LIBSTONE_AUROCKS_EXECUTABLE}")
  else()
    if(CMAKE_CROSSCOMPILING)
      message(FATAL_ERROR "Set LIBSTONE_AUROCKS_EXECUTABLE to a host Aurocks when cross-compiling")
    endif()
    if(NOT TARGET aurocks)
      if(TARGET aurocks_compiler)
        message(FATAL_ERROR "Enable the Aurocks CLI or set LIBSTONE_AUROCKS_EXECUTABLE")
      endif()
      set(LIBSTONE_BUILD_TOOLS ON)
      set(LIBSTONE_BUILD_TESTS OFF)
      add_subdirectory("${LIBSTONE_SOURCE_DIR}/cornerstone/Aurocks"
        "${CMAKE_BINARY_DIR}/libstone-host-aurocks" EXCLUDE_FROM_ALL)
    endif()
    set(_cmd "$<TARGET_FILE:aurocks>")
    set(_dep aurocks)
  endif()
  set(${command} "${_cmd}" PARENT_SCOPE)
  set(${dependency} "${_dep}" PARENT_SCOPE)
endfunction()

function(libstone_require_files source_dir)
  foreach(file IN LISTS ARGN)
    if(NOT EXISTS "${${source_dir}}/${file}")
      message(FATAL_ERROR
        "Missing ${${source_dir}}/${file}. Populate libstone/third_party, "
        "or set ${source_dir} (dependency root: LIBSTONE_THIRD_PARTY_DIR).")
    endif()
  endforeach()
endfunction()

function(libstone_warnings target)
  if(LIBSTONE_ENABLE_WARNINGS)
    target_compile_options(${target} PRIVATE
      $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
      $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
      $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
      $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
  endif()
endfunction()
