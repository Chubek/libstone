// Same arguments as dinv_call; returns 1 on success with callee saves intact.
.text
.globl dinv_test_preserve
.type dinv_test_preserve,@function
dinv_test_preserve:
  pushq %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  subq $8, %rsp
  movq $1, %rbp
  movq $2, %rbx
  movq $3, %r12
  movq $4, %r13
  movq $5, %r14
  movq $6, %r15
  call dinv_call@PLT
  testl %eax, %eax
  jne .Lbad
  cmpq $1, %rbp
  jne .Lbad
  cmpq $2, %rbx
  jne .Lbad
  cmpq $3, %r12
  jne .Lbad
  cmpq $4, %r13
  jne .Lbad
  cmpq $5, %r14
  jne .Lbad
  cmpq $6, %r15
  jne .Lbad
  movl $1, %eax
  jmp .Ldone
.Lbad:
  xorl %eax, %eax
.Ldone:
  addq $8, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  ret
.size dinv_test_preserve, .-dinv_test_preserve
.section .note.GNU-stack,"",@progbits
