#include "AbstractCalculator.h"
#include <assert.h>

static AbstractCalculator_vm run(const char *program, int expected) {
  AbstractCalculator_vm vm;
  AbstractCalculator_instruction code[32];
  AbstractCalculator_tape tape = {code, 0, 32};
  AbstractCalculator_init(&vm);
  assert(AbstractCalculator_compile(program, &tape) == 0);
  assert(AbstractCalculator_run(&vm, &tape, 32) == expected);
  assert(vm.vw_current == NULL);
  return vm;
}

int main(void) {
  AbstractCalculator_vm vm = run("PUSH 27 PUSH 6 SUB PUSH 2 MUL PUSH 3 DIV DUP POP HALT", 0);
  assert(vm.vw_sp == 1 && vm.vw_stack[0] == 14 && vm.vw_halted);
  vm = run("PUSH 20 PUSH 2 MUL DUP PUSH 40 EQ JZ &wrong "
           "SAVE BIAS LOAD JUMP &end wrong: POP PUSH 0 end: HALT", 0);
  assert(vm.acc == 42 && vm.vw_sp == 1 && vm.vw_stack[0] == 42 && vm.vw_halted);
  vm = run("PUSH 1 PUSH 2 SWAP LT JZ &zero PUSH 999 zero: MIN HALT", 0);
  assert(vm.vw_sp == 1 && vm.vw_stack[0] == INT64_MIN);
  vm = run("PUSH 11 PUSH -3 PUSH 7 PUSH 2 INC HALT PUSH 9", 0);
  assert(vm.vw_sp == 4 && vm.vw_stack[0] == 11 && vm.vw_stack[1] == -3
         && vm.vw_stack[2] == 7 && vm.vw_stack[3] == 3);
  vm = run("PUSH -3 PUSH -7 MUL HALT", 0);
  assert(vm.vw_sp == 1 && vm.vw_stack[0] == 21);

  vm = run("ADD HALT", -2);
  assert(vm.vw_pc == 0 && vm.vw_sp == 0);
  vm = run("PUSH 1 PUSH 2 PUSH 3 PUSH 4 DUP HALT", -3);
  assert(vm.vw_pc == 4 && vm.vw_sp == 4);
  vm = run("PUSH 9223372036854775807 PUSH 1 ADD HALT", -10);
  assert(vm.vw_pc == 2 && vm.vw_sp == 2 && vm.vw_stack[0] == INT64_MAX && vm.vw_stack[1] == 1);
  vm = run("MIN PUSH -1 ADD", -10);
  assert(vm.vw_sp == 2 && vm.vw_stack[0] == INT64_MIN && vm.vw_stack[1] == -1);
  vm = run("MIN PUSH 1 SUB HALT", -10);
  assert(vm.vw_sp == 2);
  vm = run("PUSH 9223372036854775807 PUSH -1 SUB", -10);
  assert(vm.vw_sp == 2);
  vm = run("MIN PUSH -1 MUL HALT", -10);
  assert(vm.vw_sp == 2);
  vm = run("PUSH -1 MIN MUL", -10);
  assert(vm.vw_sp == 2);
  vm = run("PUSH 9223372036854775807 PUSH 2 MUL", -10);
  assert(vm.vw_sp == 2);
  vm = run("MIN PUSH -1 DIV", -10);
  assert(vm.vw_sp == 2);
  vm = run("PUSH 42 PUSH 0 DIV", -10);
  assert(vm.vw_sp == 2);
  vm = run("PUSH 9223372036854775807 SAVE BIAS LOAD", -20);
  assert(vm.acc == INT64_MAX && vm.vw_pc == 2 && vm.vw_sp == 0);

  AbstractCalculator_init(&vm);
  AbstractCalculator_PUSH(&vm); /* Direct handlers also validate operand availability. */
  assert(vm.vw_error == -4 && vm.vw_sp == 0);
  return 0;
}
