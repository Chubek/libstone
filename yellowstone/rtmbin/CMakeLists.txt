add_library(rtmbin STATIC Image.cpp Model.cpp)
add_library(rtmbin::rtmbin ALIAS rtmbin)
target_compile_features(rtmbin PUBLIC cxx_std_20)
target_include_directories(rtmbin
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(rtmbin SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../wetstone")
# Both dependencies are header-only implementation details. No dependency
# lookup (including Wetstone's optional vendor adapters) is needed on install.
target_link_libraries(rtmbin PRIVATE $<BUILD_INTERFACE:qbfd::qbfd>)
set_target_properties(rtmbin PROPERTIES POSITION_INDEPENDENT_CODE ON)
libstone_warnings(rtmbin)
if(QOBJFILE_WERROR)
  target_compile_options(rtmbin PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

install(TARGETS rtmbin EXPORT rtmbinTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/rtmbin/rtmbin.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/rtmbin)
install(FILES README.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/rtmbin)

if(YELLOWSTONE_BUILD_TOOLS)
  add_executable(rtmbin_cli main.cpp)
  set_target_properties(rtmbin_cli PROPERTIES OUTPUT_NAME rtmbin)
  target_link_libraries(rtmbin_cli PRIVATE rtmbin::rtmbin)
  libstone_warnings(rtmbin_cli)
  install(TARGETS rtmbin_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(YELLOWSTONE_BUILD_TESTS)
  add_executable(rtmbin_tests tests/rtmbin.cpp)
  target_link_libraries(rtmbin_tests PRIVATE rtmbin::rtmbin)
  libstone_warnings(rtmbin_tests)
  add_test(NAME rtmbin_analysis COMMAND rtmbin_tests)
  if(YELLOWSTONE_BUILD_TOOLS)
    add_test(NAME rtmbin_cli_help COMMAND rtmbin_cli --help)
    add_test(NAME rtmbin_cli_reports COMMAND ${CMAKE_COMMAND}
      -DTOOL=$<TARGET_FILE:rtmbin_cli> -DFIXTURES=$<TARGET_FILE:rtmbin_tests>
      -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli-tests
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli.cmake)
  endif()
endif()
