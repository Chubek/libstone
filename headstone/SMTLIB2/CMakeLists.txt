cmake_minimum_required(VERSION 3.20)
project(SMTLIB2 VERSION 0.1.0 LANGUAGES CXX)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  include(CTest)
endif()
add_library(smtlib2 INTERFACE)
add_library(Headstone::SMTLIB2 ALIAS smtlib2)
set_target_properties(smtlib2 PROPERTIES EXPORT_NAME SMTLIB2)
target_compile_features(smtlib2 INTERFACE cxx_std_17)
target_include_directories(smtlib2 INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS smtlib2 EXPORT SMTLIB2Targets)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SMTLIB2Targets NAMESPACE Headstone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SMTLIB2)
configure_package_config_file(cmake/SMTLIB2Config.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2Config.cmake INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SMTLIB2)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2ConfigVersion.cmake VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2Config.cmake ${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2ConfigVersion.cmake DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SMTLIB2)
configure_file(cmake/SMTLIB2.pc.in ${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SMTLIB2.pc DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)
option(SMTLIB2_BUILD_EXAMPLES "Build SMTLIB2 examples" ON)
if(SMTLIB2_BUILD_EXAMPLES)
  add_executable(smtlib2_example examples/boolean.cpp)
  target_link_libraries(smtlib2_example PRIVATE Headstone::SMTLIB2)
endif()
if(BUILD_TESTING)
  find_package(Python3 QUIET COMPONENTS Interpreter)
  if(Python3_FOUND AND UNIX AND NOT CMAKE_CROSSCOMPILING AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_test(NAME smtlib2_installed_frontends
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/frontends.py
        ${CMAKE_COMMAND} ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CXX_COMPILER})
  endif()
  add_executable(smtlib2_tests tests/test.cpp tests/other.cpp)
  target_link_libraries(smtlib2_tests PRIVATE Headstone::SMTLIB2)
  add_test(NAME smtlib2_tests COMMAND smtlib2_tests)
  foreach(suite IN ITEMS theory api bitvector solver incremental_sat protocol syntax session session_incremental session_bitvector)
    add_executable(smtlib2_${suite}_tests tests/${suite}.cpp)
    target_link_libraries(smtlib2_${suite}_tests PRIVATE Headstone::SMTLIB2)
    add_test(NAME smtlib2_${suite}_tests COMMAND smtlib2_${suite}_tests)
  endforeach()
endif()
# Regeneration is opt-in; the checked-in result is sufficient for consumers.
set(SMTLIB2_AMALGAM8_EXECUTABLE "" CACHE FILEPATH "Path to the Amalgam8 executable")
set(SMTLIB2_LIBSTONE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Libstone source root for regeneration")
if(SMTLIB2_AMALGAM8_EXECUTABLE)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_target(smtlib2_regenerate_firestone
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/amalgam8
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_SOURCE_DIR}/amalgam8/Firestone.am8 ${CMAKE_CURRENT_BINARY_DIR}/amalgam8/Firestone.am8
    COMMAND ${SMTLIB2_AMALGAM8_EXECUTABLE} --api --bundle --include-root ${SMTLIB2_LIBSTONE_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/amalgam8/Firestone.am8
    COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/amalgam8/Firestone.hpp ${CMAKE_CURRENT_SOURCE_DIR}/include/smtlib2/generated_firestone.hpp
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/amalgam8/extract_frontend.py
      ${SMTLIB2_LIBSTONE_ROOT} ${CMAKE_CURRENT_BINARY_DIR}/amalgam8/frontend.hpp
    COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/amalgam8/frontend.hpp ${CMAKE_CURRENT_SOURCE_DIR}/include/smtlib2/generated_frontend.hpp
    COMMENT "Regenerate and verify the Firestone snapshots" VERBATIM)
endif()
