cmake_minimum_required(VERSION 3.20)
project(LLSynthInteropSandstone LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
# A small, real SDK build for this integration suite. No replacement encoder,
# mock assembler, installed compiler framework, or network downloads.
get_filename_component(REPO "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(VENDOR_ROOT "${REPO}" CACHE PATH "Root containing third_party and cmake")
set(SAND "${REPO}/sandstone")
file(GLOB DECODER "${SAND}/decoder/*.cpp")
file(GLOB LUA "${VENDOR_ROOT}/third_party/lua/*.c")
list(FILTER LUA EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
if(NOT LUA)
  message(FATAL_ERROR "Lua sources missing; set VENDOR_ROOT to the checkout/vendor snapshot root")
endif()
add_library(lua STATIC ${LUA})
target_include_directories(lua PUBLIC "${VENDOR_ROOT}/third_party/lua")
add_library(sandstone STATIC "${SAND}/decoder/domsymas.c" ${DECODER})
target_include_directories(sandstone PUBLIC "${SAND}/include" PRIVATE "${SAND}/decoder")
find_package(ZLIB REQUIRED)
target_link_libraries(sandstone PUBLIC ZLIB::ZLIB)
add_library(sandstone_assembler STATIC
  "${SAND}/assembler/assembler.c" "${SAND}/assembler/parser.c"
  "${SAND}/assembler/preprocess.cpp" "${SAND}/assembler/directives.c"
  "${SAND}/assembler/binarizer.c" "${SAND}/assembler/lua.cpp"
  "${SAND}/assembler/sdk.cpp")
target_include_directories(sandstone_assembler PUBLIC "${SAND}/assembler"
  PRIVATE "${VENDOR_ROOT}/third_party/kaguya/include" "${VENDOR_ROOT}/cmake")
target_link_libraries(sandstone_assembler PUBLIC sandstone lua ${CMAKE_DL_LIBS} m)
add_executable(sandstone-as "${SAND}/assembler/driver.c")
target_link_libraries(sandstone-as PRIVATE sandstone_assembler)
add_executable(mkbank mkbank.cpp)
target_link_libraries(mkbank PRIVATE sandstone)
