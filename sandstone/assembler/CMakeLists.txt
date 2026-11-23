# Sandstone assembler framework: library + `sandstone-as` CLI.
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/SandASSources.cmake")
add_library(sandstone_assembler STATIC ${sandstone_assembler_sources})
add_library(Sandstone::assembler ALIAS sandstone_assembler)
set_target_properties(sandstone_assembler PROPERTIES EXPORT_NAME assembler)
target_include_directories(sandstone_assembler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sandstone>
)
target_link_libraries(sandstone_assembler PUBLIC sandstone)
libstone_warnings(sandstone_assembler)
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/LibstoneLua.cmake")
target_link_libraries(sandstone_assembler PRIVATE
  $<BUILD_INTERFACE:libstone_kaguya> $<INSTALL_INTERFACE:LibstoneLua::libstone_script_lua>)
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
install(TARGETS sandstone_assembler EXPORT SandstoneTargets
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
  set_tests_properties(sandstone-assembler-sdk PROPERTIES TIMEOUT 30)
endif()
