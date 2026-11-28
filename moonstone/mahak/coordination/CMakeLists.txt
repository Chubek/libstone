cmake_minimum_required(VERSION 3.20)

find_package(Threads REQUIRED)

file(GLOB_RECURSE MAHAK_COORD_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(moonstone_mahak_coordination STATIC ${MAHAK_COORD_SOURCES})
target_link_libraries(moonstone_mahak_coordination PUBLIC
  Moonstone::Mahak Threads::Threads)
target_compile_features(moonstone_mahak_coordination PUBLIC cxx_std_20)
target_include_directories(moonstone_mahak_coordination PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(MSVC)
  target_compile_options(moonstone_mahak_coordination PRIVATE /W4)
else()
  target_compile_options(moonstone_mahak_coordination PRIVATE
    -Wall -Wextra -Wpedantic)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(moonstone_mahak_coordination PUBLIC rt)
endif()

if(MOONSTONE_BUILD_TOOLS)
  foreach(tool IN ITEMS broker demo status)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/${tool}.cpp")
      add_executable(mahak-coord-${tool} "tools/${tool}.cpp")
      target_link_libraries(mahak-coord-${tool} PRIVATE moonstone_mahak_coordination)
    endif()
  endforeach()
endif()

if(MOONSTONE_BUILD_TESTS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol.cpp")
  add_executable(moonstone_test_mahak_coordination tests/protocol.cpp)
  target_link_libraries(moonstone_test_mahak_coordination
    PRIVATE moonstone_mahak_coordination)
  if(MSVC)
    target_compile_options(moonstone_test_mahak_coordination PRIVATE /UNDEBUG /W4)
  else()
    target_compile_options(moonstone_test_mahak_coordination PRIVATE
      -UNDEBUG -Wall -Wextra -Wpedantic)
  endif()
  add_test(NAME moonstone.mahak.coordination COMMAND moonstone_test_mahak_coordination)
  set_tests_properties(moonstone.mahak.coordination PROPERTIES TIMEOUT 90)
endif()
