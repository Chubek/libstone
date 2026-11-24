find_package(Perl REQUIRED)
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

set(dsymspec_llsynth "${CMAKE_CURRENT_SOURCE_DIR}/../../cornerstone/LLSynth/scripts")
set(dsymspec_frontend "${CMAKE_CURRENT_BINARY_DIR}/dsymspec_frontend.py")
set(dsymspec_compiler "${CMAKE_CURRENT_SOURCE_DIR}/dsymspec.py")
set(dsymspec_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")

file(GLOB dsymspec_generator_sources CONFIGURE_DEPENDS "${dsymspec_llsynth}/*.pm")
file(GLOB dsymspec_runtime_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.py")
add_custom_command(
  OUTPUT "${dsymspec_frontend}"
  COMMAND "${PERL_EXECUTABLE}" "${dsymspec_llsynth}/llsynth.pl"
          --mode=interpret -T python3 --driver-mode=library
          -o "${dsymspec_frontend}" "${CMAKE_CURRENT_SOURCE_DIR}/DSymspec.llsynth"
  DEPENDS DSymspec.llsynth "${dsymspec_llsynth}/llsynth.pl" ${dsymspec_generator_sources}
  VERBATIM
)

file(GLOB dsymspec_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/stdspec/*.dsymspec")
set(dsymspec_test_catalog "")
set(dsymspec_test_profiles "")
foreach(spec IN LISTS dsymspec_inputs)
  get_filename_component(profile "${spec}" NAME_WE)
  set(output "${dsymspec_generated_dir}/${profile}.i")
  set(included_specs)
  if(profile STREQUAL "rv64im" OR profile STREQUAL "riscv32")
    list(APPEND included_specs "${CMAKE_CURRENT_SOURCE_DIR}/stdspec/riscv64.dsymspec")
  endif()
  add_custom_command(
    OUTPUT "${output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dsymspec_generated_dir}"
    COMMAND "${Python3_EXECUTABLE}" -B "${dsymspec_compiler}"
            --frontend "${dsymspec_frontend}" -o "${output}" "${spec}"
    DEPENDS "${spec}" ${included_specs} "${dsymspec_frontend}" ${dsymspec_runtime_sources}
    COMMENT "Generating DSymspec include ${profile}.i"
    VERBATIM
  )
  list(APPEND dsymspec_includes "${output}")
  string(APPEND dsymspec_test_catalog "#include \"${profile}.i\"\n")
  string(APPEND dsymspec_test_profiles "    &dsymspec_${profile},\n")
endforeach()
add_custom_target(sandstone-dsymspec ALL DEPENDS ${dsymspec_includes})

add_library(sandstone_dsymspec INTERFACE)
add_library(Sandstone::DSymspec ALIAS sandstone_dsymspec)
set_target_properties(sandstone_dsymspec PROPERTIES EXPORT_NAME DSymspec)
add_dependencies(sandstone_dsymspec sandstone-dsymspec)
target_include_directories(sandstone_dsymspec INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  "$<BUILD_INTERFACE:${dsymspec_generated_dir}>"
  "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/sandstone/dsymspec>"
)
target_compile_features(sandstone_dsymspec INTERFACE c_std_11 cxx_std_11)
install(TARGETS sandstone_dsymspec EXPORT SandstoneTargets)
install(FILES DSymspec.h ${dsymspec_includes}
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sandstone/dsymspec"
)

if(SANDSTONE_BUILD_TESTS)
  set(dsymspec_test_catalog_path "${CMAKE_CURRENT_BINARY_DIR}/dsymspec_test_catalog.inc")
  file(GENERATE OUTPUT "${dsymspec_test_catalog_path}" CONTENT "${dsymspec_test_catalog}")
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dsymspec_test_profiles.inc"
    CONTENT "${dsymspec_test_profiles}")
  add_executable(sandstone_dsymspec_c tests/catalog.c)
  add_executable(sandstone_dsymspec_cpp tests/catalog.cpp)
  foreach(test_target IN ITEMS sandstone_dsymspec_c sandstone_dsymspec_cpp)
    target_include_directories(${test_target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(${test_target} PRIVATE Sandstone::DSymspec)
    set_target_properties(${test_target} PROPERTIES C_EXTENSIONS OFF CXX_EXTENSIONS OFF)
    libstone_warnings(${test_target})
    add_test(NAME ${test_target} COMMAND ${test_target})
  endforeach()
  set_target_properties(sandstone_dsymspec_cpp PROPERTIES CXX_STANDARD 11)
  add_test(NAME sandstone_dsymspec_compiler
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/compiler.py"
      --compiler "${dsymspec_compiler}" --frontend "${dsymspec_frontend}"
      --scratch "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/stdspec/rv64im.dsymspec"
  )
  find_program(SANDSTONE_DSYMSPEC_LLVM_MC NAMES llvm-mc)
  if(SANDSTONE_DSYMSPEC_LLVM_MC)
    add_test(NAME sandstone_dsymspec_llvm
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/llvm_reference.py"
        "${SANDSTONE_DSYMSPEC_LLVM_MC}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/encodings.def"
    )
    set_tests_properties(sandstone_dsymspec_llvm PROPERTIES SKIP_RETURN_CODE 77)

    find_program(SANDSTONE_DSYMSPEC_LLVM_READOBJ NAMES llvm-readobj)
    execute_process(COMMAND "${SANDSTONE_DSYMSPEC_LLVM_MC}" --version
      OUTPUT_VARIABLE dsymspec_llvm_version)
    set(dsymspec_llvm_major 0)
    if(dsymspec_llvm_version MATCHES "LLVM version ([0-9]+)")
      set(dsymspec_llvm_major "${CMAKE_MATCH_1}")
    endif()
    if(SANDSTONE_DSYMSPEC_LLVM_READOBJ AND dsymspec_llvm_major GREATER_EQUAL 22
       AND NOT CMAKE_CROSSCOMPILING AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      if(Python3_VERSION VERSION_GREATER_EQUAL 3.10
         AND dsymspec_llvm_version MATCHES "riscv32[ \t]+-" AND dsymspec_llvm_version MATCHES "riscv64[ \t]+-")
        add_test(NAME sandstone_dsymspec_riscv_llvm
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/riscv64/check.py"
            "${dsymspec_generated_dir}" --work "${CMAKE_CURRENT_BINARY_DIR}"
            --cc "${CMAKE_C_COMPILER}" --llvm-mc "${SANDSTONE_DSYMSPEC_LLVM_MC}"
            --llvm-readobj "${SANDSTONE_DSYMSPEC_LLVM_READOBJ}"
        )
      endif()
      if(dsymspec_llvm_version MATCHES "aarch64[ \t]+-")
        add_test(NAME sandstone_dsymspec_aarch64_llvm
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/aarch64/check.py"
            "${dsymspec_generated_dir}" --scratch "${CMAKE_CURRENT_BINARY_DIR}"
            --cc "${CMAKE_C_COMPILER}" --llvm-mc "${SANDSTONE_DSYMSPEC_LLVM_MC}"
            --llvm-readobj "${SANDSTONE_DSYMSPEC_LLVM_READOBJ}"
        )
      endif()
      find_program(SANDSTONE_DSYMSPEC_LLVM_OBJCOPY NAMES llvm-objcopy)
      if(Python3_VERSION VERSION_GREATER_EQUAL 3.10
         AND SANDSTONE_DSYMSPEC_LLVM_OBJCOPY AND dsymspec_llvm_version MATCHES "x86-64[ \t]+-")
        add_executable(sandstone_dsymspec_amd64_probe tests/amd64/encode.c)
        target_link_libraries(sandstone_dsymspec_amd64_probe PRIVATE Sandstone::DSymspec)
        set_target_properties(sandstone_dsymspec_amd64_probe PROPERTIES C_EXTENSIONS OFF)
        libstone_warnings(sandstone_dsymspec_amd64_probe)
        add_test(NAME sandstone_dsymspec_amd64_llvm
          COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/amd64/differential.py"
            "$<TARGET_FILE:sandstone_dsymspec_amd64_probe>" --work "${CMAKE_CURRENT_BINARY_DIR}/amd64-reference"
            --llvm-mc "${SANDSTONE_DSYMSPEC_LLVM_MC}" --llvm-readobj "${SANDSTONE_DSYMSPEC_LLVM_READOBJ}"
            --llvm-objcopy "${SANDSTONE_DSYMSPEC_LLVM_OBJCOPY}"
        )
      endif()
    endif()
  endif()
endif()
