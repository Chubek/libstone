#include "ScalarGAS.h"

/* AAPCS64 volatile registers outside the parameter bank. x11 serves as the
 * arithmetic temporary, x12 is reserved for expanded stack addresses. */
const char an_gas_a[] = "x9", an_gas_b[] = "x10", an_gas_call_target[] = "x13";

static void constant(ANGas *gas, const char *dst, uint64_t value)
{
    unsigned shift;
    an_gas_print(gas, "\tmovz %s, #%u\n", dst, (unsigned)(value & UINT64_C(0xffff)));
    for (shift = 16; shift < 64; shift += 16) {
        unsigned digit = (unsigned)((value >> shift) & UINT64_C(0xffff));
        if (digit)
            an_gas_print(gas, "\tmovk %s, #%u, lsl #%u\n", dst, digit, shift);
    }
}

static void stack_address(ANGas *gas, const char *dst, unsigned offset)
{
    if (offset < 4096u) {
        an_gas_print(gas, "\tadd %s, %s, #%u\n", dst, an_gas_abi.sp, offset);
    } else {
        constant(gas, dst, offset);
        an_gas_print(gas, "\tadd %s, %s, %s, uxtx\n", dst, an_gas_abi.sp, dst);
    }
}

static void stack_move(ANGas *gas, const char *op, const char *reg, unsigned offset,
    unsigned access_bytes)
{
    if (offset <= 4095u * access_bytes) {
        an_gas_print(gas, "\t%s %s, [%s, #%u]\n", op, reg, an_gas_abi.sp, offset);
    } else {
        stack_address(gas, "x12", offset);
        an_gas_print(gas, "\t%s %s, [x12]\n", op, reg);
    }
}

void an_gas_stack_load(ANGas *gas, const char *dst, unsigned offset)
{
    stack_move(gas, "ldr", dst, offset, 8);
}

void an_gas_stack_store(ANGas *gas, const char *src, unsigned offset)
{
    stack_move(gas, "str", src, offset, 8);
}

void an_gas_fp_load(ANGas *gas, ArchonMIRType type, unsigned reg, unsigned offset)
{
    const int single = type == ARCHON_MIR_F32;
    const char name[] = { single ? 's' : 'd', (char)('0' + reg), '\0' };
    stack_move(gas, "ldr", name, offset, single ? 4 : 8);
}

void an_gas_fp_store(ANGas *gas, ArchonMIRType type, unsigned reg, unsigned offset)
{
    int single = type == ARCHON_MIR_F32;
    an_gas_print(gas, "\tfmov %c9, %c%u\n", single ? 'w' : 'x', single ? 's' : 'd', reg);
    an_gas_stack_store(gas, an_gas_a, offset);
}

/* Save the incoming anonymous GP registers and full 128-bit SIMD registers
 * before executing any C code. Each save area ends on a 16-byte boundary. */
void an_aarch64_save_varargs(ANGas *gas, unsigned save, unsigned gp, unsigned fp)
{
    unsigned i;
    for (i = gp; i < an_gas_abi.argc; ++i)
        an_gas_stack_store(gas, an_gas_abi.args[i], save + 8 * i);
    for (i = fp; i < an_gas_abi.fp_argc; ++i) {
        const char name[] = { 'q', (char)('0' + i), '\0' };
        stack_move(gas, "str", name, save + AN_AARCH64_VA_GP_BYTES + 16 * i, 16);
    }
}

void an_aarch64_va_start(ANGas *gas, unsigned save, unsigned overflow, unsigned gp, unsigned fp)
{
    /* AAPCS64 va_list: caller stack cursor, each register area's top,
     * and signed offsets from those tops to the first anonymous register. */
    stack_address(gas, "x10", overflow);
    an_gas_print(gas, "\tstr x10, [x9]\n");
    stack_address(gas, "x10", save + AN_AARCH64_VA_GP_BYTES);
    an_gas_print(gas, "\tstr x10, [x9, #8]\n");
    stack_address(gas, "x10", save + AN_AARCH64_VA_SAVE_BYTES);
    an_gas_print(gas, "\tstr x10, [x9, #16]\n");
    an_gas_print(gas, "\tmov w10, #%d\n\tstr w10, [x9, #24]\n",
        -8 * (int)(an_gas_abi.argc - gp));
    an_gas_print(gas, "\tmov w10, #%d\n\tstr w10, [x9, #28]\n",
        -16 * (int)(an_gas_abi.fp_argc - fp));
}

void an_gas_enter(ANGas *gas, unsigned size, int frame_record)
{
    while (size >= 4096u) {
        an_gas_print(gas, "\tsub sp, sp, #1, lsl #12\n\tstr xzr, [sp]\n");
        size -= 4096u;
    }
    if (size)
        an_gas_print(gas, "\tsub sp, sp, #%u\n", size);
    if (frame_record >= 0) {
        unsigned off = (unsigned)frame_record;
        an_gas_stack_store(gas, "x29", off);
        an_gas_stack_store(gas, an_gas_abi.link, off + an_gas_abi.stack_slot_bytes);
        stack_address(gas, "x29", off);
    }
}

void an_gas_leave(ANGas *gas, unsigned size, int frame_record)
{
    if (frame_record >= 0) {
        unsigned off = (unsigned)frame_record;
        an_gas_stack_load(gas, "x29", off);
        an_gas_stack_load(gas, an_gas_abi.link, off + an_gas_abi.stack_slot_bytes);
    }
    if (size >= 4096u)
        an_gas_print(gas, "\tadd sp, sp, #%u, lsl #12\n", size / 4096u);
    if (size % 4096u)
        an_gas_print(gas, "\tadd sp, sp, #%u\n", size % 4096u);
    an_gas_print(gas, "\tret %s\n", an_gas_abi.link);
}

void an_gas_local_addr(ANGas *gas, unsigned offset)
{
    stack_address(gas, an_gas_a, offset);
}

void an_gas_symbol_addr(ANGas *gas, const char *symbol)
{
    if (gas->out->pic)
        an_gas_print(gas, "\tadrp x9, :got:\"%s\"\n\tldr x9, [x9, :got_lo12:\"%s\"]\n",
            symbol, symbol);
    else
        an_gas_print(gas, "\tadrp x9, \"%s\"\n\tadd x9, x9, :lo12:\"%s\"\n", symbol, symbol);
}

void an_gas_constant(ANGas *gas, uint64_t value)
{
    constant(gas, an_gas_a, value);
}

void an_gas_convert(ANGas *gas, ArchonMIROpcode op, unsigned width)
{
    if (width != 64u)
        an_gas_print(gas, "\t%s x9, x9, #0, #%u\n",
            op == ARCHON_MIR_SEXT ? "sbfx" : "ubfx", width);
}

void an_gas_memory(ANGas *gas, ArchonMIROpcode op, int disp)
{
    const char *name = NULL, *reg = "x9";
    switch (op) {
    case ARCHON_MIR_LOAD_S8: name = "ldrsb"; break;
    case ARCHON_MIR_LOAD_U8: name = "ldrb"; reg = "w9"; break;
    case ARCHON_MIR_LOAD_S16: name = "ldrsh"; break;
    case ARCHON_MIR_LOAD_U16: name = "ldrh"; reg = "w9"; break;
    case ARCHON_MIR_LOAD_S32: name = "ldrsw"; break;
    case ARCHON_MIR_LOAD_U32: name = "ldr"; reg = "w9"; break;
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
        name = "ldr";
        break;
    case ARCHON_MIR_STORE8: name = "strb"; reg = "w10"; break;
    case ARCHON_MIR_STORE16: name = "strh"; reg = "w10"; break;
    case ARCHON_MIR_STORE32: name = "str"; reg = "w10"; break;
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
        name = "str"; reg = "x10";
        break;
    default:
        gas->out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    /* The verified signed 12-bit displacement always fits ADD or SUB
     * immediate, even when the memory instruction cannot encode it. */
    if (disp)
        an_gas_print(gas, "\t%s x9, x9, #%u\n", disp < 0 ? "sub" : "add",
            (unsigned)(disp < 0 ? -disp : disp));
    an_gas_print(gas, "\t%s %s, [x9]\n", name, reg);
}

void an_gas_call(ANGas *gas, const char *symbol, unsigned fp_args)
{
    (void)fp_args;
    /* An identifier such as x0 can also be an assembler register keyword. */
    an_gas_print(gas, "\tbl \"%s\"\n", symbol);
}

void an_gas_call_indirect(ANGas *gas, unsigned fp_args)
{
    (void)fp_args;
    an_gas_print(gas, "\tblr %s\n", an_gas_call_target);
}

static void jump(ANGas *gas, const char *instruction, unsigned label)
{
    an_gas_print(gas, "\t%s .Larchon_%zu_%u\n", instruction, gas->function, label);
}

void an_gas_jump(ANGas *gas, unsigned label)
{
    jump(gas, "b", label);
}

void an_gas_branch_nonzero(ANGas *gas, unsigned label)
{
    jump(gas, "cbnz x9,", label);
}

void an_gas_trap(ANGas *gas)
{
    unsigned halt = gas->next_label++;
    an_gas_print(gas, "\tbrk #0\n");
    an_gas_label(gas, halt);
    an_gas_jump(gas, halt);
}

static void fp_operand(ANGas *gas, char fp, unsigned operand)
{
    an_gas_print(gas, "\tfmov %c%u, %c%u\n", fp, operand,
        fp == 's' ? 'w' : 'x', operand ? 10u : 9u);
}

void an_gas_float(ANGas *gas, ArchonMIROpcode op, ArchonMIRType src, ArchonMIRType dst)
{
    const char fp = src == ARCHON_MIR_F32 ? 's' : 'd';
    const char result = dst == ARCHON_MIR_F32 ? 's' : 'd';
    const char *mnemonic;
    switch (op) {
    case ARCHON_MIR_FNEG:
        an_gas_print(gas, "\teor x9, x9, #%s\n",
            fp == 's' ? "0x80000000" : "0x8000000000000000");
        return;
    case ARCHON_MIR_SITOFP:
    case ARCHON_MIR_UITOFP:
        an_gas_print(gas, "\t%ccvtf %c0, x9\n", op == ARCHON_MIR_SITOFP ? 's' : 'u', result);
        break;
    case ARCHON_MIR_FCAST:
        fp_operand(gas, fp, 0);
        an_gas_print(gas, "\tfcvt %c0, %c0\n", result, fp);
        break;
    case ARCHON_MIR_FPTOSI:
    case ARCHON_MIR_FPTOUI:
        fp_operand(gas, fp, 0);
        an_gas_print(gas, "\tfcvtz%c x9, %c0\n", op == ARCHON_MIR_FPTOSI ? 's' : 'u', fp);
        return;
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
        fp_operand(gas, fp, 0);
        fp_operand(gas, fp, 1);
        /* Unordered flags are NZCV=0011; none of eq/mi/ls accepts them. */
        mnemonic = op == ARCHON_MIR_FEQ ? "eq" : op == ARCHON_MIR_FLT ? "mi" : "ls";
        an_gas_print(gas, "\tfcmp%s %c0, %c1\n\tcset x9, %s\n",
            op == ARCHON_MIR_FEQ ? "" : "e", fp, fp, mnemonic);
        return;
    case ARCHON_MIR_FADD:
    case ARCHON_MIR_FSUB:
    case ARCHON_MIR_FMUL:
    case ARCHON_MIR_FDIV:
        fp_operand(gas, fp, 0);
        fp_operand(gas, fp, 1);
        mnemonic = op == ARCHON_MIR_FADD ? "fadd" : op == ARCHON_MIR_FSUB ? "fsub" :
            op == ARCHON_MIR_FMUL ? "fmul" : "fdiv";
        an_gas_print(gas, "\t%s %c0, %c0, %c1\n", mnemonic, fp, fp, fp);
        break;
    default:
        gas->out->status = ARCHON_BACKEND_INTERNAL;
        return;
    }
    an_gas_print(gas, "\tfmov %c9, %c0\n", result == 's' ? 'w' : 'x', result);
}

static void trap_unless(ANGas *gas, const char *branch)
{
    unsigned ok = gas->next_label++;
    jump(gas, branch, ok);
    an_gas_trap(gas);
    an_gas_label(gas, ok);
}

static void divide(ANGas *gas, ArchonMIROpcode op)
{
    if (op == ARCHON_MIR_DIV_CHECKED_S) {
        unsigned ok = gas->next_label++;
        trap_unless(gas, "cbnz x10,");
        an_gas_print(gas, "\tcmn x10, #1\n");
        jump(gas, "b.ne", ok);
        an_gas_print(gas, "\tmovz x11, #32768, lsl #48\n\tcmp x9, x11\n");
        trap_unless(gas, "b.ne");
        an_gas_label(gas, ok);
        an_gas_print(gas, "\tsdiv x9, x9, x10\n");
    } else {
        int sign = op == ARCHON_MIR_DIV_S || op == ARCHON_MIR_REM_S;
        an_gas_print(gas, "\t%s x11, x9, x10\n", sign ? "sdiv" : "udiv");
        /* Hardware gives MIN for MIN/-1 and zero quotient for zero
         * divisor. MSUB gives the MIR remainder in both cases. MIR's
         * quotient is all ones, not zero, when the divisor is zero. */
        if (op == ARCHON_MIR_REM_S || op == ARCHON_MIR_REM_U)
            an_gas_print(gas, "\tmsub x9, x11, x10, x9\n");
        else
            an_gas_print(gas, "\tcmp x10, #0\n\tcsinv x9, x11, xzr, ne\n");
    }
}

int an_gas_binary(ANGas *gas, ArchonMIROpcode op)
{
    const char *name;
    switch (op) {
    case ARCHON_MIR_ADD: case ARCHON_MIR_PTR_ADD: name = "add"; break;
    case ARCHON_MIR_SUB: name = "sub"; break;
    case ARCHON_MIR_MUL: name = "mul"; break;
    case ARCHON_MIR_AND: name = "and"; break;
    case ARCHON_MIR_OR: name = "orr"; break;
    case ARCHON_MIR_XOR: name = "eor"; break;
    case ARCHON_MIR_SHL: name = "lslv"; break;
    case ARCHON_MIR_LSHR: name = "lsrv"; break;
    case ARCHON_MIR_ASHR: name = "asrv"; break;
    case ARCHON_MIR_ADD_CHECKED_S: case ARCHON_MIR_SUB_CHECKED_S:
        name = op == ARCHON_MIR_ADD_CHECKED_S ? "adds" : "subs";
        an_gas_print(gas, "\t%s x9, x9, x10\n", name);
        trap_unless(gas, "b.vc");
        return 1;
    case ARCHON_MIR_MUL_CHECKED_S:
        an_gas_print(gas, "\tsmulh x11, x9, x10\n\tmul x9, x9, x10\n\tcmp x11, x9, asr #63\n");
        trap_unless(gas, "b.eq");
        return 1;
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
        name = op == ARCHON_MIR_EQ ? "eq" : op == ARCHON_MIR_SLT ? "lt" : "lo";
        an_gas_print(gas, "\tcmp x9, x10\n\tcset x9, %s\n", name);
        return 1;
    case ARCHON_MIR_DIV_S: case ARCHON_MIR_REM_S:
    case ARCHON_MIR_DIV_U: case ARCHON_MIR_REM_U: case ARCHON_MIR_DIV_CHECKED_S:
        divide(gas, op);
        return 1;
    default:
        return 0;
    }
    an_gas_print(gas, "\t%s x9, x9, x10\n", name);
    return 1;
}
