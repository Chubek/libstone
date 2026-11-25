#include "ScalarGAS.h"

#include <inttypes.h>

/* SysV volatile registers; %rcx also provides %cl, and DIV uses %rdx:%rax. */
const char an_gas_a[] = "%rax", an_gas_b[] = "%rcx", an_gas_call_target[] = "%r11";

void an_gas_enter(ANGas *gas, unsigned size, int frame_record)
{
    (void)frame_record; /* CALL already saved the return address. */
    while (size >= 4096u) {
        an_gas_print(gas, "\tsubq $4096, %%rsp\n\tmovq $0, (%%rsp)\n");
        size -= 4096u;
    }
    if (size)
        an_gas_print(gas, "\tsubq $%u, %%rsp\n", size);
    if (gas->base_save >= 0) {
        an_gas_stack_store(gas, "%rbp", (unsigned)gas->base_save);
        an_gas_print(gas, "\tmovq %%rsp, %%rbp\n");
        gas->frame_base = "%rbp";
    }
}

void an_gas_leave(ANGas *gas, unsigned size, int frame_record)
{
    (void)frame_record;
    if (gas->base_save >= 0)
        an_gas_print(gas, "\tmovq %%rbp, %%rsp\n\tmovq %d(%%rsp), %%rbp\n", gas->base_save);
    if (size)
        an_gas_print(gas, "\taddq $%u, %%rsp\n", size);
    an_gas_print(gas, "\tret\n");
}

void an_gas_stack_load(ANGas *gas, const char *dst, unsigned offset)
{
    an_gas_print(gas, "\tmovq %u(%s), %s\n", offset, gas->frame_base, dst);
}

void an_gas_stack_store(ANGas *gas, const char *src, unsigned offset)
{
    an_gas_print(gas, "\tmovq %s, %u(%s)\n", src, offset, gas->frame_base);
}

void an_gas_stack_alloc(ANGas *gas, unsigned outgoing)
{
    unsigned loop = gas->next_label++, tail = gas->next_label++, invalid = gas->next_label++;
    unsigned done = gas->next_label++;
    an_gas_print(gas, "\ttestq %%rax, %%rax\n\tjz .Larchon_%zu_%u\n"
        "\taddq $15, %%rax\n\tjb .Larchon_%zu_%u\n\tandq $-16, %%rax\n",
        gas->function, invalid, gas->function, invalid);
    an_gas_label(gas, loop);
    an_gas_print(gas, "\tcmpq $4097, %%rax\n\tjb .Larchon_%zu_%u\n"
        "\tsubq $4096, %%rsp\n\tmovq $0, (%%rsp)\n\tsubq $4096, %%rax\n",
        gas->function, tail);
    an_gas_jump(gas, loop);
    an_gas_label(gas, tail);
    an_gas_print(gas, "\tsubq %%rax, %%rsp\n\tmovq $0, (%%rsp)\n"
        "\tleaq %u(%%rsp), %%rax\n", outgoing);
    an_gas_jump(gas, done);
    an_gas_label(gas, invalid);
    an_gas_trap(gas);
    an_gas_label(gas, done);
}

void an_gas_stack_free(ANGas *gas)
{
    unsigned invalid = gas->next_label++, done = gas->next_label++;
    an_gas_print(gas, "\taddq $15, %%rax\n\tjb .Larchon_%zu_%u\n"
        "\tandq $-16, %%rax\n\taddq %%rsp, %%rax\n\tjb .Larchon_%zu_%u\n"
        "\tcmpq %%rax, %%rbp\n\tjb .Larchon_%zu_%u\n\tmovq %%rax, %%rsp\n",
        gas->function, invalid, gas->function, invalid, gas->function, invalid);
    an_gas_jump(gas, done);
    an_gas_label(gas, invalid);
    an_gas_trap(gas);
    an_gas_label(gas, done);
}

void an_gas_fp_load(ANGas *gas, ArchonMIRType type, unsigned reg, unsigned offset)
{
    an_gas_print(gas, "\tmovs%c %u(%s), %%xmm%u\n",
        type == ARCHON_MIR_F32 ? 's' : 'd', offset, gas->frame_base, reg);
}

void an_gas_fp_store(ANGas *gas, ArchonMIRType type, unsigned reg, unsigned offset)
{
    int single = type == ARCHON_MIR_F32;
    an_gas_print(gas, "\tmov%c %%xmm%u, %s\n", single ? 'd' : 'q', reg,
        single ? "%eax" : "%rax");
    an_gas_stack_store(gas, an_gas_a, offset);
}

void an_gas_local_addr(ANGas *gas, unsigned offset)
{
    an_gas_print(gas, "\tleaq %u(%s), %%rax\n", offset, gas->frame_base);
}

void an_amd64_save_varargs(ANGas *gas, unsigned save, unsigned gp, unsigned fp)
{
    unsigned i, skip;
    for (i = gp; i < an_gas_abi.argc; ++i)
        an_gas_stack_store(gas, an_gas_abi.args[i], save + i * 8);
    if (fp == an_gas_abi.fp_argc)
        return;
    /* AL is still the incoming vector-register count upper bound. */
    skip = gas->next_label++;
    an_gas_print(gas, "\ttestb %%al, %%al\n\tjz .Larchon_%zu_%u\n", gas->function, skip);
    for (i = fp; i < an_gas_abi.fp_argc; ++i)
        an_gas_print(gas, "\tmovaps %%xmm%u, %u(%s)\n", i, save + 48 + i * 16, gas->frame_base);
    an_gas_label(gas, skip);
}

void an_amd64_va_start(ANGas *gas, unsigned save, unsigned overflow, unsigned gp, unsigned fp)
{
    /* RAX contains the destination va_list address. Its pointers refer to
     * the initial caller stack and to our entry-time register save area. */
    an_gas_print(gas, "\tmovl $%u, 0(%%rax)\n\tmovl $%u, 4(%%rax)\n"
        "\tleaq %u(%s), %%rcx\n\tmovq %%rcx, 8(%%rax)\n"
        "\tleaq %u(%s), %%rcx\n\tmovq %%rcx, 16(%%rax)\n",
        gp * 8, 48 + fp * 16, overflow, gas->frame_base, save, gas->frame_base);
}

void an_gas_symbol_addr(ANGas *gas, const char *symbol)
{
    if (gas->out->pic)
        an_gas_print(gas, "\tmovq %s@GOTPCREL(%%rip), %%rax\n", symbol);
    else
        an_gas_print(gas, "\tleaq %s(%%rip), %%rax\n", symbol);
}

void an_gas_constant(ANGas *gas, uint64_t value)
{
    an_gas_print(gas, "\tmovabsq $0x%" PRIx64 ", %%rax\n", value);
}

void an_gas_convert(ANGas *gas, ArchonMIROpcode op, unsigned width)
{
    if (width != 64u)
        an_gas_print(gas, "\tshlq $%u, %%rax\n\t%s $%u, %%rax\n",
            64u - width, op == ARCHON_MIR_SEXT ? "sarq" : "shrq", 64u - width);
}

void an_gas_shift_left_imm(ANGas *gas, unsigned shift)
{
    if (shift)
        an_gas_print(gas, "\tshlq $%u, %%rax\n", shift);
}

void an_gas_memory(ANGas *gas, ArchonMIROpcode op, int disp)
{
    const char *name = NULL, *reg = "%rax";
    int store = 0;
    switch (op) {
    case ARCHON_MIR_LOAD_S8: name = "movsbq"; break;
    case ARCHON_MIR_LOAD_U8: name = "movzbq"; break;
    case ARCHON_MIR_LOAD_S16: name = "movswq"; break;
    case ARCHON_MIR_LOAD_U16: name = "movzwq"; break;
    case ARCHON_MIR_LOAD_S32: name = "movslq"; break;
    case ARCHON_MIR_LOAD_U32: name = "movl"; reg = "%eax"; break;
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
        name = "movq";
        break;
    case ARCHON_MIR_STORE8: name = "movb"; reg = "%cl"; store = 1; break;
    case ARCHON_MIR_STORE16: name = "movw"; reg = "%cx"; store = 1; break;
    case ARCHON_MIR_STORE32: name = "movl"; reg = "%ecx"; store = 1; break;
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
        name = "movq"; reg = "%rcx"; store = 1;
        break;
    default:
        gas->out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    if (store)
        an_gas_print(gas, "\t%s %s, %d(%%rax)\n", name, reg, disp);
    else
        an_gas_print(gas, "\t%s %d(%%rax), %s\n", name, disp, reg);
}

static void call_fp_count(ANGas *gas, unsigned count)
{
    /* Set the SysV variadic upper bound for every call. A producer using a
     * fixed signature alias remains responsible for default promotions. */
    if (count)
        an_gas_print(gas, "\tmovl $%u, %%eax\n", count);
    else
        an_gas_print(gas, "\txorl %%eax, %%eax\n");
}

void an_gas_call(ANGas *gas, const char *symbol, unsigned fp_args)
{
    call_fp_count(gas, fp_args);
    an_gas_print(gas, "\tcall %s%s\n", symbol, gas->out->pic ? "@PLT" : "");
}

void an_gas_call_indirect(ANGas *gas, unsigned fp_args)
{
    call_fp_count(gas, fp_args);
    an_gas_print(gas, "\tcall *%s\n", an_gas_call_target);
}

static void jump(ANGas *gas, const char *mnemonic, unsigned label)
{
    an_gas_print(gas, "\t%s .Larchon_%zu_%u\n", mnemonic, gas->function, label);
}

void an_gas_jump(ANGas *gas, unsigned label)
{
    jump(gas, "jmp", label);
}

void an_gas_branch_nonzero(ANGas *gas, unsigned label)
{
    an_gas_print(gas, "\ttestq %%rax, %%rax\n");
    jump(gas, "jnz", label);
}

void an_gas_trap(ANGas *gas)
{
    unsigned halt = gas->next_label++;
    an_gas_print(gas, "\tint3\n");
    an_gas_label(gas, halt);
    an_gas_jump(gas, halt);
}

static void fp_operand(ANGas *gas, int single, unsigned operand)
{
    const char *gp = operand ? (single ? "%ecx" : "%rcx") : (single ? "%eax" : "%rax");
    an_gas_print(gas, "\tmov%c %s, %%xmm%u\n", single ? 'd' : 'q', gp, operand);
}

static void fp_result(ANGas *gas, int single)
{
    an_gas_print(gas, "\tmov%c %%xmm0, %s\n", single ? 'd' : 'q', single ? "%eax" : "%rax");
}

void an_gas_float(ANGas *gas, ArchonMIROpcode op, ArchonMIRType src, ArchonMIRType dst)
{
    const int single = src == ARCHON_MIR_F32;
    const int dst_single = dst == ARCHON_MIR_F32;
    const char suffix = single ? 's' : 'd', dst_suffix = dst_single ? 's' : 'd';
    const char *mnemonic;
    unsigned ordinary, done;
    switch (op) {
    case ARCHON_MIR_FNEG:
        /* A sign-bit operation preserves zeros and NaN payloads. */
        if (single)
            an_gas_print(gas, "\txorl $0x80000000, %%eax\n");
        else
            an_gas_print(gas, "\tbtcq $63, %%rax\n");
        return;
    case ARCHON_MIR_SITOFP:
        an_gas_print(gas, "\tcvtsi2s%cq %%rax, %%xmm0\n", dst_suffix);
        break;
    case ARCHON_MIR_UITOFP:
        ordinary = gas->next_label++;
        done = gas->next_label++;
        an_gas_print(gas, "\ttestq %%rax, %%rax\n");
        jump(gas, "jns", ordinary);
        /* SSE2 has no u64 conversion. For the high half, halve with a
         * sticky low bit, perform the only rounding in signed conversion,
         * and double exactly (the halved value is far below overflow). */
        an_gas_print(gas, "\tmovq %%rax, %%rcx\n\tshrq $1, %%rax\n"
            "\tandl $1, %%ecx\n\torq %%rcx, %%rax\n"
            "\tcvtsi2s%cq %%rax, %%xmm0\n\tadds%c %%xmm0, %%xmm0\n",
            dst_suffix, dst_suffix);
        an_gas_jump(gas, done);
        an_gas_label(gas, ordinary);
        an_gas_print(gas, "\tcvtsi2s%cq %%rax, %%xmm0\n", dst_suffix);
        an_gas_label(gas, done);
        break;
    case ARCHON_MIR_FCAST:
        fp_operand(gas, single, 0);
        an_gas_print(gas, "\tcvts%c2s%c %%xmm0, %%xmm0\n", suffix, dst_suffix);
        break;
    case ARCHON_MIR_FPTOSI:
        fp_operand(gas, single, 0);
        an_gas_print(gas, "\tcvtts%c2siq %%xmm0, %%rax\n", suffix);
        return;
    case ARCHON_MIR_FPTOUI:
        ordinary = gas->next_label++;
        done = gas->next_label++;
        fp_operand(gas, single, 0);
        /* Compare with exact 2^63. On the upper half of the defined
         * domain, subtraction is exact and the remainder fits in i64. */
        if (single)
            an_gas_print(gas, "\tmovl $0x5f000000, %%ecx\n");
        else
            an_gas_print(gas, "\tmovabsq $0x43e0000000000000, %%rcx\n");
        fp_operand(gas, single, 1);
        an_gas_print(gas, "\tucomis%c %%xmm1, %%xmm0\n", suffix);
        jump(gas, "jb", ordinary);
        an_gas_print(gas, "\tsubs%c %%xmm1, %%xmm0\n"
            "\tcvtts%c2siq %%xmm0, %%rax\n\tbtsq $63, %%rax\n", suffix, suffix);
        an_gas_jump(gas, done);
        an_gas_label(gas, ordinary);
        an_gas_print(gas, "\tcvtts%c2siq %%xmm0, %%rax\n", suffix);
        an_gas_label(gas, done);
        return;
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
        fp_operand(gas, single, 0);
        fp_operand(gas, single, 1);
        mnemonic = op == ARCHON_MIR_FEQ ? "sete" : op == ARCHON_MIR_FLT ? "setb" : "setbe";
        /* Ordered predicates exclude the unordered (PF=1) case; unlike
         * signed zeros, NaNs never compare equal to themselves. */
        an_gas_print(gas, "\t%scomis%c %%xmm1, %%xmm0\n",
            op == ARCHON_MIR_FEQ ? "u" : "", suffix);
        an_gas_print(gas, "\t%s %%al\n\tsetnp %%dl\n\tandb %%dl, %%al\n"
            "\tmovzbl %%al, %%eax\n", mnemonic);
        return;
    case ARCHON_MIR_FADD:
    case ARCHON_MIR_FSUB:
    case ARCHON_MIR_FMUL:
    case ARCHON_MIR_FDIV:
        fp_operand(gas, single, 0);
        fp_operand(gas, single, 1);
        mnemonic = op == ARCHON_MIR_FADD ? "add" : op == ARCHON_MIR_FSUB ? "sub" :
            op == ARCHON_MIR_FMUL ? "mul" : "div";
        an_gas_print(gas, "\t%ss%c %%xmm1, %%xmm0\n", mnemonic, suffix);
        break;
    default:
        gas->out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    fp_result(gas, dst_single);
}

void an_amd64_x87(ANGas *gas, ArchonMIROpcode op, ArchonMIRType b, ArchonMIRType dst)
{
    const char *mnemonic;
    unsigned ordinary, done;
    /* Keep the x87 stack balanced across every MIR instruction. RAX and RCX
     * carry addresses or raw scalar bits according to the operation. */
    switch (op) {
    case ARCHON_MIR_AMD64_X87_ADD:
    case ARCHON_MIR_AMD64_X87_SUB:
    case ARCHON_MIR_AMD64_X87_MUL:
    case ARCHON_MIR_AMD64_X87_DIV:
        /* Both operands are loaded before overwriting the lhs. GAS AT&T
         * fsubrp/fdivrp encode Intel fsubp/fdivp st(1), st. */
        mnemonic = op == ARCHON_MIR_AMD64_X87_ADD ? "faddp" :
                   op == ARCHON_MIR_AMD64_X87_SUB ? "fsubrp" :
                   op == ARCHON_MIR_AMD64_X87_MUL ? "fmulp" : "fdivrp";
        an_gas_print(gas, "\tfldt (%%rax)\n\tfldt (%%rcx)\n\t%s %%st, %%st(1)\n\tfstpt (%%rax)\n",
            mnemonic);
        return;
    case ARCHON_MIR_AMD64_X87_EQ:
    case ARCHON_MIR_AMD64_X87_LT:
    case ARCHON_MIR_AMD64_X87_LE:
        mnemonic = op == ARCHON_MIR_AMD64_X87_EQ ? "sete" :
                   op == ARCHON_MIR_AMD64_X87_LT ? "setb" : "setbe";
        /* Equality is quiet for qNaNs; ordered relations are signaling.
         * Exclude unordered (PF=1) from all three predicates. */
        an_gas_print(gas, "\tfldt (%%rcx)\n\tfldt (%%rax)\n\tf%scomip %%st(1), %%st\n"
            "\tfstp %%st(0)\n\t%s %%al\n\tsetnp %%dl\n\tandb %%dl, %%al\n\tmovzbl %%al, %%eax\n",
            op == ARCHON_MIR_AMD64_X87_EQ ? "u" : "", mnemonic);
        return;
    case ARCHON_MIR_AMD64_X87_FROM_FP:
        an_gas_print(gas, "\tsubq $16, %%rsp\n\tmovq %%rcx, (%%rsp)\n"
            "\tfld%c (%%rsp)\n\tfstpt (%%rax)\n\taddq $16, %%rsp\n",
            b == ARCHON_MIR_F32 ? 's' : 'l');
        return;
    case ARCHON_MIR_AMD64_X87_TO_FP:
        /* Direct store to the nominal format uses the x87 rounding mode,
         * with no intermediate binary64 rounding for a float result. */
        an_gas_print(gas, "\tfldt (%%rax)\n\tsubq $16, %%rsp\n\tfstp%c (%%rsp)\n"
            "\tmov%c (%%rsp), %s\n\taddq $16, %%rsp\n",
            dst == ARCHON_MIR_F32 ? 's' : 'l', dst == ARCHON_MIR_F32 ? 'l' : 'q',
            dst == ARCHON_MIR_F32 ? "%eax" : "%rax");
        return;
    case ARCHON_MIR_AMD64_X87_FROM_SI:
    case ARCHON_MIR_AMD64_X87_FROM_UI:
        an_gas_print(gas, "\tsubq $16, %%rsp\n\tmovq %%rcx, (%%rsp)\n\tfildq (%%rsp)\n");
        if (op == ARCHON_MIR_AMD64_X87_FROM_UI) {
            ordinary = gas->next_label++;
            an_gas_print(gas, "\ttestq %%rcx, %%rcx\n");
            jump(gas, "jns", ordinary);
            /* fildq is signed. Add exact 2^64 to its negative result:
             * every uint64_t is representable at x87's 64-bit precision. */
            an_gas_print(gas, "\tmovabsq $0x43f0000000000000, %%rdx\n"
                "\tmovq %%rdx, (%%rsp)\n\tfaddl (%%rsp)\n");
            an_gas_label(gas, ordinary);
        }
        an_gas_print(gas, "\tfstpt (%%rax)\n\taddq $16, %%rsp\n");
        return;
    case ARCHON_MIR_AMD64_X87_TO_SI:
    case ARCHON_MIR_AMD64_X87_TO_UI:
        an_gas_print(gas, "\tfldt (%%rax)\n\tsubq $16, %%rsp\n");
        if (op == ARCHON_MIR_AMD64_X87_TO_UI) {
            ordinary = gas->next_label++;
            done = gas->next_label++;
            /* Reduce the upper half of the defined unsigned domain to
             * signed range using exact subtraction of 2^63. */
            an_gas_print(gas, "\tmovabsq $0x43e0000000000000, %%rcx\n"
                "\tmovq %%rcx, (%%rsp)\n\tfldl (%%rsp)\n\tfxch %%st(1)\n"
                "\txorl %%edx, %%edx\n\tfucomi %%st(1), %%st\n");
            jump(gas, "jb", ordinary);
            an_gas_print(gas, "\tfsubp %%st, %%st(1)\n\tmovabsq $0x8000000000000000, %%rdx\n");
            an_gas_jump(gas, done);
            an_gas_label(gas, ordinary);
            an_gas_print(gas, "\tfstp %%st(1)\n");
            an_gas_label(gas, done);
        }
        /* C requires truncation regardless of the active rounding mode.
         * Use baseline AMD64 x87 (FISTTP would require SSE3). Restore the
         * original control word without clearing floating status flags. */
        an_gas_print(gas, "\tfnstcw 8(%%rsp)\n\tmovzwl 8(%%rsp), %%ecx\n"
            "\torw $0x0c00, %%cx\n\tmovw %%cx, 10(%%rsp)\n\tfldcw 10(%%rsp)\n"
            "\tfistpq (%%rsp)\n\tfldcw 8(%%rsp)\n\tmovq (%%rsp), %%rax\n\taddq $16, %%rsp\n");
        if (op == ARCHON_MIR_AMD64_X87_TO_UI)
            an_gas_print(gas, "\torq %%rdx, %%rax\n");
        return;
    default:
        gas->out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
}

static void divide(ANGas *gas, ArchonMIROpcode op)
{
    const int checked = op == ARCHON_MIR_DIV_CHECKED_S;
    const int sign = checked || op == ARCHON_MIR_DIV_S || op == ARCHON_MIR_REM_S;
    const int rem = op == ARCHON_MIR_REM_S || op == ARCHON_MIR_REM_U;
    unsigned nonzero = gas->next_label++, done = gas->next_label++;

    /* Both zero and signed MIN/-1 must be handled before DIV/IDIV, since
     * the hardware would raise a divide error rather than return MIR's
     * total division/remainder result. */
    an_gas_print(gas, "\ttestq %%rcx, %%rcx\n");
    jump(gas, "jnz", nonzero);
    if (checked) {
        an_gas_trap(gas);
    } else {
        if (!rem)
            an_gas_print(gas, "\tmovq $-1, %%rax\n");
        an_gas_jump(gas, done);
    }
    an_gas_label(gas, nonzero);
    if (sign) {
        unsigned hardware = gas->next_label++;
        an_gas_print(gas, "\tcmpq $-1, %%rcx\n");
        jump(gas, "jne", hardware);
        an_gas_print(gas, "\tmovabsq $0x8000000000000000, %%rdx\n\tcmpq %%rdx, %%rax\n");
        jump(gas, "jne", hardware);
        if (checked) {
            an_gas_trap(gas);
        } else {
            if (rem)
                an_gas_print(gas, "\txorl %%eax, %%eax\n");
            an_gas_jump(gas, done);
        }
        an_gas_label(gas, hardware);
        an_gas_print(gas, "\tcqto\n\tidivq %%rcx\n");
    } else {
        an_gas_print(gas, "\txorl %%edx, %%edx\n\tdivq %%rcx\n");
    }
    if (rem)
        an_gas_print(gas, "\tmovq %%rdx, %%rax\n");
    if (!checked)
        an_gas_label(gas, done);
}

int an_gas_binary(ANGas *gas, ArchonMIROpcode op)
{
    const char *name;
    int checked = 0;
    switch (op) {
    case ARCHON_MIR_ADD_CHECKED_S: checked = 1; /* fall through */
    case ARCHON_MIR_ADD: case ARCHON_MIR_PTR_ADD: name = "addq"; break;
    case ARCHON_MIR_SUB_CHECKED_S: checked = 1; /* fall through */
    case ARCHON_MIR_SUB: name = "subq"; break;
    case ARCHON_MIR_MUL_CHECKED_S: checked = 1; /* fall through */
    case ARCHON_MIR_MUL: name = "imulq"; break;
    case ARCHON_MIR_AND: name = "andq"; break;
    case ARCHON_MIR_OR: name = "orq"; break;
    case ARCHON_MIR_XOR: name = "xorq"; break;
    case ARCHON_MIR_SHL: case ARCHON_MIR_LSHR: case ARCHON_MIR_ASHR:
        name = op == ARCHON_MIR_SHL ? "shlq" : op == ARCHON_MIR_LSHR ? "shrq" : "sarq";
        an_gas_print(gas, "\t%s %%cl, %%rax\n", name);
        return 1;
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
        name = op == ARCHON_MIR_EQ ? "sete" : op == ARCHON_MIR_SLT ? "setl" : "setb";
        an_gas_print(gas, "\tcmpq %%rcx, %%rax\n\t%s %%al\n\tmovzbq %%al, %%rax\n", name);
        return 1;
    case ARCHON_MIR_DIV_S: case ARCHON_MIR_REM_S:
    case ARCHON_MIR_DIV_U: case ARCHON_MIR_REM_U: case ARCHON_MIR_DIV_CHECKED_S:
        divide(gas, op);
        return 1;
    default:
        return 0;
    }
    an_gas_print(gas, "\t%s %%rcx, %%rax\n", name);
    if (checked) {
        unsigned ok = gas->next_label++;
        jump(gas, "jno", ok);
        an_gas_trap(gas);
        an_gas_label(gas, ok);
    }
    return 1;
}
