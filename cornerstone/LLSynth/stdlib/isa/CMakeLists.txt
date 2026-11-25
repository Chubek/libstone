cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(LLSynthISA LANGUAGES C CXX)
endif()
include(GNUInstallDirs)

if(NOT TARGET Sandstone::ArchML)
  find_package(ArchML CONFIG QUIET)
endif()
if(NOT TARGET Sandstone::ArchML)
  if(NOT COMMAND libstone_warnings)
    include(../../../../cmake/LibstoneBuild.cmake)
  endif()
  add_subdirectory(../../../../sandstone/archml "${CMAKE_CURRENT_BINARY_DIR}/archml")
endif()

add_library(llsynth_isa SHARED ISA.cpp)
add_library(LLSynth::ISA ALIAS llsynth_isa)
target_link_libraries(llsynth_isa PRIVATE Sandstone::ArchML)
target_compile_features(llsynth_isa PRIVATE cxx_std_20)
if(COMMAND libstone_warnings)
  libstone_warnings(llsynth_isa)
endif()
# Use the name in std.isa's .extern c declarations on all platforms.
set_target_properties(llsynth_isa PROPERTIES
  PREFIX "lib" SUFFIX ".so" WINDOWS_EXPORT_ALL_SYMBOLS ON)
install(TARGETS llsynth_isa
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
