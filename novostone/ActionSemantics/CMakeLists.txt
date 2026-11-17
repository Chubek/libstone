set(action_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${action_generated}")
add_custom_command(
  OUTPUT "${action_generated}/ActionSemanticsAST.h" "${action_generated}/ActionSemanticsAST.c"
    "${action_generated}/ActionSemanticsVisitor.h"
  COMMAND "${PERL_EXECUTABLE}" "${novostone_scripts}/asdl.pl"
    --header "${action_generated}/ActionSemanticsAST.h" --source "${action_generated}/ActionSemanticsAST.c"
    --visitor "${action_generated}/ActionSemanticsVisitor.h" "${CMAKE_CURRENT_SOURCE_DIR}/ActionSemantics.asdl"
  DEPENDS ActionSemantics.asdl "${novostone_scripts}/asdl.pl" VERBATIM)
add_custom_command(
  OUTPUT "${action_generated}/ActionSemanticsParser.c"
  COMMAND "${CMAKE_COMMAND}" "-DPERL=${PERL_EXECUTABLE}" "-DGENERATOR=${novostone_scripts}/aurocks.pl"
    "-DGRAMMAR=${CMAKE_CURRENT_SOURCE_DIR}/ActionSemantics.g" "-DOUTPUT=${action_generated}/ActionSemanticsParser.c"
    -DPREFIX=novo_as -P "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/GenerateParser.cmake"
  DEPENDS ActionSemantics.g Syntax.h ../cmake/GenerateParser.cmake "${novostone_scripts}/aurocks.pl" VERBATIM)
add_custom_target(novostone-action-generated DEPENDS
  "${action_generated}/ActionSemanticsAST.h" "${action_generated}/ActionSemanticsAST.c"
  "${action_generated}/ActionSemanticsVisitor.h" "${action_generated}/ActionSemanticsParser.c")
add_dependencies(novostone_ActionSemantics novostone-action-generated)
file(READ runtime.c.in ACSEM_C)
file(READ runtime.py.in ACSEM_PYTHON)
file(READ runtime.rs.in ACSEM_RUST)
file(READ runtime.pas.in ACSEM_PASCAL)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS runtime.c.in runtime.py.in runtime.rs.in runtime.pas.in)
configure_file(RuntimeSources.hpp.in "${action_generated}/ActionRuntimeSources.hpp" @ONLY)
target_sources(novostone_ActionSemantics PRIVATE Generate.cpp
  "${action_generated}/ActionSemanticsAST.h" "${action_generated}/ActionSemanticsAST.c"
  "${action_generated}/ActionSemanticsVisitor.h" "${action_generated}/ActionSemanticsParser.c")
target_include_directories(novostone_ActionSemantics PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(novostone_ActionSemantics SYSTEM PRIVATE "${action_generated}")
target_compile_features(novostone_ActionSemantics PRIVATE c_std_11)
target_link_libraries(novostone_ActionSemantics PRIVATE Threads::Threads)
if(NOVOSTONE_BUILD_TOOLS)
  add_executable(acsem main.cpp)
  target_link_libraries(acsem PRIVATE Novostone::ActionSemantics)
  libstone_warnings(acsem)
  file(RELATIVE_PATH bin_to_lib "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
  if(APPLE)
    set_target_properties(acsem PROPERTIES INSTALL_RPATH "@loader_path/${bin_to_lib}")
  elseif(UNIX)
    set_target_properties(acsem PROPERTIES INSTALL_RPATH "$ORIGIN/${bin_to_lib}")
  endif()
  install(TARGETS acsem RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES ActionSemantics.g ActionSemantics.asdl DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/ActionSemantics)
install(FILES README.md DESIGN.md STATUS.md DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/ActionSemantics)
install(DIRECTORY examples DESTINATION ${CMAKE_INSTALL_DATADIR}/novostone/ActionSemantics)
if(NOVOSTONE_BUILD_TESTS)
  add_executable(novostone-action-tests tests/frontend.cpp)
  target_link_libraries(novostone-action-tests PRIVATE Novostone::ActionSemantics Novostone::Datalog)
  libstone_warnings(novostone-action-tests)
  add_test(NAME novostone-action-frontend COMMAND novostone-action-tests)
  if(NOVOSTONE_BUILD_TOOLS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    find_program(ACSEM_RUSTC rustc)
    find_program(ACSEM_FPC fpc)
    find_program(ACSEM_PDFLATEX pdflatex)
    function(action_backend_test backend compiler)
      add_test(NAME novostone-action-${backend} COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/backends.py" --cli "$<TARGET_FILE:acsem>"
        --source "${CMAKE_CURRENT_SOURCE_DIR}/examples/expressions.acsem"
        --work "${CMAKE_CURRENT_BINARY_DIR}/test-${backend}" --target "${backend}" --compiler "${compiler}"
        "--flags=${CMAKE_C_FLAGS}")
      set_tests_properties(novostone-action-${backend} PROPERTIES TIMEOUT 180)
    endfunction()
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      action_backend_test(c "${CMAKE_C_COMPILER}")
    endif()
    action_backend_test(python "${Python3_EXECUTABLE}")
    if(ACSEM_RUSTC)
      action_backend_test(rust "${ACSEM_RUSTC}")
    else()
      message(STATUS "ActionSemantics Rust execution tests unavailable: rustc not found")
    endif()
    if(ACSEM_FPC)
      action_backend_test(pascal "${ACSEM_FPC}")
    else()
      message(STATUS "ActionSemantics Pascal execution tests unavailable: fpc not found")
    endif()
    if(ACSEM_PDFLATEX)
      action_backend_test(latex "${ACSEM_PDFLATEX}")
    else()
      message(STATUS "ActionSemantics LaTeX compilation test unavailable: pdflatex not found")
    endif()
  endif()
endif()
