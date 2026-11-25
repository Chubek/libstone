cmake_minimum_required(VERSION 3.20)
project(Novostone VERSION 0.1.0 LANGUAGES C CXX)
include(../cmake/LibstoneBuild.cmake)
include(CMakePackageConfigHelpers)
find_package(Threads REQUIRED)
find_package(Perl 5.14 REQUIRED)
option(NOVOSTONE_BUILD_TESTS "Build Novostone semantic and contract tests" "${LIBSTONE_BUILD_TESTS}")
option(NOVOSTONE_BUILD_TOOLS "Build the Novostone example/inspector" "${LIBSTONE_BUILD_TOOLS}")
option(NOVOSTONE_PCC_USE_SATIE
  "Use Satie's SMT-LIB2 frontend to discharge PCC proof obligations" ON)
option(NOVOSTONE_PCC_REQUIRE_SATIE
  "Fail configuration when PCC Satie integration is unavailable" OFF)
option(NOVOSTONE_PCC_SIGNED_PACKAGES
  "Build Ed25519 PCC package signing, verification, and SSH stdio service" OFF)
if(NOVOSTONE_PCC_REQUIRE_SATIE AND NOT NOVOSTONE_PCC_USE_SATIE)
  message(FATAL_ERROR
    "NOVOSTONE_PCC_REQUIRE_SATIE=ON requires NOVOSTONE_PCC_USE_SATIE=ON")
endif()

foreach(component IN ITEMS Datalog CCDB DatabaseBasedIR QueryBasedIR SpeculativeIR CostCC PCC ActionSemantics)
  add_library(novostone_${component} ${component}/${component}.cpp)
  add_library(Novostone::${component} ALIAS novostone_${component})
  set_target_properties(novostone_${component} PROPERTIES EXPORT_NAME ${component}
    CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(APPLE)
    set_target_properties(novostone_${component} PROPERTIES INSTALL_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(novostone_${component} PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  target_compile_features(novostone_${component} PUBLIC cxx_std_20)
  target_include_directories(novostone_${component} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  libstone_warnings(novostone_${component})
  install(TARGETS novostone_${component} EXPORT NovostoneTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES ${component}/${component}.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/novostone/${component})
endforeach()
set(novostone_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(novostone_scripts "${CMAKE_CURRENT_SOURCE_DIR}/../scripts")
libstone_aurocks_command(novostone_aurocks novostone_aurocks_dep)
file(MAKE_DIRECTORY "${novostone_generated}")
add_custom_command(
  OUTPUT "${novostone_generated}/DatalogAST.h" "${novostone_generated}/DatalogAST.c"
  COMMAND "${PERL_EXECUTABLE}" "${novostone_scripts}/asdl.pl"
    --header "${novostone_generated}/DatalogAST.h" --source "${novostone_generated}/DatalogAST.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/Datalog/Datalog.asdl"
  DEPENDS Datalog/Datalog.asdl "${novostone_scripts}/asdl.pl" VERBATIM)
add_custom_command(
  OUTPUT "${novostone_generated}/DatalogParser.c"
  COMMAND "${CMAKE_COMMAND}" "-DGENERATOR_COMMAND=${novostone_aurocks}"
    "-DGRAMMAR=${CMAKE_CURRENT_SOURCE_DIR}/Datalog/Datalog.g"
    "-DOUTPUT=${novostone_generated}/DatalogParser.c" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateParser.cmake"
  DEPENDS Datalog/Datalog.g Datalog/Syntax.h cmake/GenerateParser.cmake "${novostone_aurocks_dep}" VERBATIM)
target_sources(novostone_Datalog PRIVATE Datalog/Parse.cpp
  "${novostone_generated}/DatalogAST.h" "${novostone_generated}/DatalogAST.c" "${novostone_generated}/DatalogParser.c")
target_compile_features(novostone_Datalog PRIVATE c_std_11)
target_include_directories(novostone_Datalog PRIVATE Datalog)
target_include_directories(novostone_Datalog SYSTEM PRIVATE "${novostone_generated}")
target_link_libraries(novostone_Datalog PRIVATE Threads::Threads)
add_subdirectory(ActionSemantics)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/DatabaseBasedIR/Analysis.dl" NOVOSTONE_ANALYSIS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1" NOVOSTONE_ANALYSIS "${NOVOSTONE_ANALYSIS_HEX}")
configure_file(cmake/AnalysisSource.hpp.in "${novostone_generated}/AnalysisSource.hpp" @ONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS DatabaseBasedIR/Analysis.dl)
target_include_directories(novostone_DatabaseBasedIR PRIVATE "${novostone_generated}")
target_link_libraries(novostone_CCDB PUBLIC Novostone::Datalog Threads::Threads)
target_link_libraries(novostone_DatabaseBasedIR PUBLIC Novostone::CCDB)
target_sources(novostone_DatabaseBasedIR PRIVATE DatabaseBasedIR/Analysis.cpp)
target_link_libraries(novostone_QueryBasedIR PUBLIC Novostone::DatabaseBasedIR)
target_sources(novostone_QueryBasedIR PRIVATE QueryBasedIR/Validation.cpp QueryBasedIR/Propagation.cpp QueryBasedIR/Reduction.cpp)
target_link_libraries(novostone_SpeculativeIR PUBLIC Novostone::DatabaseBasedIR)
target_link_libraries(novostone_CostCC PUBLIC Novostone::QueryBasedIR)
target_sources(novostone_PCC PRIVATE
  PCC/SMTProof.cpp PCC/RegisterAllocation.cpp PCC/Native.cpp)
target_link_libraries(novostone_PCC PUBLIC Novostone::QueryBasedIR)
install(FILES PCC/SMTProof.hpp PCC/RegisterAllocation.hpp PCC/Native.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/novostone/PCC)

set(NOVOSTONE_PCC_HAS_SATIE OFF)
set(NOVOSTONE_PCC_PACKAGE_REQUIRES_SATIE OFF)
if(NOVOSTONE_PCC_USE_SATIE)
  set(novostone_pcc_satie_target_usable OFF)
  if(TARGET satie::satie)
    # The aggregate Limestone build exposes a headers/runtime-only INTERFACE
    # target under this name.  It intentionally omits Satie's parser sources,
    # so only a compiled Satie target can authorize the PCC conduit.
    get_target_property(novostone_pcc_satie_type satie::satie TYPE)
    if(NOT novostone_pcc_satie_type STREQUAL "INTERFACE_LIBRARY")
      set(novostone_pcc_satie_target_usable ON)
    endif()
  else()
    find_package(Satie 0.1 CONFIG QUIET)
    if(TARGET satie::satie)
      set(novostone_pcc_satie_target_usable ON)
    endif()
  endif()
  if(novostone_pcc_satie_target_usable)
    set(NOVOSTONE_PCC_HAS_SATIE ON)
    target_compile_definitions(novostone_PCC PRIVATE
      NOVOSTONE_PCC_USE_SATIE=1)
    target_link_libraries(novostone_PCC PRIVATE satie::satie)
    get_target_property(novostone_pcc_library_type novostone_PCC TYPE)
    if(novostone_pcc_library_type STREQUAL "STATIC_LIBRARY")
      # A static PCC archive leaves Satie symbols for the final consumer link.
      # Shared PCC builds absorb the private static Satie implementation.
      set(NOVOSTONE_PCC_PACKAGE_REQUIRES_SATIE ON)
    endif()
    message(STATUS "Novostone PCC will discharge SMT-LIB2 obligations with Satie")
  elseif(NOVOSTONE_PCC_REQUIRE_SATIE)
    message(FATAL_ERROR
      "NOVOSTONE_PCC_REQUIRE_SATIE=ON requires a compiled Satie package; "
      "set Satie_DIR/CMAKE_PREFIX_PATH or disable the requirement")
  else()
    message(STATUS
      "Compiled Satie frontend unavailable: PCC emits SMT-LIB2 with Unknown "
      "discharge status")
  endif()
endif()

set(NOVOSTONE_PCC_PACKAGE_REQUIRES_OPENSSL OFF)
if(NOVOSTONE_PCC_SIGNED_PACKAGES)
  find_package(OpenSSL 3.0 REQUIRED COMPONENTS Crypto)
  add_library(novostone_PCCPackage PCC/Package.cpp)
  add_library(Novostone::PCCPackage ALIAS novostone_PCCPackage)
  set_target_properties(novostone_PCCPackage PROPERTIES EXPORT_NAME PCCPackage
    CXX_EXTENSIONS OFF POSITION_INDEPENDENT_CODE ON WINDOWS_EXPORT_ALL_SYMBOLS ON)
  if(APPLE)
    set_target_properties(novostone_PCCPackage PROPERTIES INSTALL_RPATH "@loader_path")
  elseif(UNIX)
    set_target_properties(novostone_PCCPackage PROPERTIES INSTALL_RPATH "$ORIGIN")
  endif()
  target_link_libraries(novostone_PCCPackage PUBLIC Novostone::PCC PRIVATE OpenSSL::Crypto)
  libstone_warnings(novostone_PCCPackage)
  get_target_property(novostone_package_type novostone_PCCPackage TYPE)
  if(novostone_package_type STREQUAL "STATIC_LIBRARY")
    set(NOVOSTONE_PCC_PACKAGE_REQUIRES_OPENSSL ON)
  endif()
  install(TARGETS novostone_PCCPackage EXPORT NovostoneTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  install(FILES PCC/Package.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/novostone/PCC)
  install(FILES PCC/Package.md DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/PCC)
endif()

if(NOVOSTONE_BUILD_TOOLS)
  add_executable(novostone tools/main.cpp)
  target_link_libraries(novostone PRIVATE
    Novostone::CostCC Novostone::SpeculativeIR Novostone::PCC)
  if(NOVOSTONE_PCC_SIGNED_PACKAGES)
    target_sources(novostone PRIVATE tools/PCCPackage.cpp)
    target_link_libraries(novostone PRIVATE Novostone::PCCPackage)
    target_compile_definitions(novostone PRIVATE NOVOSTONE_PCC_SIGNED_PACKAGES=1)
  endif()
  file(RELATIVE_PATH novostone_bin_to_lib "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(APPLE)
    set_target_properties(novostone PROPERTIES INSTALL_RPATH "@loader_path/${novostone_bin_to_lib}")
  elseif(UNIX)
    set_target_properties(novostone PROPERTIES INSTALL_RPATH "$ORIGIN/${novostone_bin_to_lib}")
  endif()
  libstone_warnings(novostone)
  install(TARGETS novostone RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(NOVOSTONE_BUILD_TESTS)
  enable_testing()
  add_executable(novostone-frontend-tests tests/frontend.cpp)
  target_link_libraries(novostone-frontend-tests PRIVATE Novostone::CostCC Novostone::SpeculativeIR)
  libstone_warnings(novostone-frontend-tests)
  foreach(suite IN ITEMS frontend integration)
    add_test(NAME novostone-${suite} COMMAND novostone-frontend-tests ${suite})
  endforeach()
  add_executable(novostone-tests tests/contracts.cpp)
  target_link_libraries(novostone-tests PRIVATE Novostone::CostCC Novostone::SpeculativeIR)
  libstone_warnings(novostone-tests)
  foreach(suite IN ITEMS datalog ccdb ir query speculation cost)
    add_test(NAME novostone-${suite} COMMAND novostone-tests ${suite})
  endforeach()
  add_executable(novostone-regressions tests/regressions.cpp)
  target_link_libraries(novostone-regressions PRIVATE Novostone::CostCC Novostone::SpeculativeIR)
  libstone_warnings(novostone-regressions)
  foreach(suite IN ITEMS ssa cfg memory validation persistence hardening)
    add_test(NAME novostone-${suite} COMMAND novostone-regressions ${suite})
  endforeach()
  add_executable(novostone-analysis-tests tests/analyses.cpp)
  target_link_libraries(novostone-analysis-tests PRIVATE Novostone::CostCC Novostone::SpeculativeIR)
  libstone_warnings(novostone-analysis-tests)
  foreach(suite IN ITEMS seminaive transactions liveness sccp cycles)
    add_test(NAME novostone-${suite} COMMAND novostone-analysis-tests ${suite})
  endforeach()
  add_executable(novostone-dossier-tests tests/dossier.cpp)
  target_link_libraries(novostone-dossier-tests PRIVATE Novostone::CostCC Novostone::SpeculativeIR)
  libstone_warnings(novostone-dossier-tests)
  foreach(suite IN ITEMS recovery uncertainty reduction)
    add_test(NAME novostone-${suite} COMMAND novostone-dossier-tests ${suite})
  endforeach()
  add_executable(novostone-pcc-tests tests/pcc.cpp)
  target_link_libraries(novostone-pcc-tests PRIVATE Novostone::PCC)
  libstone_warnings(novostone-pcc-tests)
  foreach(suite IN ITEMS rewrite smt regalloc negative)
    add_test(NAME novostone-pcc-${suite} COMMAND novostone-pcc-tests ${suite})
  endforeach()
  add_executable(novostone-pcc-native-tests tests/pcc-native.cpp)
  target_link_libraries(novostone-pcc-native-tests PRIVATE Novostone::PCC)
  libstone_warnings(novostone-pcc-native-tests)
  if(NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
      AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
      AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_test(NAME novostone-pcc-native
      COMMAND novostone-pcc-native-tests "${CMAKE_C_COMPILER}")
  else()
    add_test(NAME novostone-pcc-native COMMAND novostone-pcc-native-tests)
  endif()
  if(NOVOSTONE_PCC_SIGNED_PACKAGES)
    add_executable(novostone-pcc-package-tests tests/pcc-package.cpp)
    target_link_libraries(novostone-pcc-package-tests PRIVATE Novostone::PCCPackage OpenSSL::Crypto)
    libstone_warnings(novostone-pcc-package-tests)
    add_test(NAME novostone-pcc-package COMMAND novostone-pcc-package-tests)
  endif()
  if(NOVOSTONE_BUILD_TOOLS)
    # PASS_REGULAR_EXPRESSION alone ignores a failing process exit code.
    # Require both successful execution and the expected semantic observation.
    function(novostone_cli_test name pattern)
      add_test(NAME ${name} COMMAND "${CMAKE_COMMAND}" "-DCLI=$<TARGET_FILE:novostone>"
        "-DARGUMENTS=${ARGN}" "-DEXPECT=${pattern}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cli-success.cmake")
    endfunction()
    novostone_cli_test(novostone-cli-datalog "\\(4\\)" datalog ${CMAKE_CURRENT_SOURCE_DIR}/examples/reachability.dl Sink)
    add_test(NAME novostone-demo COMMAND novostone demo)
    novostone_cli_test(novostone-cli-optimize "instr 4 const 0" optimize ${CMAKE_CURRENT_SOURCE_DIR}/examples/wrapping.nir)
    novostone_cli_test(novostone-cli-loop "result 45" run ${CMAKE_CURRENT_SOURCE_DIR}/examples/sum-loop.nir 10)
    novostone_cli_test(novostone-cli-validate "validation: passed" validate
      ${CMAKE_CURRENT_SOURCE_DIR}/examples/wrapping.nir ${CMAKE_CURRENT_SOURCE_DIR}/examples/wrapping.nir 728)
    add_test(NAME novostone-cli-reduction COMMAND ${CMAKE_COMMAND}
      -DCLI=$<TARGET_FILE:novostone> -DWORK=${CMAKE_CURRENT_BINARY_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli-reduction.cmake)
    novostone_cli_test(novostone-cli-analyze "loop header 2" analyze ${CMAKE_CURRENT_SOURCE_DIR}/examples/sum-loop.nir)
    novostone_cli_test(novostone-cli-sccp "instr 11 const 42" optimize ${CMAKE_CURRENT_SOURCE_DIR}/examples/equal-phi.nir)
    novostone_cli_test(novostone-cli-pcc "frontend smtlib2"
      pcc ${CMAKE_CURRENT_SOURCE_DIR}/examples/pcc.nir
      --frontend smtlib2 --transforms constant_folding,local_cse)
    add_test(NAME novostone-cli-pcc-require-proof COMMAND ${CMAKE_COMMAND}
      -DCLI=$<TARGET_FILE:novostone>
      -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/pcc.nir
      -DEXPECT_PROVED=${NOVOSTONE_PCC_HAS_SATIE}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli-pcc.cmake)
  endif()
endif()
install(FILES README.md STATUS.md DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone)
install(FILES PCC/README.md PCC/Native.md
  DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/PCC)
install(FILES Datalog/Datalog.g Datalog/Datalog.asdl DatabaseBasedIR/Analysis.dl
  DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/grammar)
install(DIRECTORY examples DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone)
install(EXPORT NovostoneTargets NAMESPACE Novostone:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Novostone)
configure_package_config_file(cmake/NovostoneConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/NovostoneConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Novostone)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/NovostoneConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/NovostoneConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/NovostoneConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Novostone)
