/* Linux x86-64 syscall wrappers; errno is process-global, not TLS. */
    .text
    .type __cephyr_syscall_result,@function
__cephyr_syscall_result:
    cmpq $-4095, %rax
    jae 1f
    ret
1:
    negl %eax
    movl %eax, errno(%rip)
    movq $-1, %rax
    ret
    .size __cephyr_syscall_result, .-__cephyr_syscall_result

    .macro CEPHYR_SYSCALL name, number
    .globl \name
    .type \name,@function
\name:
    movq %rcx, %r10          /* SysV argument 4 -> Linux syscall argument 4 */
    movl $\number, %eax
    syscall
    jmp __cephyr_syscall_result
    .size \name, .-\name
    .endm

    CEPHYR_SYSCALL read, 0
    CEPHYR_SYSCALL write, 1
    CEPHYR_SYSCALL open, 2
    CEPHYR_SYSCALL close, 3
    CEPHYR_SYSCALL lseek, 8
    CEPHYR_SYSCALL mmap, 9
    CEPHYR_SYSCALL munmap, 11
    CEPHYR_SYSCALL openat, 257

    .globl _exit
    .type _exit,@function
_exit:
    movl $231, %eax          /* exit_group */
    syscall
    ud2
    .size _exit, .-_exit

    .globl _Exit
    .type _Exit,@function
    .set _Exit, _exit

    .globl abort
    .type abort,@function
abort:
    subq $56, %rsp
    movq $32, 0(%rsp)        /* SIGABRT bit in the kernel signal mask */
    movl $1, %edi            /* SIG_UNBLOCK */
    movq %rsp, %rsi
    xorl %edx, %edx
    movl $8, %r10d
    movl $14, %eax           /* rt_sigprocmask */
    syscall

    movl $39, %eax           /* getpid */
    syscall
    movl %eax, 8(%rsp)
    movl $186, %eax          /* gettid */
    syscall
    movl %eax, 12(%rsp)
    movl 8(%rsp), %edi
    movl %eax, %esi
    movl $6, %edx            /* SIGABRT */
    movl $234, %eax          /* tgkill */
    syscall

    /* If a signal handler returned, reset to SIG_DFL and raise again. */
    movq $0, 16(%rsp)
    movq $0, 24(%rsp)
    movq $0, 32(%rsp)
    movq $0, 40(%rsp)
    movl $6, %edi
    leaq 16(%rsp), %rsi
    xorl %edx, %edx
    movl $8, %r10d
    movl $13, %eax           /* rt_sigaction */
    syscall

    movl 8(%rsp), %edi
    movl 12(%rsp), %esi
    movl $6, %edx
    movl $234, %eax
    syscall
    movl $134, %edi
    jmp _exit
    .size abort, .-abort

    .bss
    .p2align 2
    .globl errno
    .hidden errno
    .type errno,@object
    .size errno,4
errno:
    .zero 4

    .section .note.GNU-stack,"",@progbits
