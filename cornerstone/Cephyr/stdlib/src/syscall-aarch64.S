/* Linux AArch64 syscall wrappers; errno is process-global, not TLS. */
    .text
    .type __cephyr_syscall_result,%function
__cephyr_syscall_result:
    cmn x0, #4095
    b.cs 1f
    ret
1:
    neg w9, w0
    adrp x10, errno
    str w9, [x10, :lo12:errno]
    mov x0, #-1
    ret
    .size __cephyr_syscall_result, .-__cephyr_syscall_result

    .macro CEPHYR_SYSCALL name, number
    .globl \name
    .type \name,%function
\name:
    mov x8, #\number
    svc #0
    b __cephyr_syscall_result
    .size \name, .-\name
    .endm

    CEPHYR_SYSCALL read, 63
    CEPHYR_SYSCALL write, 64
    CEPHYR_SYSCALL close, 57
    CEPHYR_SYSCALL lseek, 62
    CEPHYR_SYSCALL openat, 56
    CEPHYR_SYSCALL mmap, 222
    CEPHYR_SYSCALL munmap, 215

    .globl open
    .type open,%function
open:
    /* open(path, flags, mode) -> openat(AT_FDCWD, path, flags, mode). */
    mov x3, x2
    mov x2, x1
    mov x1, x0
    mov x0, #-100
    b openat
    .size open, .-open

    .globl _exit
    .type _exit,%function
_exit:
    mov x8, #94
    svc #0
    brk #0
    .size _exit, .-_exit

    .globl _Exit
    .type _Exit,%function
    .set _Exit, _exit

    .globl abort
    .type abort,%function
abort:
    sub sp, sp, #48
    mov x9, #32             /* SIGABRT bit in the kernel signal mask */
    str x9, [sp]
    mov x0, #1              /* SIG_UNBLOCK */
    mov x1, sp
    mov x2, xzr
    mov x3, #8
    mov x8, #135            /* rt_sigprocmask */
    svc #0

    mov x8, #172            /* getpid */
    svc #0
    str w0, [sp, #8]
    mov x8, #178            /* gettid */
    svc #0
    str w0, [sp, #12]
    ldr w0, [sp, #8]
    ldr w1, [sp, #12]
    mov x2, #6              /* SIGABRT */
    mov x8, #131            /* tgkill */
    svc #0

    /* If a signal handler returned, reset to SIG_DFL and raise again. */
    stp xzr, xzr, [sp, #16]
    stp xzr, xzr, [sp, #32]
    mov x0, #6
    add x1, sp, #16
    mov x2, xzr
    mov x3, #8
    mov x8, #134            /* rt_sigaction */
    svc #0

    ldr w0, [sp, #8]
    ldr w1, [sp, #12]
    mov x2, #6
    mov x8, #131
    svc #0
    mov x0, #134
    b _exit
    .size abort, .-abort

    .bss
    .p2align 2
    .globl errno
    .hidden errno
    .type errno,%object
    .size errno,4
errno:
    .zero 4

    .section .note.GNU-stack,"",%progbits
