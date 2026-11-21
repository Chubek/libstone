/* A direct probe deliberately clobbers nonvolatile registers before longjmp.
   Normal C code (especially around returns_twice) might use only spills. */
    .text
    .globl cephyr_test_jump_registers
#if defined(__x86_64__)
    .type cephyr_test_jump_registers,@function
cephyr_test_jump_registers:
    pushq %rbp
    pushq %r15
    pushq %rdi
    movq $17, %rbp
    movq $31, %r15
    call __cephyr_setjmp
    testl %eax, %eax
    jne 1f
    movq (%rsp), %rdi
    subq $64, %rsp
    xorq %rbp, %rbp
    xorq %r15, %r15
    movl $19, %esi
    call longjmp
    ud2
1:
    cmpl $19, %eax
    jne 2f
    cmpq $17, %rbp
    jne 2f
    cmpq $31, %r15
    jne 2f
    xorl %eax, %eax
    jmp 3f
2:
    movl $1, %eax
3:
    addq $8, %rsp
    popq %r15
    popq %rbp
    ret
#elif defined(__aarch64__)
    .type cephyr_test_jump_registers,%function
cephyr_test_jump_registers:
    stp x29, x30, [sp, #-48]!
    stp x0, x19, [sp, #16]
    stp d8, d15, [sp, #32]
    mov x19, #17
    mov x29, #31
    fmov d8, #1.0
    fmov d15, #2.0
    bl __cephyr_setjmp
    cbnz w0, 1f
    ldr x0, [sp, #16]
    sub sp, sp, #64
    mov x19, xzr
    mov x29, xzr
    fmov d8, xzr
    fmov d15, xzr
    mov w1, #19
    bl longjmp
    brk #0
1:
    cmp w0, #19
    b.ne 2f
    cmp x19, #17
    b.ne 2f
    cmp x29, #31
    b.ne 2f
    fmov d0, #1.0
    fcmp d8, d0
    b.ne 2f
    fmov d0, #2.0
    fcmp d15, d0
    b.ne 2f
    mov w0, #0
    b 3f
2:
    mov w0, #1
3:
    ldp d8, d15, [sp, #32]
    ldr x19, [sp, #24]
    ldp x29, x30, [sp], #48
    ret
#else
    .type cephyr_test_jump_registers,@function
cephyr_test_jump_registers:
    addi sp, sp, -32
    sd a0, 0(sp)
    sd s0, 8(sp)
    sd s11, 16(sp)
    sd ra, 24(sp)
    li s0, 17
    li s11, 31
    call __cephyr_setjmp
    bnez a0, 1f
    ld a0, 0(sp)
    addi sp, sp, -64
    li s0, 0
    li s11, 0
    li a1, 19
    call longjmp
    unimp
1:
    li t0, 19
    bne a0, t0, 2f
    li t0, 17
    bne s0, t0, 2f
    li t0, 31
    bne s11, t0, 2f
    li a0, 0
    j 3f
2:
    li a0, 1
3:
    ld s0, 8(sp)
    ld s11, 16(sp)
    ld ra, 24(sp)
    addi sp, sp, 32
    ret
#endif
    .size cephyr_test_jump_registers, .-cephyr_test_jump_registers

    .section .note.GNU-stack,""
