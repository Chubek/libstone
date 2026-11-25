cmake_minimum_required(VERSION 3.20)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(LLSynthAssembler LANGUAGES C CXX)
endif()

include(GNUInstallDirs)
if(NOT TARGET Sandstone::assembler)
  find_package(Sandstone CONFIG REQUIRED)
endif()
if(NOT TARGET Sandstone::assembler)
  message(FATAL_ERROR "Sandstone was built without its assembler SDK")
endif()

add_library(llsynth_assembler SHARED llsynth_assembler.c)
add_library(LLSynth::assembler ALIAS llsynth_assembler)
target_compile_features(llsynth_assembler PRIVATE c_std_99)
target_compile_definitions(llsynth_assembler PRIVATE LLSYNTH_ASSEMBLER_EXPORTS)
target_include_directories(llsynth_assembler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(llsynth_assembler PRIVATE Sandstone::assembler)
set_target_properties(llsynth_assembler PROPERTIES LINKER_LANGUAGE CXX)

install(TARGETS llsynth_assembler
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES llsynth_assembler.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
