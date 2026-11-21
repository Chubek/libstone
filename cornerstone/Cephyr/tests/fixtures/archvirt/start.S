/* Enter a freestanding Cephyr program via the DomIOS boot ABI. */
    .section .text.boot,"ax"
    .globl _start
_start:
#if defined(__x86_64__)
    cld
    mov $0x700000, %rsp
    call archvirt_main
    hlt
#elif defined(__aarch64__)
    mov x1, #0x700000
    mov sp, x1
    bl archvirt_main
    hlt #0
#elif defined(__riscv) && __riscv_xlen == 64
    li sp, 0x700000
    call archvirt_main
    ebreak
#else
#error Unsupported target
#endif
    .section .note.GNU-stack,"",%progbits
