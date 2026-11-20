cmake_minimum_required(VERSION 3.20)

set(args --no-strpool)
if(NOT HELPER STREQUAL "")
  list(APPEND args --liblr-helper "${HELPER}")
endif()
get_filename_component(generated_dir "${PARSER}" DIRECTORY)
file(MAKE_DIRECTORY "${generated_dir}")
execute_process(COMMAND "${PERL}" "${GENERATOR}" ${args} "${GRAMMAR}"
  OUTPUT_VARIABLE source ERROR_VARIABLE diagnostic RESULT_VARIABLE result)
if(NOT result STREQUAL "0")
  message(FATAL_ERROR "Aurocks generator failed (${result}): ${diagnostic}")
endif()
file(WRITE "${PARSER}" "${source}")
