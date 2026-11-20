#include "NativeInternal.h"

enum { HAS_DST = 1u, HAS_A = 2u, HAS_B = 4u };

unsigned an_shape(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_CONST:
    case ARCHON_MIR_FRAME_ADDR:
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
    case ARCHON_MIR_CALL: /* Optional destination, depending on the signature. */
        return HAS_DST;
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
    case ARCHON_MIR_CALL_INDIRECT: /* Optional destination, func_ptr callee in a. */
        return HAS_DST | HAS_A;
    case ARCHON_MIR_STORE:
        return HAS_A | HAS_B;
    case ARCHON_MIR_ADD:
    case ARCHON_MIR_SUB:
    case ARCHON_MIR_MUL:
    case ARCHON_MIR_DIV_S:
    case ARCHON_MIR_REM_S:
    case ARCHON_MIR_DIV_U:
    case ARCHON_MIR_REM_U:
    case ARCHON_MIR_AND:
    case ARCHON_MIR_OR:
    case ARCHON_MIR_XOR:
    case ARCHON_MIR_SHL:
    case ARCHON_MIR_LSHR:
    case ARCHON_MIR_ASHR:
    case ARCHON_MIR_EQ:
    case ARCHON_MIR_SLT:
    case ARCHON_MIR_ULT:
    case ARCHON_MIR_ADD_CHECKED_S:
    case ARCHON_MIR_SUB_CHECKED_S:
    case ARCHON_MIR_DIV_CHECKED_S:
    case ARCHON_MIR_MUL_CHECKED_S:
    case ARCHON_MIR_PTR_ADD:
        return HAS_DST | HAS_A | HAS_B;
    default:
        return 0;
    }
}

static ArchonMIRType value_type(const ArchonMIRFunction *fn, ArchonValue value)
{
    return value != ARCHON_MIR_NONE && value < fn->value_count
        ? fn->types[value] : ARCHON_MIR_VOID;
}

static int is_value_type(ArchonMIRType type)
{
    return type == ARCHON_MIR_I64 || type == ARCHON_MIR_PTR || type == ARCHON_MIR_FUNC_PTR;
}

static int legal_displacement(ANContext *ctx, uint64_t imm)
{
    if ((imm & UINT64_C(7)) != 0)
        return an_error(ctx, ARCHON_BACKEND_INVALID,
                        "memory displacement must be a multiple of 8 bytes");
    /* Compare two's-complement bit patterns without an out-of-range cast. */
    if (imm > UINT64_C(2047) && imm < UINT64_MAX - UINT64_C(2047))
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
                        "memory displacement does not fit signed 12 bits");
    return 1;
}

static int is_conversion(ArchonMIROpcode op)
{
    return op == ARCHON_MIR_TRUNC || op == ARCHON_MIR_ZEXT || op == ARCHON_MIR_SEXT;
}

/* The verifier checks value table bounds and SSA, and resolves symbols and
 * call signatures. This pass handles local legality. */
int an_legal(ANContext *ctx, const ArchonMIRFunction *fn, const ArchonMIRInst *inst)
{
    const ArchonMIROpcode op = inst->op;
    const unsigned shape = an_shape(op);
    const int call = an_is_call(op);
    ArchonMIRType dst, a, b;

    if (shape == 0)
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "unknown MIR opcode");

    if (((shape & HAS_DST) == 0 && inst->dst != ARCHON_MIR_NONE) ||
            ((shape & HAS_A) == 0 && inst->a != ARCHON_MIR_NONE) ||
            ((shape & HAS_B) == 0 && inst->b != ARCHON_MIR_NONE))
        return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected fixed value operand");

    if (call || op == ARCHON_MIR_GLOBAL_ADDR || op == ARCHON_MIR_FUNC_ADDR) {
        if (inst->symbol == NULL)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "missing symbol operand");
    } else if (inst->symbol != NULL) {
        return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected symbol operand");
    }

    if (call) {
        if (inst->arg_count != 0 && inst->args == NULL)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "missing call arguments");
    } else if (inst->arg_count != 0 || inst->args != NULL) {
        return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected call arguments");
    }

    if (op != ARCHON_MIR_CONST && op != ARCHON_MIR_FRAME_ADDR &&
            op != ARCHON_MIR_LOAD && op != ARCHON_MIR_STORE &&
            !is_conversion(op) && inst->imm != 0)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected immediate operand");

    dst = (shape & HAS_DST) != 0 ? value_type(fn, inst->dst) : ARCHON_MIR_VOID;
    a = (shape & HAS_A) != 0 ? value_type(fn, inst->a) : ARCHON_MIR_VOID;
    b = (shape & HAS_B) != 0 ? value_type(fn, inst->b) : ARCHON_MIR_VOID;

    switch (op) {
    case ARCHON_MIR_CONST:
        if (dst != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "CONST requires an i64 destination");
        return 1;
    case ARCHON_MIR_COPY:
        if (!is_value_type(dst) || dst != a)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "COPY requires identical nonvoid types");
        return 1;
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "integer conversion requires (i64) -> i64");
        if (inst->imm < 1 || inst->imm > 64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "integer conversion width must be in 1..64");
        return 1;
    case ARCHON_MIR_PTR_ADD:
        if (dst != ARCHON_MIR_PTR || a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "PTR_ADD requires (ptr, i64) -> ptr");
        return 1;
    case ARCHON_MIR_LOAD:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "LOAD requires (ptr) -> i64");
        return legal_displacement(ctx, inst->imm);
    case ARCHON_MIR_STORE:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "STORE requires (ptr, i64)");
        return legal_displacement(ctx, inst->imm);
    case ARCHON_MIR_FRAME_ADDR:
        if (dst != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FRAME_ADDR requires a ptr destination");
        if (inst->imm >= fn->local_count)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "frame local index is out of bounds");
        return 1;
    case ARCHON_MIR_GLOBAL_ADDR:
        if (dst != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "GLOBAL_ADDR requires a ptr destination");
        return 1;
    case ARCHON_MIR_FUNC_ADDR:
        if (dst != ARCHON_MIR_FUNC_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FUNC_ADDR requires a func_ptr destination");
        return 1;
    case ARCHON_MIR_CALL:
    case ARCHON_MIR_CALL_INDIRECT:
        if (op == ARCHON_MIR_CALL_INDIRECT && a != ARCHON_MIR_FUNC_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "CALL_INDIRECT requires a func_ptr callee");
        if (inst->dst != ARCHON_MIR_NONE && !is_value_type(dst))
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "call destination must be i64, ptr, or func_ptr");
        return 1;
    case ARCHON_MIR_ADD:
    case ARCHON_MIR_SUB:
    case ARCHON_MIR_MUL:
    case ARCHON_MIR_DIV_S:
    case ARCHON_MIR_REM_S:
    case ARCHON_MIR_DIV_U:
    case ARCHON_MIR_REM_U:
    case ARCHON_MIR_AND:
    case ARCHON_MIR_OR:
    case ARCHON_MIR_XOR:
    case ARCHON_MIR_SHL:
    case ARCHON_MIR_LSHR:
    case ARCHON_MIR_ASHR:
    case ARCHON_MIR_EQ:
    case ARCHON_MIR_SLT:
    case ARCHON_MIR_ULT:
    case ARCHON_MIR_ADD_CHECKED_S:
    case ARCHON_MIR_SUB_CHECKED_S:
    case ARCHON_MIR_DIV_CHECKED_S:
    case ARCHON_MIR_MUL_CHECKED_S:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_I64 || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "integer operation requires (i64, i64) -> i64");
        return 1;
    default:
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "missing legality rule for MIR opcode");
    }
}
