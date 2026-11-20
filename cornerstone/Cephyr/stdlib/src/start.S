/*
 * RV64 Linux process entry point for statically linked LP64 programs.
 *
 * Linux enters with argc followed by argv, envp, and the auxiliary vector on
 * the stack.  The C main function receives envp as a harmless third argument;
 * ordinary two-argument main functions simply ignore it.
 */

    .section .text.startup,"ax",@progbits
    .p2align 2
    .globl _start
    .type _start,@function
_start:
    /* Linker relaxation may make generated code depend on an initialized gp. */
    .option push
    .option norelax
    la gp, __global_pointer$
    .option pop

    mv t0, sp
    ld a0, 0(t0)             /* argc */
    addi a1, t0, 8           /* argv */
    slli t1, a0, 3
    add a2, a1, t1
    addi a2, a2, 8           /* envp = argv + argc + 1 */
    lla t2, environ
    sd a2, 0(t2)

    andi sp, t0, -16
    li s0, 0
    call main
    call exit

    /* exit is noreturn; do not run into another text section if it is broken. */
1:
    j 1b
    .size _start, .-_start

    .section .bss,"aw",@nobits
    .p2align 3
    .globl environ
    .type environ,@object
    .size environ,8
environ:
    .zero 8

    .section .note.GNU-stack,"",@progbits
