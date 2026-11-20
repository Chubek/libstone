file(MAKE_DIRECTORY "${WORK_DIR}")
set(svg_file "${WORK_DIR}/shapes.svg")
set(eps_file "${WORK_DIR}/shapes.eps")

execute_process(COMMAND "${CLI}" "${INPUT}" RESULT_VARIABLE result
                OUTPUT_VARIABLE svg ERROR_VARIABLE errors)
if(NOT result STREQUAL "0" OR NOT svg MATCHES "<svg" OR NOT svg MATCHES "stroke=\"#173b52\"")
  message(FATAL_ERROR "SVG compilation failed (${result}): ${errors}\n${svg}")
endif()

execute_process(COMMAND "${CLI}" --backend svg -o "${svg_file}" -
                INPUT_FILE "${INPUT}" RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "stdin/file compilation failed (${result}): ${errors}")
endif()
file(READ "${svg_file}" file_svg)
if(NOT file_svg STREQUAL svg)
  message(FATAL_ERROR "SVG compiled from stdin and a file differs")
endif()

execute_process(COMMAND "${CLI}" --backend eps -o "${eps_file}" "${INPUT}"
                RESULT_VARIABLE result ERROR_VARIABLE errors)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "EPS compilation failed (${result}): ${errors}")
endif()
file(READ "${eps_file}" eps)
if(NOT eps MATCHES "^%!PS-Adobe-3.0 EPSF-3.0" OR NOT eps MATCHES "%%BoundingBox: 0 0 120 80")
  message(FATAL_ERROR "EPS is missing its header or bounding box")
endif()
if(GHOSTSCRIPT)
  execute_process(COMMAND "${GHOSTSCRIPT}" -q -dSAFER -dBATCH -dNOPAUSE -sDEVICE=nullpage "${eps_file}"
                  RESULT_VARIABLE result ERROR_VARIABLE errors)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Ghostscript rejected EPS (${result}): ${errors}")
  endif()
endif()

file(WRITE "${WORK_DIR}/invalid.mvct" "canvas(2, 2) {\n  circle(0,0,-1,\"#000000\");\n}\n")
file(WRITE "${svg_file}" "untouched")
execute_process(COMMAND "${CLI}" -o "${svg_file}" "${WORK_DIR}/invalid.mvct"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result MATCHES "^[1-9][0-9]*$" OR NOT output STREQUAL ""
   OR NOT errors MATCHES "invalid\\.mvct:2:[0-9]+")
  message(FATAL_ERROR "Invalid source did not yield a located failure (${result}): ${errors}")
endif()
file(READ "${svg_file}" file_svg)
if(NOT file_svg STREQUAL "untouched")
  message(FATAL_ERROR "Compilation failure rewrote its output file")
endif()

execute_process(COMMAND "${CLI}" --backend unsupported "${INPUT}"
                RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result MATCHES "^[1-9][0-9]*$" OR NOT output STREQUAL "")
  message(FATAL_ERROR "CLI accepted an unknown backend")
endif()
