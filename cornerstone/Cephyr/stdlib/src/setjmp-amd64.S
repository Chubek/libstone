/* SysV AMD64 jmp_buf: rbx, rbp, r12-r15, caller's rsp, return address. */
    .text
    .globl __cephyr_setjmp
    .type __cephyr_setjmp,@function
__cephyr_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rdx
    movq %rdx, 48(%rdi)
    movq (%rsp), %rdx
    movq %rdx, 56(%rdi)
    xorl %eax, %eax
    ret
    .size __cephyr_setjmp, .-__cephyr_setjmp

    .globl longjmp
    .type longjmp,@function
longjmp:
    movl %esi, %eax
    movl $1, %edx
    testl %eax, %eax
    cmovzl %edx, %eax
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmp *56(%rdi)
    .size longjmp, .-longjmp

    .section .note.GNU-stack,"",@progbits
