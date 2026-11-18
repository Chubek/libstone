add_library(binrev STATIC BinRev.cpp)
add_library(binrev::binrev ALIAS binrev)
target_compile_features(binrev PUBLIC cxx_std_20)
target_link_libraries(binrev PUBLIC qbfd::qbfd)
target_include_directories(binrev PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(binrev PROPERTIES POSITION_INDEPENDENT_CODE ON)

function(binrev_warnings target)
  libstone_warnings(${target})
  if(QOBJFILE_WERROR)
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
  endif()
endfunction()
binrev_warnings(binrev)

install(TARGETS binrev EXPORT binrevTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/binrev/BinRev.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/binrev)

if(YELLOWSTONE_BUILD_TOOLS)
  add_executable(binrev_cli main.cpp)
  set_target_properties(binrev_cli PROPERTIES OUTPUT_NAME binrev)
  target_link_libraries(binrev_cli PRIVATE binrev::binrev)
  target_include_directories(binrev_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../common)
  binrev_warnings(binrev_cli)
  install(TARGETS binrev_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(binrev_tests tests/BinRevTest.cpp)
  target_link_libraries(binrev_tests PRIVATE binrev::binrev)
  binrev_warnings(binrev_tests)
  add_test(NAME binrev_inspection COMMAND binrev_tests)
  if(YELLOWSTONE_BUILD_TOOLS)
    add_test(NAME binrev_cli
      COMMAND ${CMAKE_COMMAND}
        -DCLI=$<TARGET_FILE:binrev_cli>
        -DFIXTURES=$<TARGET_FILE:binrev_tests>
        -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-fixtures
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
  endif()
endif()
