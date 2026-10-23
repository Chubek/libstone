cmake_minimum_required(VERSION 3.20)
project(qBFD LANGUAGES C CXX)

# qBFD builds on its own, so the qDSL lookup lives here as well as in the
# parent project: `cmake -S libstone/yellowstone/qbfd -B build` also works.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(QobjDSL)

add_library(qbfd INTERFACE)
add_library(qbfd::qbfd ALIAS qbfd)
target_compile_features(qbfd INTERFACE cxx_std_20)
target_include_directories(qbfd INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbfd INTERFACE qobjfile::qdsl)
message(STATUS "qBFD: qDSL headers from ${qobjfile_qdsl_SOURCE}")

if(DEFINED YELLOWSTONE_BUILD_TESTS)
  set(_qbfd_tests_default "${YELLOWSTONE_BUILD_TESTS}")
else()
  set(_qbfd_tests_default "${LIBSTONE_BUILD_TESTS}")
endif()

option(QBFD_BUILD_TESTS "Build qBFD regression tests" "${_qbfd_tests_default}")
option(QBFD_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)

# Compiled application ABI. Its installed headers do not depend on qBFD or DSLtk.
option(ARCHBFD_BUILD_SHARED "Build ArchBFD as a shared library" ON)
option(ARCHBFD_BUILD_EXAMPLES "Build ArchBFD C and C++ application examples" OFF)
option(ARCHBFD_BUILD_TESTS "Build ArchBFD ABI regression tests" "${QBFD_BUILD_TESTS}")
if(ARCHBFD_BUILD_SHARED)
  set(_archbfd_kind SHARED)
else()
  set(_archbfd_kind STATIC)
endif()
add_library(archbfd ${_archbfd_kind} BFDBinABI.c BFDBinBridge.cpp)
add_library(ArchBFD::archbfd ALIAS archbfd)
target_compile_features(archbfd PRIVATE c_std_11 cxx_std_20)
target_link_libraries(archbfd PRIVATE $<BUILD_INTERFACE:qbfd::qbfd>)
target_include_directories(archbfd PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/archbfd>)
set_target_properties(archbfd PROPERTIES VERSION 1.0.0 SOVERSION 1
  C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
if(ARCHBFD_BUILD_SHARED)
  target_compile_definitions(archbfd PUBLIC ARCHBFD_SHARED PRIVATE ARCHBFD_BUILDING)
endif()
target_compile_options(archbfd PRIVATE
  $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>
  $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QBFD_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(archbfd PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(archbfd PUBLIC -fsanitize=address,undefined)
endif()
add_library(archbfd_cpp INTERFACE)
add_library(ArchBFD::cpp ALIAS archbfd_cpp)
set_target_properties(archbfd_cpp PROPERTIES EXPORT_NAME cpp)
target_compile_features(archbfd_cpp INTERFACE cxx_std_20)
target_link_libraries(archbfd_cpp INTERFACE archbfd)

if(ARCHBFD_BUILD_EXAMPLES)
  add_executable(archbfd-sections examples/archbfd-sections.c)
  target_link_libraries(archbfd-sections PRIVATE ArchBFD::archbfd)
  # A static ArchBFD contains C++ readers, even when the consumer is written in C.
  set_target_properties(archbfd-sections PROPERTIES LINKER_LANGUAGE CXX)
  add_executable(archbfd-inspect examples/archbfd-inspect.cpp)
  target_link_libraries(archbfd-inspect PRIVATE ArchBFD::cpp)
endif()
if(ARCHBFD_BUILD_TESTS)
  enable_testing()
  add_executable(archbfd_tests tests/archbfd.cpp tests/archbfd_c.c)
  target_link_libraries(archbfd_tests PRIVATE ArchBFD::cpp qbfd::qbfd)
  if(QBFD_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(archbfd_tests PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
  add_test(NAME archbfd_abi COMMAND archbfd_tests)
endif()

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/ArchBFDConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/ArchBFDConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArchBFD)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ArchBFDConfigVersion.cmake
  VERSION 1.0.0 COMPATIBILITY SameMajorVersion)
install(TARGETS archbfd archbfd_cpp EXPORT ArchBFDTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES BFDBinABI.h BFDBinAPI.hpp ArchBFD.h ArchBFD.hpp ArchBFDApp.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/archbfd)
install(FILES ArchBFD.md DESTINATION ${CMAKE_INSTALL_DOCDIR}/ArchBFD)
install(EXPORT ArchBFDTargets NAMESPACE ArchBFD::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArchBFD)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ArchBFDConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/ArchBFDConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArchBFD)
if(QBFD_BUILD_TESTS)
  enable_testing()
  add_executable(qbfd_tests tests/test.cpp tests/other.cpp)
  target_link_libraries(qbfd_tests PRIVATE qbfd)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(qbfd_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(QBFD_SANITIZE)
      target_compile_options(qbfd_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(qbfd_tests PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME qbfd_regressions COMMAND qbfd_tests)

  # Cross-target fixtures: build objects with Clang, cross-check their metadata
  # against LLVM's own reader, then feed them to the same test binary. Skipped,
  # not failed, when Clang or llvm-readobj is unavailable.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME qbfd_source_notes
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/validate_bfd_notes.py)
    set_tests_properties(qbfd_source_notes PROPERTIES SKIP_RETURN_CODE 77)
    add_test(NAME qbfd_fixtures
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-fixtures.py
              $<TARGET_FILE:qbfd_tests>)
    set_tests_properties(qbfd_fixtures PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()
