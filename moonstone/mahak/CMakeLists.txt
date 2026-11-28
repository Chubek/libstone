cmake_minimum_required(VERSION 3.20)

option(MOONSTONE_BUILD_MAHAK_COORDINATION
  "Build Mahak development coordination tools (not linked into Mahak)" OFF)

find_package(Threads REQUIRED)

# Every implementation unit under src/ is a library member. CONFIGURE_DEPENDS
# keeps parallel contributions conflict-free: adding a file does not require
# editing a shared source list.
file(GLOB_RECURSE MAHAK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(NOT MAHAK_SOURCES)
  message(FATAL_ERROR "Mahak has no source files")
endif()

add_library(moonstone_mahak STATIC ${MAHAK_SOURCES})
add_library(Moonstone::Mahak ALIAS moonstone_mahak)
set_target_properties(moonstone_mahak PROPERTIES EXPORT_NAME Mahak)
target_compile_features(moonstone_mahak PUBLIC cxx_std_20)
target_link_libraries(moonstone_mahak PUBLIC moonstone_core Threads::Threads)
target_include_directories(moonstone_mahak PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(moonstone_mahak PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(MSVC)
  target_compile_options(moonstone_mahak PRIVATE /W4)
else()
  target_compile_options(moonstone_mahak PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(moonstone_mahak PUBLIC ${CMAKE_DL_LIBS} rt)
endif()

if(MOONSTONE_BUILD_TOOLS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/mahak.cpp")
  add_executable(mahak tools/mahak.cpp)
  target_link_libraries(mahak PRIVATE Moonstone::Mahak)
  install(TARGETS mahak RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(MOONSTONE_BUILD_TESTS)
  enable_testing()
  file(GLOB MAHAK_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp")
  foreach(source IN LISTS MAHAK_TEST_SOURCES)
    get_filename_component(stem "${source}" NAME_WE)
    if(stem MATCHES "^fixture_")
      continue()
    endif()
    set(target "moonstone_test_mahak_${stem}")
    add_executable(${target} "${source}")
    target_link_libraries(${target} PRIVATE Moonstone::Mahak)
    if(MSVC)
      target_compile_options(${target} PRIVATE /UNDEBUG /W4)
    else()
      target_compile_options(${target} PRIVATE -UNDEBUG -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME moonstone.mahak.${stem} COMMAND ${target})
    set_tests_properties(moonstone.mahak.${stem} PROPERTIES TIMEOUT 120)
  endforeach()

  # The suite manifests are validated and planned by a test that does not need
  # the compilers they name.
  if(TARGET moonstone_test_mahak_suites)
    target_compile_definitions(moonstone_test_mahak_suites PRIVATE
      MAHAK_SUITE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/suites")
  endif()

  # Strict C11 consumer for the embedding ABI.
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/c_api.c")
    add_executable(moonstone_test_mahak_c_api tests/c_api.c)
    target_link_libraries(moonstone_test_mahak_c_api PRIVATE Moonstone::Mahak)
    set_target_properties(moonstone_test_mahak_c_api PROPERTIES
      C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO LINKER_LANGUAGE CXX)
    add_test(NAME moonstone.mahak.c_api COMMAND moonstone_test_mahak_c_api)
  endif()

  # Each public header is a separate translation unit. An umbrella header must
  # not hide a missing dependency.
  file(GLOB MAHAK_PUBLIC_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/mahak/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/mahak/*.h")
  set(MAHAK_HEADER_SOURCES)
  foreach(header IN LISTS MAHAK_PUBLIC_HEADERS)
    get_filename_component(name "${header}" NAME)
    string(MAKE_C_IDENTIFIER "${name}" identifier)
    set(wrapper "${CMAKE_CURRENT_BINARY_DIR}/header-checks/${identifier}.cpp")
    file(GENERATE OUTPUT "${wrapper}" CONTENT "#include \"mahak/${name}\"\n")
    list(APPEND MAHAK_HEADER_SOURCES "${wrapper}")
  endforeach()
  if(MAHAK_HEADER_SOURCES)
    add_library(moonstone_mahak_header_checks OBJECT ${MAHAK_HEADER_SOURCES})
    target_link_libraries(moonstone_mahak_header_checks PRIVATE Moonstone::Mahak)
    if(MSVC)
      target_compile_options(moonstone_mahak_header_checks PRIVATE /W4 /WX)
    else()
      target_compile_options(moonstone_mahak_header_checks PRIVATE
        -Wall -Wextra -Wpedantic -Werror)
    endif()
  endif()
endif()

if(MOONSTONE_BUILD_MAHAK_COORDINATION)
  add_subdirectory(coordination)
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
