#ifndef CEPHYR_CLI_EXIT_SYSCALL
#define CEPHYR_CLI_EXIT_SYSCALL 93
#endif

    .section .text
    .globl _start
    .type _start, @function
_start:
    call main
    li a7, CEPHYR_CLI_EXIT_SYSCALL
    ecall
