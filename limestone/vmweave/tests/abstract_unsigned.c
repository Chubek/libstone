#include "AbstractUnsigned.h"
#include <assert.h>

int main(void) {
  AbstractUnsigned_vm vm;
  AbstractUnsigned_instruction code[8];
  AbstractUnsigned_tape tape = {code, 0, 8};
  AbstractUnsigned_init(&vm);
  assert(AbstractUnsigned_compile("MAX WRAP HALT", &tape) == 0);
  assert(AbstractUnsigned_run(&vm, &tape, 8) == 0);
  assert(vm.vw_sp == 1 && vm.vw_stack[0] == 0);
  AbstractUnsigned_init(&vm);
  assert(AbstractUnsigned_compile("MAX WRAP RESTORE SQUARE MIX HALT", &tape) == 0);
  assert(AbstractUnsigned_run(&vm, &tape, 8) == 0);
  assert(vm.vw_sp == 1 && vm.vw_stack[0] == 3);
  AbstractUnsigned_init(&vm);
  assert(AbstractUnsigned_compile("MAX TRAP HALT", &tape) == 0);
  assert(AbstractUnsigned_run(&vm, &tape, 8) == -10);
  assert(vm.vw_pc == 1 && vm.vw_sp == 1 && vm.vw_stack[0] == UINT64_MAX);
  assert(vm.last == UINT64_MAX);
  return 0;
}
