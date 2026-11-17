# Linux x86-64; no libc or compiler startup files.
.data
answer: .quad 42
.bss
.balign 16
scratch: .skip 64
.text
.global _start
_start:
    mov answer(%rip), %rdi
    mov $60, %eax
    syscall
.section .note.GNU-stack,"",@progbits
