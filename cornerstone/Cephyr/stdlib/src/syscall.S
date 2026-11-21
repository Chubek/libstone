/*
 * Direct Linux RV64 syscall wrappers.
 *
 * Linux reports errors in a0 as -errno.  Public wrappers translate that to
 * -1 and update Cephyr's process-global errno.  This deliberately makes no
 * thread-local errno claim.
 */

    .section .text,"ax",@progbits
    .p2align 2

    .type __cephyr_syscall_result,@function
__cephyr_syscall_result:
    li t0, -4095
    bgeu a0, t0, 1f
    ret
1:
    neg t1, a0
    lla t0, errno
    sw t1, 0(t0)
    li a0, -1
    ret
    .size __cephyr_syscall_result, .-__cephyr_syscall_result

    .macro CEPHYR_SYSCALL name, number
    .globl \name
    .type \name,@function
\name:
    li a7, \number
    ecall
    j __cephyr_syscall_result
    .size \name, .-\name
    .endm

    /* asm-generic Linux syscall numbers used by RV64. */
    CEPHYR_SYSCALL read, 63
    CEPHYR_SYSCALL write, 64
    CEPHYR_SYSCALL close, 57
    CEPHYR_SYSCALL lseek, 62
    CEPHYR_SYSCALL openat, 56
    CEPHYR_SYSCALL mmap, 222
    CEPHYR_SYSCALL munmap, 215
    CEPHYR_SYSCALL __cephyr_ioctl, 29
    CEPHYR_SYSCALL __cephyr_clock_gettime, 113
    CEPHYR_SYSCALL __cephyr_unlinkat, 35
    CEPHYR_SYSCALL __cephyr_renameat, 276 /* renameat2; flags argument is zero */
    CEPHYR_SYSCALL __cephyr_getrandom, 278

    .globl open
    .type open,@function
open:
    /* open(path, flags, mode) is openat(AT_FDCWD, path, flags, mode). */
    mv a3, a2
    mv a2, a1
    mv a1, a0
    li a0, -100
    li a7, 56
    ecall
    j __cephyr_syscall_result
    .size open, .-open

    .globl _exit
    .type _exit,@function
_exit:
    li a7, 94               /* exit_group */
    ecall
1:
    j 1b
    .size _exit, .-_exit

    .globl _Exit
    .type _Exit,@function
    .set _Exit, _exit

    /*
     * ISO C abort.  Unblock SIGABRT, raise it once so an installed handler can
     * run, then restore the default disposition and raise it again.  A final
     * exit_group(128 + SIGABRT) guarantees that abort never returns if signal
     * delivery fails.
     */
    .globl abort
    .type abort,@function
abort:
    addi sp, sp, -64

    li t0, 32               /* signal set bit SIGABRT - 1 */
    sd t0, 0(sp)
    li a0, 1                /* SIG_UNBLOCK */
    mv a1, sp
    li a2, 0
    li a3, 8                /* RV64 kernel sigset size */
    li a7, 135              /* rt_sigprocmask */
    ecall

    li a7, 172              /* getpid */
    ecall
    sd a0, 8(sp)
    li a7, 178              /* gettid */
    ecall
    sd a0, 16(sp)

    ld a0, 8(sp)
    ld a1, 16(sp)
    li a2, 6                /* SIGABRT */
    li a7, 131              /* tgkill */
    ecall

    /* Zero enough storage for the RV64 kernel sigaction (including padding). */
    sd zero, 24(sp)
    sd zero, 32(sp)
    sd zero, 40(sp)
    sd zero, 48(sp)
    li a0, 6
    addi a1, sp, 24
    li a2, 0
    li a3, 8
    li a7, 134              /* rt_sigaction */
    ecall

    ld a0, 8(sp)
    ld a1, 16(sp)
    li a2, 6
    li a7, 131
    ecall

    li a0, 134
    li a7, 94
    ecall
1:
    j 1b
    .size abort, .-abort

    .section .sbss,"aw",@nobits
    .p2align 2
    .globl errno
    .hidden errno
    .type errno,@object
    .size errno,4
errno:
    .zero 4

    .section .note.GNU-stack,"",@progbits
