# Sandstone assembler framework: library + `sandstone-as` CLI.
add_library(sandstone_assembler STATIC
  assembler.c
  parser.c
  preprocess.cpp
  directives.c
  binarizer.c
  lua.cpp
  sdk.cpp
)
target_include_directories(sandstone_assembler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sandstone>
)
target_link_libraries(sandstone_assembler PUBLIC sandstone)
libstone_warnings(sandstone_assembler)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LibstoneLua.cmake")
target_link_libraries(sandstone_assembler PRIVATE libstone_kaguya)
if(TARGET sandstone-archml)
  target_link_libraries(sandstone_assembler PRIVATE sandstone-archml)
  target_compile_definitions(sandstone_assembler PRIVATE SANDSTONE_ASM_ARCHML=1)
endif()

if(SANDSTONE_BUILD_TOOLS OR SANDSTONE_BUILD_TESTS)
  add_executable(sandstone-as driver.c)
  target_link_libraries(sandstone-as PRIVATE sandstone_assembler)
  libstone_warnings(sandstone-as)
endif()

if(SANDSTONE_BUILD_TOOLS)
  install(TARGETS sandstone-as RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(TARGETS sandstone_assembler
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES assembler.h assembler.hpp sdk.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sandstone
)
if(SANDSTONE_BUILD_TESTS)
  add_executable(sandstone-assembler-test ../tests/assembler_sdk.cpp)
  target_link_libraries(sandstone-assembler-test PRIVATE sandstone_assembler)
  if(TARGET sandstone-archml)
    target_compile_definitions(sandstone-assembler-test PRIVATE
      ARCHML_PROFILE="${CMAKE_CURRENT_SOURCE_DIR}/../archml/stdspec/amd64.aml")
  endif()
  libstone_warnings(sandstone-assembler-test)
  add_test(NAME sandstone-assembler-sdk COMMAND sandstone-assembler-test)
endif()
