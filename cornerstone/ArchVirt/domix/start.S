#if defined(__aarch64__)
.section .text.boot, "ax"
.global _start
_start:
    mov x1, #0x700000
    mov sp, x1
    bl kernel_main
    hlt #0
#elif defined(__riscv) && __riscv_xlen == 64
.section .text.boot, "ax"
.global _start
_start:
    li sp, 0x700000
    call kernel_main
    ebreak
#elif defined(__x86_64__)
.section .text.boot, "ax"
.global _start
_start:
    cld
    mov $0x700000, %rsp
    call kernel_main
    hlt
#endif
.section .note.GNU-stack,"",%progbits
