cmake_minimum_required(VERSION 3.20)

file(MAKE_DIRECTORY "${WORK_DIR}")
set(input "${WORK_DIR}/test input.nfa")
set(output "${WORK_DIR}/test output.dfa")
string(REPEAT "0" 64 zero)
string(REPEAT "f" 64 full)

function(check_dfa nfa expected)
  file(WRITE "${input}" "${nfa}")
  execute_process(COMMAND "${HELPER}" "${input}" "${output}"
    RESULT_VARIABLE result ERROR_VARIABLE diagnostic)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Helper failed (${result}): ${diagnostic}")
  endif()
  file(READ "${output}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Unexpected DFA:\n${actual}\nExpected:\n${expected}")
  endif()
endfunction()

function(make_label name group digit)
  string(SUBSTRING "${zero}" 0 ${group} prefix)
  math(EXPR suffix_size "63 - ${group}")
  string(SUBSTRING "${zero}" 0 ${suffix_size} suffix)
  set(${name} "${prefix}${digit}${suffix}" PARENT_SCOPE)
endfunction()

make_label(byte_5 1 2)
make_label(byte_7 1 8)
make_label(byte_10 2 4)
make_label(byte_128 32 1)
string(REPEAT "0" 62 middle)
set(byte_0_and_255 "1${middle}8")

# Start at state 3, with an epsilon cycle through 4. Bytes 5 and 7
# reach equivalent states; 5 also reaches the dead sink (state 0).
# States 5 and 6 are equivalent accepting states. State 7 is unreachable
# because the byte set labeling its sole incoming edge is empty.
set(nfa "8\t3\r\n")
string(APPEND nfa "0 1 0 ${full}\n")
string(APPEND nfa "0 1 5 ${byte_0_and_255}\n")
string(APPEND nfa "0 1 6 ${byte_0_and_255}\n")
string(APPEND nfa "0 3 4 - 7 ${zero} 0 ${byte_10}\n")
string(APPEND nfa "0 5 3 - 1 ${byte_5} 2 ${byte_7} 5 ${byte_128} 0 ${byte_5}\n")
string(APPEND nfa "1 0\n1 0\n1 1 7 ${full}\n")

set(row0 "0")
set(row1 "0")
set(row2 "1")
foreach(byte RANGE 0 255)
  if(byte EQUAL 5 OR byte EQUAL 7)
    string(APPEND row0 " 1")
  elseif(byte EQUAL 128)
    string(APPEND row0 " 2")
  else()
    string(APPEND row0 " -1")
  endif()
  if(byte EQUAL 0 OR byte EQUAL 255)
    string(APPEND row1 " 2")
  else()
    string(APPEND row1 " -1")
  endif()
  string(APPEND row2 " -1")
endforeach()
check_dfa("${nfa}" "3 0\n${row0}\n${row1}\n${row2}\n")

# The empty string can be accepted at the start of the DFA.
check_dfa("2 1\n1 0\n0 1 0 -" "1 0\n${row2}\n")
# A reject-all NFA still produces a valid one-state partial DFA.
string(REPEAT " -1" 256 missing)
check_dfa("1 0\n0 1 0 ${full}\n" "1 0\n0${missing}\n")
