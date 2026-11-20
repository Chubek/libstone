cmake_minimum_required(VERSION 3.20)
project(Aurocks LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)

add_executable(aurocks-liblr aurocks-liblr.cpp)
set_target_properties(aurocks-liblr PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(TARGET Firestone::lr)
  target_link_libraries(aurocks-liblr PRIVATE Firestone::lr)
else()
  # liblr is header-only; building the helper does not require the rest of Firestone.
  target_include_directories(aurocks-liblr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../firestone")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(aurocks-liblr PRIVATE -Wall -Wextra -Wpedantic)
endif()
install(TARGETS aurocks-liblr RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(NOT BUILD_TESTING OR CMAKE_CROSSCOMPILING)
  return()
endif()

foreach(suite IN ITEMS protocol invalid)
  add_test(NAME "aurocks.${suite}" COMMAND "${CMAKE_COMMAND}"
    "-DHELPER=$<TARGET_FILE:aurocks-liblr>"
    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-${suite}/$<CONFIG>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/${suite}.cmake")
  set_tests_properties("aurocks.${suite}" PROPERTIES TIMEOUT 30)
endforeach()

find_package(Perl 5.10 QUIET)
if(NOT PERL_FOUND)
  return()
endif()
include(CheckLanguage)
check_language(C)
if(NOT CMAKE_C_COMPILER)
  return()
endif()
enable_language(C)

set(generator "${CMAKE_CURRENT_SOURCE_DIR}/scripts/aurocks.pl")
set(grammar "${CMAKE_CURRENT_SOURCE_DIR}/tests/matchers.g")
set(generate_script "${CMAKE_CURRENT_SOURCE_DIR}/tests/generate.cmake")
foreach(backend IN ITEMS legacy liblr)
  set(deps "${generator}" "${grammar}" "${generate_script}")
  set(helper "")
  if(backend STREQUAL "liblr")
    list(APPEND deps aurocks-liblr)
    set(helper "$<TARGET_FILE:aurocks-liblr>")
  endif()
  set(parser "${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>/${backend}.c")
  add_custom_command(OUTPUT "${parser}"
    COMMAND "${CMAKE_COMMAND}"
      "-DPERL=${PERL_EXECUTABLE}" "-DGENERATOR=${generator}" "-DGRAMMAR=${grammar}"
      "-DPARSER=${parser}" "-DHELPER=${helper}" -P "${generate_script}"
    DEPENDS ${deps}
    VERBATIM)
  add_executable("aurocks_${backend}_test" "${parser}" tests/matchers.c)
  set_target_properties("aurocks_${backend}_test" PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
  add_test(NAME "aurocks.${backend}_matchers" COMMAND "aurocks_${backend}_test")
endforeach()

add_test(NAME aurocks.backend_switch COMMAND "${CMAKE_COMMAND}"
  "-DGENERATED_DIR=${CMAKE_CURRENT_BINARY_DIR}/generated/$<CONFIG>"
  -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/backend-switch.cmake")
