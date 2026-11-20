/* Linux x86-64 static process entry; invoke main using the SysV ABI. */
    .section .text.startup,"ax",@progbits
    .p2align 4
    .globl _start
    .type _start,@function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 8(%rsi,%rdi,8), %rdx
    movq %rdx, environ(%rip)
    andq $-16, %rsp
    call main
    movl %eax, %edi
    call exit
    ud2
    .size _start, .-_start

    .bss
    .p2align 3
    .globl environ
    .type environ,@object
    .size environ,8
environ:
    .zero 8

    .section .note.GNU-stack,"",@progbits
