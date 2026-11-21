/* Linux process entry for benchmark fixtures with no libc dependencies. */
    .text
    .globl _start
_start:
#if defined(__x86_64__)
    call main
    mov %eax, %edi
    mov $60, %eax
    syscall
#elif defined(__aarch64__)
    bl main
    mov x8, #93
    svc #0
#elif defined(__riscv) && __riscv_xlen == 64
    call main
    li a7, 93
    ecall
#else
#error Unsupported benchmark target
#endif
#if defined(__x86_64__)
    .section .note.GNU-stack,"",@progbits
#endif
