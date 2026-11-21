/* Linux RISC-V integer LP64 jmp_buf: s0-s11, sp, ra. */
#if !defined(__riscv_float_abi_soft)
#error "Cephyr's RV64 setjmp requires the soft-float LP64 ABI"
#endif

    .text
    .globl __cephyr_setjmp
    .type __cephyr_setjmp,@function
__cephyr_setjmp:
    sd s0, 0(a0)
    sd s1, 8(a0)
    sd s2, 16(a0)
    sd s3, 24(a0)
    sd s4, 32(a0)
    sd s5, 40(a0)
    sd s6, 48(a0)
    sd s7, 56(a0)
    sd s8, 64(a0)
    sd s9, 72(a0)
    sd s10, 80(a0)
    sd s11, 88(a0)
    sd sp, 96(a0)
    sd ra, 104(a0)
    li a0, 0
    ret
    .size __cephyr_setjmp, .-__cephyr_setjmp

    .globl longjmp
    .type longjmp,@function
longjmp:
    ld s0, 0(a0)
    ld s1, 8(a0)
    ld s2, 16(a0)
    ld s3, 24(a0)
    ld s4, 32(a0)
    ld s5, 40(a0)
    ld s6, 48(a0)
    ld s7, 56(a0)
    ld s8, 64(a0)
    ld s9, 72(a0)
    ld s10, 80(a0)
    ld s11, 88(a0)
    ld sp, 96(a0)
    ld ra, 104(a0)
    mv a0, a1
    bnez a0, 1f
    li a0, 1
1:
    ret
    .size longjmp, .-longjmp

    .section .note.GNU-stack,"",@progbits
