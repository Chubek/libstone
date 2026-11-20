/* Linux AArch64 static process entry; invoke main using AAPCS64. */
    .section .text.startup,"ax",%progbits
    .p2align 4
    .globl _start
    .type _start,%function
_start:
    mov x29, xzr
    mov x30, xzr
    ldr x0, [sp]
    add x1, sp, #8
    add x2, x1, x0, lsl #3
    add x2, x2, #8
    adrp x9, environ
    str x2, [x9, :lo12:environ]
    bl main
    bl exit
    brk #0
    .size _start, .-_start

    .bss
    .p2align 3
    .globl environ
    .type environ,%object
    .size environ,8
environ:
    .zero 8

    .section .note.GNU-stack,"",%progbits
