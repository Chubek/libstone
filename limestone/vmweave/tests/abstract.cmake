set(out "${BINARY}/abstract-runtime")
file(MAKE_DIRECTORY "${out}")
separate_arguments(flags NATIVE_COMMAND "${CFLAGS}")
function(checked)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(NOT status STREQUAL "0")
    message(FATAL_ERROR "Abstract VM command failed: ${ARGV}\n${output}${error}")
  endif()
endfunction()
set(spec "${SOURCE}/examples/abstract_calculator.lua")
set(tape "${SOURCE}/examples/abstract_calculator.tape")
checked("${CLI}" --generate -o "${out}" "${spec}")
checked("${CLI}" --generate -o "${out}/reload" "${out}/AbstractCalculator.stk00")
foreach(ext IN ITEMS stk00 c h insncode dispatch)
  checked("${CMAKE_COMMAND}" -E compare_files
    "${out}/AbstractCalculator.${ext}" "${out}/reload/AbstractCalculator.${ext}")
endforeach()
checked("${CC}" ${flags} -std=c99 -Wall -Wextra -Werror -pedantic -I "${out}"
  "${out}/AbstractCalculator.c" "${SOURCE}/tests/abstract.c" -o "${out}/abstract")
checked("${out}/abstract")
checked("${CLI}" --generate -o "${out}" "${SOURCE}/tests/abstract_unsigned.lua")
checked("${CC}" ${flags} -std=c99 -Wall -Wextra -Werror -pedantic -I "${out}"
  "${out}/AbstractUnsigned.c" "${SOURCE}/tests/abstract_unsigned.c" -o "${out}/unsigned")
checked("${out}/unsigned")

# Compile STK-00, not the original Lua file, and execute both C and MachineIR
# outputs on independent state instances with a budget stop and resume.
# The standalone C test above honors CFLAGS (including sanitizers); a native
# image must link as a self-contained DSO, independently of the host flags.
checked("${CLI}" --native --cc "${CC}" --cflag -Wall --cflag -Wextra --cflag -Werror
  --program "${tape}" -o "${out}/native" "${out}/AbstractCalculator.stk00")
file(WRITE "${out}/differential.c" [=[
#include "native/AbstractCalculator.h"
#include <assert.h>

int main(void) {
  AbstractCalculator_vm vm, reference;
  AbstractCalculator_instruction code[32];
  AbstractCalculator_tape tape = {code, 0, 32};
  AbstractCalculator_init(&vm);
  AbstractCalculator_init(&reference);
  assert(AbstractCalculator_compile("PUSH 20 PUSH 2 MUL DUP PUSH 40 EQ JZ &wrong "
                                    "SAVE BIAS LOAD JUMP &end wrong: POP PUSH 0 end: HALT", &tape) == 0);
  assert(AbstractCalculator_run_native(&vm, 3) == -7);
  assert(AbstractCalculator_run(&reference, &tape, 3) == -7);
  assert(vm.vw_pc == reference.vw_pc && vm.vw_pc == 3 && vm.vw_sp == 1 && vm.vw_stack[0] == 40);
  assert(AbstractCalculator_run_native(&vm, 32) == 0);
  assert(AbstractCalculator_run(&reference, &tape, 32) == 0);
  assert(vm.vw_pc == reference.vw_pc && vm.vw_halted && vm.vw_current == NULL);
  assert(vm.acc == reference.acc && vm.acc == 42 && vm.vw_sp == 1 && vm.vw_stack[0] == 42);
  assert(vm.vw_sp == reference.vw_sp && vm.vw_stack[0] == reference.vw_stack[0]);
  return 0;
}
]=])
checked("${CC}" ${flags} -std=c99 -Wall -Wextra -Werror -pedantic
  "${out}/differential.c" "${out}/native/AbstractCalculator.so" -o "${out}/differential")
checked("${out}/differential")
