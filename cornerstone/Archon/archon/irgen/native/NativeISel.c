#include "NativeInternal.h"
#include "TargetNative.h"

static void emit_trap(ANOut *out, size_t fn, unsigned *next_label)
{
    unsigned halt = (*next_label)++;
    an_emit(out, AN_EBREAK, AN_ZERO, AN_ZERO, AN_ZERO, 0);
    /* A debugger that advances past EBREAK must not reach the success path. */
    an_label(out, fn, halt);
    an_jump(out, fn, halt);
}

static void select_checked_addsub(ANOut *out, int subtract, unsigned dst,
    unsigned a, unsigned b, size_t fn, unsigned *next_label)
{
    unsigned ok = (*next_label)++;
    an_emit(out, subtract ? AN_SUB : AN_ADD, dst, a, b, 0);

    /* Overflow is the sign bit of:
     *   add: (b ^ result) & (a ^ result)
     *   sub: (b ^ a)      & (a ^ result)
     * Inputs are scratch copies; keep the wrapped result in dst. */
    an_emit(out, AN_XOR, b, b, subtract ? a : dst, 0);
    an_emit(out, AN_XOR, a, a, dst, 0);
    an_emit(out, AN_AND, a, a, b, 0);
    an_emit(out, AN_SLT, a, a, AN_ZERO, 0);
    an_branch(out, 0, a, AN_ZERO, fn, ok);
    emit_trap(out, fn, next_label);
    an_label(out, fn, ok);
}

static void select_checked_div(ANOut *out, unsigned dst, unsigned a,
    unsigned b, size_t fn, unsigned *next_label)
{
    unsigned trap = (*next_label)++;
    unsigned ok = (*next_label)++;

    /* RISC-V DIV does not trap; guard both zero and INT64_MIN / -1. */
    an_branch(out, 0, b, AN_ZERO, fn, trap);
    an_emit(out, AN_ADDI, dst, b, AN_ZERO, 1);
    an_branch(out, 1, dst, AN_ZERO, fn, ok);
    /* Here b is -1; its low bit shifted to bit 63 is INT64_MIN. */
    an_emit(out, AN_SLLI, dst, b, AN_ZERO, 63);
    an_branch(out, 1, a, dst, fn, ok);
    an_label(out, fn, trap);
    emit_trap(out, fn, next_label);
    an_label(out, fn, ok);
    an_emit(out, AN_DIV, dst, a, b, 0);
}

static void select_checked_mul(ANOut *out, unsigned dst, unsigned a,
    unsigned b, size_t fn, unsigned *next_label)
{
    unsigned minus_one = (*next_label)++;
    unsigned trap = (*next_label)++;
    unsigned ok = (*next_label)++;

    an_emit(out, AN_MUL, dst, a, b, 0);
    /* A zero divisor already gives an exact zero product. */
    an_branch(out, 0, b, AN_ZERO, fn, ok);
    an_emit(out, AN_ADDI, AN_T3, AN_ZERO, AN_ZERO, -1);
    an_branch(out, 0, b, AN_T3, fn, minus_one);

    /* A quotient equal to a implies |signed(dst) - a*b| < |b|. The
     * difference is a multiple of 2^64, so it must be zero. Keep dst,
     * clobber b. */
    an_emit(out, AN_DIV, b, dst, b, 0);
    an_branch(out, 0, b, a, fn, ok);
    an_label(out, fn, trap);
    emit_trap(out, fn, next_label);

    an_label(out, fn, minus_one);
    /* Negation overflows only for INT64_MIN (which DIV would leave unchanged).
     * Materialize INT64_MIN here: taken branches clobber T3. */
    an_emit(out, AN_SLLI, AN_T3, b, AN_ZERO, 63);
    an_branch(out, 0, a, AN_T3, fn, trap);
    an_label(out, fn, ok);
}

static void select_base_mul(ANOut *out, unsigned dst, unsigned a,
    unsigned b, size_t fn, unsigned *next_label)
{
    unsigned loop = (*next_label)++;
    unsigned next_bit = (*next_label)++;

    /* Unsigned shift-and-add also gives the signed product modulo 2^64.
     * dst + a*b is invariant modulo 2^64; b becomes zero in <=64 steps.
     * a and b are scratch copies. Taken transfers clobber T3, so
     * rematerialize the shift count at next_bit. */
    an_emit(out, AN_ADDI, dst, AN_ZERO, AN_ZERO, 0);
    an_label(out, fn, loop);
    an_emit(out, AN_ADDI, AN_T3, AN_ZERO, AN_ZERO, 1);
    an_emit(out, AN_AND, AN_T3, b, AN_T3, 0);
    an_branch(out, 0, AN_T3, AN_ZERO, fn, next_bit);
    an_emit(out, AN_ADD, dst, dst, a, 0);
    an_label(out, fn, next_bit);
    an_emit(out, AN_SLLI, a, a, AN_ZERO, 1);
    an_emit(out, AN_ADDI, AN_T3, AN_ZERO, AN_ZERO, 1);
    an_emit(out, AN_SRL, b, b, AN_T3, 0);
    an_branch(out, 1, b, AN_ZERO, fn, loop);
}

/* The caller stages operands and the result in three distinct scratch
 * registers (T0, T1, T2); checked operations may overwrite operand copies. */
void an_select_shift_left_imm(ANOut *out, unsigned dst, unsigned src, unsigned shift)
{
    an_emit(out, shift ? AN_SLLI : AN_ADDI, dst, src, AN_ZERO, (int)shift);
}

int an_select(ANOut *out, ArchonMIROpcode op, unsigned dst, unsigned a,
    unsigned b, size_t fn, unsigned *next_label)
{
    ANOpcode selected;
    switch (op) {
    case ARCHON_MIR_ADD: selected = AN_ADD; break;
    case ARCHON_MIR_SUB: selected = AN_SUB; break;
    case ARCHON_MIR_MUL:
        if (!AN_RV64_HAS_M) {
            select_base_mul(out, dst, a, b, fn, next_label);
            return 1;
        }
        selected = AN_MUL;
        break;
    /* Total division/remainder use the RV64M edge-case results. */
    case ARCHON_MIR_DIV_S: selected = AN_DIV; break;
    case ARCHON_MIR_REM_S: selected = AN_REM; break;
    case ARCHON_MIR_DIV_U: selected = AN_DIVU; break;
    case ARCHON_MIR_REM_U: selected = AN_REMU; break;
    case ARCHON_MIR_AND: selected = AN_AND; break;
    case ARCHON_MIR_OR: selected = AN_OR; break;
    case ARCHON_MIR_XOR: selected = AN_XOR; break;
    /* RV64 register shifts use the low six bits of the count. */
    case ARCHON_MIR_SHL: selected = AN_SLL; break;
    case ARCHON_MIR_LSHR: selected = AN_SRL; break;
    case ARCHON_MIR_ASHR: selected = AN_SRA; break;
    case ARCHON_MIR_SLT: selected = AN_SLT; break;
    case ARCHON_MIR_ULT: selected = AN_SLTU; break;
    case ARCHON_MIR_EQ:
        /* Normalize the nonzero test, then compute 1 - nonzero. */
        an_emit(out, AN_XOR, dst, a, b, 0);
        an_emit(out, AN_SLTU, dst, AN_ZERO, dst, 0);
        an_emit(out, AN_SUB, dst, AN_ZERO, dst, 0);
        an_emit(out, AN_ADDI, dst, dst, AN_ZERO, 1);
        return 1;
    case ARCHON_MIR_ADD_CHECKED_S:
    case ARCHON_MIR_SUB_CHECKED_S:
        select_checked_addsub(out, op == ARCHON_MIR_SUB_CHECKED_S, dst, a, b,
            fn, next_label);
        return 1;
    case ARCHON_MIR_DIV_CHECKED_S:
        select_checked_div(out, dst, a, b, fn, next_label);
        return 1;
    case ARCHON_MIR_MUL_CHECKED_S:
        select_checked_mul(out, dst, a, b, fn, next_label);
        return 1;
    default:
        return 0;
    }
    an_emit(out, selected, dst, a, b, 0);
    return 1;
}
