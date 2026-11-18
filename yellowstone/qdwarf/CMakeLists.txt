add_library(qdwarf STATIC qdwarf.cpp)
add_library(qdwarf::qdwarf ALIAS qdwarf)
target_compile_features(qdwarf PUBLIC cxx_std_20)
target_include_directories(qdwarf PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
# qBFD is header-only, and none of its types appear in the public API.
target_link_libraries(qdwarf PRIVATE $<BUILD_INTERFACE:qbfd::qbfd>)
set_target_properties(qdwarf PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(qdwarf_warnings target)
  target_compile_options(${target} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
  if(QOBJFILE_WERROR)
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
  endif()
endfunction()
qdwarf_warnings(qdwarf)

install(TARGETS qdwarf EXPORT qdwarfTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/qdwarf/qdwarf.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qdwarf)

if(YELLOWSTONE_BUILD_TOOLS)
  add_executable(qdwarf_cli main.cpp)
  set_target_properties(qdwarf_cli PROPERTIES OUTPUT_NAME qdwarf)
  target_link_libraries(qdwarf_cli PRIVATE qdwarf::qdwarf)
  qdwarf_warnings(qdwarf_cli)
  install(TARGETS qdwarf_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(qdwarf_tests tests/qdwarf.cpp)
  target_link_libraries(qdwarf_tests PRIVATE qdwarf::qdwarf)
  qdwarf_warnings(qdwarf_tests)
  add_test(NAME qdwarf_regressions COMMAND qdwarf_tests)
  if(YELLOWSTONE_BUILD_TOOLS)
    add_test(NAME qdwarf_cli_help COMMAND qdwarf_cli --help)
    add_test(NAME qdwarf_cli_inspection
      COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:qdwarf_cli> -DFIXTURES=$<TARGET_FILE:qdwarf_tests>
        -DINPUT=${CMAKE_CURRENT_BINARY_DIR}/qdwarf-cli-fixture.elf
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
  endif()
endif()
