# The library itself needs only C++; compile emitted source when a native C
# toolchain is available. Cross builds cannot run the fixture generator.
if(CMAKE_CROSSCOMPILING)
  return()
endif()
include(CheckLanguage)
check_language(C)
if(NOT CMAKE_C_COMPILER)
  return()
endif()
enable_language(C)

add_executable(firestone_lr_fixture_generator tests/lr_generate_fixture.cpp)
target_link_libraries(firestone_lr_fixture_generator PRIVATE Firestone::lr)
set_target_properties(firestone_lr_fixture_generator PROPERTIES
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(firestone_lr_fixture_generator PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

set(lr_fixture_directory "${CMAKE_CURRENT_BINARY_DIR}/lr-generated/$<CONFIG>")
set(lr_fixture_c "${lr_fixture_directory}/scanner.c")
set(lr_fixture_outputs "${lr_fixture_c}")
set(lr_test_assembly OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
    AND CMAKE_SIZEOF_VOID_P EQUAL 8
    AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  check_language(ASM)
  if(CMAKE_ASM_COMPILER)
    enable_language(ASM)
    set(lr_test_assembly ON)
    list(APPEND lr_fixture_outputs "${lr_fixture_directory}/scanner.S")
  endif()
endif()

add_custom_command(OUTPUT ${lr_fixture_outputs}
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${lr_fixture_directory}"
  COMMAND firestone_lr_fixture_generator ${lr_fixture_outputs}
  DEPENDS firestone_lr_fixture_generator
  VERBATIM)
add_executable(firestone_lr_codegen_test tests/lr_generated.c ${lr_fixture_outputs})
set_target_properties(firestone_lr_codegen_test PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(firestone_lr_codegen_test PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wpedantic;-Werror>)
endif()
if(lr_test_assembly)
  target_compile_definitions(firestone_lr_codegen_test PRIVATE FIRESTONE_LR_TEST_ASSEMBLY)
endif()
add_test(NAME firestone.lr_codegen COMMAND firestone_lr_codegen_test)
set_tests_properties(firestone.lr_codegen PROPERTIES TIMEOUT 60)
