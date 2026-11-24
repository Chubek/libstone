#pragma once
#include "LexerGenerator.hpp"
#include <cstddef>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace firestone::lr {

// Target: x86-64 System V ELF, LP64, GNU assembler (AT&T syntax). No libc calls;
// tables are read-only and addressed relative to RIP (ordinary small code model).
// Same signature, status codes, and pointer contract as emitC99. Uses 16-bit states.
inline std::string emitAssembly(const DFA& dfa, std::string_view name = "firestone_lr_scan") {
    detail::require_scanner_name(name);
    detail::require_nonnullable(dfa);
    const auto& states = dfa.states();
    if (states.size() > 65535)
        throw std::length_error("lexer assembly codegen: more than 65535 states");
    const auto alphabet = dfa.byte_classes();
    const auto saved_columns = byte_count - alphabet.representatives.size();
    const auto compressed = saved_columns != 0 && states.size() > byte_count / (2 * saved_columns);
    const auto width = compressed ? alphabet.representatives.size() : byte_count;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    // r10/r11: table bases, r12: token start, r13: probe cursor, r14: DFA state,
    // rbx: winning acceptance record (or zero), r15: last accepting cursor.
    out << ".text\n.globl " << name << "\n.type " << name << ",@function\n" << name << R"(:
    .cfi_startproc
    testq %rdx,%rdx
    jz 8f
    testq %rcx,%rcx
    jz 8f
    testq %r8,%r8
    jz 8f
    testq %r9,%r9
    jz 8f
    movq (%rdx),%rax
    cmpq %rsi,%rax
    ja 8f
    testq %rdi,%rdi
    jnz 1f
    testq %rsi,%rsi
    jnz 8f
1:
    pushq %rbx
    .cfi_adjust_cfa_offset 8
    .cfi_offset %rbx,-16
    pushq %r12
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r12,-24
    pushq %r13
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r13,-32
    pushq %r14
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r14,-40
    pushq %r15
    .cfi_adjust_cfa_offset 8
    .cfi_offset %r15,-48
    leaq 9f(%rip),%r10
    leaq 10f(%rip),%r11
    movq %rax,%r12
2:
    cmpq %rsi,%r12
    je 6f
    movq %r12,%r13
    xorl %ebx,%ebx
    movl $)" << dfa.start() << R"(,%r14d
3:
    cmpq %rsi,%r13
    je 4f
    movzbl (%rdi,%r13),%eax
)";
    if (compressed)
        out << "    movzbl -256(%r10,%rax),%eax\n    imulq $" << width << ",%r14,%r14\n";
    else out << "    shlq $8,%r14\n";
    out << R"(    addq %rax,%r14
    movzwl (%r10,%r14,2),%r14d
    testl %r14d,%r14d
    jz 4f
    decq %r14
    incq %r13
    movq %r14,%rax
    shlq $4,%rax
    addq %r11,%rax
    cmpq $0,8(%rax)
    je 3b
    movq %rax,%rbx
    movq %r13,%r15
    jmp 3b
4:
    testq %rbx,%rbx
    jz 5f
    cmpq $2,8(%rbx)
    jne 1f
    movq %r15,%r12
    jmp 2b
1:
    movq %r15,(%rdx)
    movq (%rbx),%rax
    movq %rax,(%rcx)
    movq %r12,(%r8)
    subq %r12,%r15
    movq %r15,(%r9)
    movl $1,%eax
    jmp 7f
5:
    leaq 1(%r12),%rax
    movq %rax,(%rdx)
    movq $0,(%rcx)
    movq %r12,(%r8)
    movq $1,(%r9)
    movl $-1,%eax
    jmp 7f
6:
    movq %r12,(%rdx)
    movq $0,(%rcx)
    movq %r12,(%r8)
    movq $0,(%r9)
    xorl %eax,%eax
7:
    popq %r15
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r15
    popq %r14
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r14
    popq %r13
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r13
    popq %r12
    .cfi_adjust_cfa_offset -8
    .cfi_restore %r12
    popq %rbx
    .cfi_adjust_cfa_offset -8
    .cfi_restore %rbx
    ret
8:
    movl $-2,%eax
    ret
    .cfi_endproc
)";
    out << ".size " << name << ",.-" << name << "\n.section .rodata\n.p2align 1\n";
    if (compressed) {
        // Exactly 256 bytes immediately before the transition table at 9.
        for (std::size_t byte = 0; byte < byte_count; ++byte) {
            out << (byte % 16 == 0 ? "    .byte " : ",") << static_cast<unsigned int>(alphabet.index[byte]);
            if (byte % 16 == 15) out << '\n';
        }
    }
    out << "9:\n";
    for (const auto& state : states) {
        for (std::size_t cls = 0; cls < width; ++cls) {
            auto target = state.transitions[compressed ? alphabet.representatives[cls] : cls];
            out << (cls % 16 == 0 ? "    .word " : ",") << (target == no_state ? 0 : target + 1);
            if (cls % 16 == 15 || cls + 1 == width) out << '\n';
        }
    }
    out << ".p2align 3\n10:\n";
    for (const auto& state : states) {
        if (state.acceptance) {
            detail::require_codegen_id(state.acceptance->token);
            out << "    .quad " << state.acceptance->token << ',' << (state.acceptance->skip ? 2 : 1) << '\n';
        } else out << "    .quad 0,0\n";
    }
    // GNU as can warn if concatenated scanners specify this section type twice.
    out << ".ifndef .Lfirestone_lr_stack_note\n"
           ".set .Lfirestone_lr_stack_note,1\n"
           ".section .note.GNU-stack,\"\",@progbits\n"
           ".endif\n";
    return out.str();
}

inline std::string emitAssembly(const Lexer& lexer, std::string_view name = "firestone_lr_scan") {
    return emitAssembly(lexer.dfa(), name);
}

} // namespace firestone::lr
