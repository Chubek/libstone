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
}

void an_gas_leave(ANGas *gas, unsigned size, int frame_record)
{
    (void)frame_record;
    if (size)
        an_gas_print(gas, "\taddq $%u, %%rsp\n", size);
    an_gas_print(gas, "\tret\n");
}

void an_gas_stack_load(ANGas *gas, const char *dst, unsigned offset)
{
    an_gas_print(gas, "\tmovq %u(%s), %s\n", offset, an_gas_abi.sp, dst);
}

void an_gas_stack_store(ANGas *gas, const char *src, unsigned offset)
{
    an_gas_print(gas, "\tmovq %s, %u(%s)\n", src, offset, an_gas_abi.sp);
}

void an_gas_local_addr(ANGas *gas, unsigned offset)
{
    an_gas_print(gas, "\tleaq %u(%s), %%rax\n", offset, an_gas_abi.sp);
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

void an_gas_call(ANGas *gas, const char *symbol)
{
    /* The supported scalar signatures never have vector arguments. Set %al
     * to zero even when the external target is not declared variadic in MIR. */
    an_gas_print(gas, "\txorl %%eax, %%eax\n\tcall %s%s\n",
        symbol, gas->out->pic ? "@PLT" : "");
}

void an_gas_call_indirect(ANGas *gas)
{
    an_gas_print(gas, "\txorl %%eax, %%eax\n\tcall *%s\n", an_gas_call_target);
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
