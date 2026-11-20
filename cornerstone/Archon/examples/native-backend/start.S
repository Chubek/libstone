/* Minimal Linux/RV64 process entry with no target libc. */
    .text
    .globl _start
    .type _start, @function
_start:
    call target_main
    li a7, 93                    /* Linux exit(a0) */
    ecall
    unimp
    .size _start, .-_start
    .section .note.GNU-stack,"",@progbits
