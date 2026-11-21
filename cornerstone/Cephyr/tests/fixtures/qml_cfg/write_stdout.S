/* Static Linux write(1, data, size), with no target libc or stdio dependency. */
    .text
    .globl cfg_write_stdout
cfg_write_stdout:
#if defined(__x86_64__)
    mov %rsi, %rdx
    mov %rdi, %rsi
    mov $1, %edi
    mov $1, %eax
    syscall
    ret
#elif defined(__aarch64__)
    mov x2, x1
    mov x1, x0
    mov x0, #1
    mov x8, #64
    svc #0
    ret
#elif defined(__riscv) && __riscv_xlen == 64
    mv a2, a1
    mv a1, a0
    li a0, 1
    li a7, 64
    ecall
    ret
#else
#error Unsupported target
#endif
