# Optional integration check: compile actual tool-conventions objects, exercise
# the library API, and run the CLI's finalized output.
if(NOT CLANG)
  find_program(CLANG NAMES clang clang-22 clang-21 clang-20 clang-19 clang-18 clang-17)
endif()
if(NOT CLANG)
  message(STATUS "Skipping wasm32 object interoperability: clang not found")
  return()
endif()
execute_process(COMMAND "${CLANG}" -print-targets
  RESULT_VARIABLE status OUTPUT_VARIABLE targets ERROR_QUIET)
if(NOT status EQUAL 0 OR NOT targets MATCHES "wasm32[ \t]+-")
  message(STATUS "Skipping wasm32 object interoperability: ${CLANG} does not report a wasm32 target")
  return()
endif()

foreach(var IN ITEMS LINK RUN VALIDATE OBJECT_TEST BINARY)
  if(NOT DEFINED ${var})
    message(FATAL_ERROR "Missing -D${var}=...")
  endif()
endforeach()
set(work "${BINARY}/object_link_clang")
file(MAKE_DIRECTORY "${work}")

function(check label)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE status
    OUTPUT_VARIABLE out ERROR_VARIABLE error)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "${label} failed (${status}): ${out}${error}")
  endif()
endfunction()

foreach(optimization IN ITEMS O0 O2)
  foreach(fixture IN ITEMS caller provider)
    check("clang wasm32 -${optimization} ${fixture}" "${CLANG}" --target=wasm32-unknown-unknown
      -ffreestanding "-${optimization}" -c "${CMAKE_CURRENT_LIST_DIR}/object_${fixture}.c"
      -o "${work}/${fixture}_${optimization}.o")
  endforeach()
  check("object-link API on clang -${optimization} inputs" "${OBJECT_TEST}"
    "${work}/caller_${optimization}.o" "${work}/provider_${optimization}.o")
endforeach()
set(caller "${work}/caller_O0.o")
set(provider "${work}/provider_O0.o")

set(output "${work}/linked.wasm")
execute_process(COMMAND "${LINK}" --export run --export=__wasm_call_ctors "${caller}" "${provider}"
  RESULT_VARIABLE status OUTPUT_FILE "${output}" ERROR_VARIABLE error)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "wasm-link failed (${status}): ${error}")
endif()
check("validate object-link output" "${VALIDATE}" "${output}")
execute_process(COMMAND "${RUN}" "${output}" run RESULT_VARIABLE status
  OUTPUT_VARIABLE out ERROR_VARIABLE error OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status EQUAL 0 OR NOT out STREQUAL "i32:0x00000070")
  message(FATAL_ERROR "linked program should return 112, got (${status}): ${out}${error}")
endif()

function(reject needle)
  execute_process(COMMAND "${LINK}" ${ARGN}
    RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE error)
  string(FIND "${error}" "${needle}" found)
  if(status EQUAL 0 OR found LESS 0)
    message(FATAL_ERROR "Expected '${needle}' (status ${status}): ${error}")
  endif()
endfunction()

reject("undefined data symbol 'value'" --export run "${caller}")
reject("cannot mix relocatable Wasm objects and finalized modules" "${caller}" "${output}")
reject("named inputs are not supported for Wasm objects" "ns=${caller}")

check("clang wasm32 PIC diagnostic fixture" "${CLANG}" --target=wasm32-unknown-unknown
  -ffreestanding -fPIC -c "${CMAKE_CURRENT_LIST_DIR}/object_caller.c" -o "${work}/pic.o")
reject("unsupported position-independent or TLS import" "${work}/pic.o")
