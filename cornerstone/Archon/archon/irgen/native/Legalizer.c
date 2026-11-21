#include "NativeInternal.h"
/* Embedders can reuse the structural verifier without a generated provider. */
#ifndef AN_GENERIC_MIR_VERIFY
#include "TargetNative.h"
#include <stdlib.h>
#include <string.h>
#endif

enum { HAS_DST = 1u, HAS_A = 2u, HAS_B = 4u };

unsigned an_shape(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_CONST:
    case ARCHON_MIR_FCONST:
    case ARCHON_MIR_FRAME_ADDR:
    case ARCHON_MIR_GLOBAL_ADDR:
    case ARCHON_MIR_FUNC_ADDR:
    case ARCHON_MIR_CALL: /* Optional destination, depending on the signature. */
        return HAS_DST;
    case ARCHON_MIR_COPY:
    case ARCHON_MIR_FLOAD:
    case ARCHON_MIR_FNEG:
    case ARCHON_MIR_FCAST:
    case ARCHON_MIR_SITOFP:
    case ARCHON_MIR_UITOFP:
    case ARCHON_MIR_FPTOSI:
    case ARCHON_MIR_FPTOUI:
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_S8:
    case ARCHON_MIR_LOAD_U8:
    case ARCHON_MIR_LOAD_S16:
    case ARCHON_MIR_LOAD_U16:
    case ARCHON_MIR_LOAD_S32:
    case ARCHON_MIR_LOAD_U32:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
    case ARCHON_MIR_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_PTR:
    case ARCHON_MIR_FUNC_PTR_TO_INT:
    case ARCHON_MIR_INT_TO_FUNC_PTR:
    case ARCHON_MIR_TRUNC:
    case ARCHON_MIR_ZEXT:
    case ARCHON_MIR_SEXT:
    case ARCHON_MIR_AMD64_X87_TO_FP:
    case ARCHON_MIR_AMD64_X87_TO_SI:
    case ARCHON_MIR_AMD64_X87_TO_UI:
    case ARCHON_MIR_CALL_INDIRECT: /* Optional destination, func_ptr callee in a. */
        return HAS_DST | HAS_A;
    case ARCHON_MIR_AMD64_VA_START:
    case ARCHON_MIR_AARCH64_VA_START:
    case ARCHON_MIR_RV64_VA_START:
        return HAS_A;
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_FSTORE:
    case ARCHON_MIR_STORE8:
    case ARCHON_MIR_STORE16:
    case ARCHON_MIR_STORE32:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
    case ARCHON_MIR_AMD64_X87_ADD:
    case ARCHON_MIR_AMD64_X87_SUB:
    case ARCHON_MIR_AMD64_X87_MUL:
    case ARCHON_MIR_AMD64_X87_DIV:
    case ARCHON_MIR_AMD64_X87_FROM_FP:
    case ARCHON_MIR_AMD64_X87_FROM_SI:
    case ARCHON_MIR_AMD64_X87_FROM_UI:
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
    case ARCHON_MIR_FADD:
    case ARCHON_MIR_FSUB:
    case ARCHON_MIR_FMUL:
    case ARCHON_MIR_FDIV:
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
    case ARCHON_MIR_AMD64_X87_EQ:
    case ARCHON_MIR_AMD64_X87_LT:
    case ARCHON_MIR_AMD64_X87_LE:
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

int an_target_type(ANContext *ctx, ArchonMIRType type)
{
#if defined(AN_GENERIC_MIR_VERIFY) || defined(AN_SCALAR_GAS_FP) || defined(AN_RV64_SOFT_FP_TRANSPORT)
    (void)ctx;
    (void)type;
#else
    if (an_is_float(type))
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
            "floating-point MIR requires a hardware FP ABI (RV64 LP64 is soft-float)");
#endif
    return 1;
}

static int legal_displacement(ANContext *ctx, uint64_t imm, unsigned alignment)
{
    if ((imm & (alignment - 1u)) != 0)
        return an_error(ctx, ARCHON_BACKEND_INVALID,
                        "memory displacement does not satisfy access alignment");
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

static int legal_fp_arithmetic_or_conversion(ANContext *ctx)
{
#ifdef AN_RV64_SOFT_FP_TRANSPORT
    return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED,
                    "runtime floating-point arithmetic and conversions are not supported on RV64IM LP64");
#else
    (void)ctx;
    return 1;
#endif
}

static int legal_x87(ANContext *ctx, int types_match)
{
#if defined(AN_GENERIC_MIR_VERIFY) || defined(AN_SYSV_AMD64)
    if (!types_match)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "incorrect operand types for AMD64 x87 operation");
    return 1;
#else
    (void)types_match;
    return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "x87 MIR requires SysV AMD64");
#endif
}

static int is_memory(ArchonMIROpcode op)
{
    switch (op) {
    case ARCHON_MIR_FLOAD:
    case ARCHON_MIR_FSTORE:
    case ARCHON_MIR_LOAD:
    case ARCHON_MIR_LOAD_S8:
    case ARCHON_MIR_LOAD_U8:
    case ARCHON_MIR_LOAD_S16:
    case ARCHON_MIR_LOAD_U16:
    case ARCHON_MIR_LOAD_S32:
    case ARCHON_MIR_LOAD_U32:
    case ARCHON_MIR_LOAD_PTR:
    case ARCHON_MIR_LOAD_FUNC_PTR:
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_STORE8:
    case ARCHON_MIR_STORE16:
    case ARCHON_MIR_STORE32:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
        return 1;
    default:
        return 0;
    }
}

static int legal_integer_operation(ANContext *ctx, ArchonMIROpcode op)
{
#if !defined(AN_SCALAR_GAS) && !defined(AN_GENERIC_MIR_VERIFY)
    if (!AN_RV64_HAS_M) {
        switch (op) {
        case ARCHON_MIR_DIV_S:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "div_s is unsupported without M (RV64I)");
        case ARCHON_MIR_REM_S:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "rem_s is unsupported without M (RV64I)");
        case ARCHON_MIR_DIV_U:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "div_u is unsupported without M (RV64I)");
        case ARCHON_MIR_REM_U:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "rem_u is unsupported without M (RV64I)");
        case ARCHON_MIR_DIV_CHECKED_S:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "div_checked_s is unsupported without M (RV64I)");
        case ARCHON_MIR_MUL_CHECKED_S:
            return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "mul_checked_s is unsupported without M (RV64I)");
        default:
            break;
        }
    }
#else
    (void)ctx;
    (void)op;
#endif
    return 1;
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

    if (op != ARCHON_MIR_CONST && op != ARCHON_MIR_FCONST && op != ARCHON_MIR_FRAME_ADDR &&
            !is_memory(op) &&
            !is_conversion(op) && inst->imm != 0)
        return an_error(ctx, ARCHON_BACKEND_INVALID, "unexpected immediate operand");

    dst = (shape & HAS_DST) != 0 ? value_type(fn, inst->dst) : ARCHON_MIR_VOID;
    a = (shape & HAS_A) != 0 ? value_type(fn, inst->a) : ARCHON_MIR_VOID;
    b = (shape & HAS_B) != 0 ? value_type(fn, inst->b) : ARCHON_MIR_VOID;

    switch (op) {
    case ARCHON_MIR_AMD64_X87_ADD:
    case ARCHON_MIR_AMD64_X87_SUB:
    case ARCHON_MIR_AMD64_X87_MUL:
    case ARCHON_MIR_AMD64_X87_DIV:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && b == ARCHON_MIR_PTR);
    case ARCHON_MIR_AMD64_X87_EQ:
    case ARCHON_MIR_AMD64_X87_LT:
    case ARCHON_MIR_AMD64_X87_LE:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && b == ARCHON_MIR_PTR && dst == ARCHON_MIR_I64);
    case ARCHON_MIR_AMD64_X87_FROM_FP:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && an_is_float(b));
    case ARCHON_MIR_AMD64_X87_TO_FP:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && an_is_float(dst));
    case ARCHON_MIR_AMD64_X87_FROM_SI:
    case ARCHON_MIR_AMD64_X87_FROM_UI:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && b == ARCHON_MIR_I64);
    case ARCHON_MIR_AMD64_X87_TO_SI:
    case ARCHON_MIR_AMD64_X87_TO_UI:
        return legal_x87(ctx, a == ARCHON_MIR_PTR && dst == ARCHON_MIR_I64);
    case ARCHON_MIR_AMD64_VA_START:
#if defined(AN_GENERIC_MIR_VERIFY) || defined(AN_SYSV_AMD64)
        if (a != ARCHON_MIR_PTR || fn->param_count == 0)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "AMD64_VA_START requires a list pointer and named parameters");
        return 1;
#else
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "AMD64_VA_START requires SysV AMD64");
#endif
    case ARCHON_MIR_AARCH64_VA_START:
#if defined(AN_GENERIC_MIR_VERIFY) || defined(AN_AAPCS64)
        if (a != ARCHON_MIR_PTR || fn->param_count == 0)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "AARCH64_VA_START requires a list pointer and named parameters");
        return 1;
#else
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "AARCH64_VA_START requires AAPCS64");
#endif
    case ARCHON_MIR_RV64_VA_START:
#if defined(AN_GENERIC_MIR_VERIFY) || defined(AN_RV64_STDARG)
        if (a != ARCHON_MIR_PTR || fn->param_count == 0)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "RV64_VA_START requires a list pointer and named parameters");
        return 1;
#else
        return an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "RV64_VA_START requires RV64IM LP64");
#endif
    case ARCHON_MIR_CONST:
        if (dst != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "CONST requires an i64 destination");
        return 1;
    case ARCHON_MIR_COPY:
        if (!an_is_value_type(dst) || dst != a)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "COPY requires identical nonvoid types");
        return 1;
    case ARCHON_MIR_FCONST:
        if (!an_is_float(dst))
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FCONST requires a floating-point destination");
        if (dst == ARCHON_MIR_F32 && inst->imm > UINT32_MAX)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FCONST f32 bits exceed 32 bits");
        return 1;
    case ARCHON_MIR_FLOAD:
        if (!an_is_float(dst) || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FLOAD requires (ptr) -> f32 or f64");
        return legal_displacement(ctx, inst->imm, dst == ARCHON_MIR_F32 ? 4u : 8u);
    case ARCHON_MIR_FSTORE:
        if (a != ARCHON_MIR_PTR || !an_is_float(b))
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FSTORE requires (ptr, f32 or f64)");
        return legal_displacement(ctx, inst->imm, b == ARCHON_MIR_F32 ? 4u : 8u);
    case ARCHON_MIR_FNEG:
        if (!an_is_float(dst) || dst != a)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FNEG requires matching FP types");
        return 1;
    case ARCHON_MIR_FADD:
    case ARCHON_MIR_FSUB:
    case ARCHON_MIR_FMUL:
    case ARCHON_MIR_FDIV:
        if (!an_is_float(dst) || dst != a || dst != b)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "floating arithmetic requires matching FP types");
        return legal_fp_arithmetic_or_conversion(ctx);
    case ARCHON_MIR_FEQ:
    case ARCHON_MIR_FLT:
    case ARCHON_MIR_FLE:
        if (dst != ARCHON_MIR_I64 || !an_is_float(a) || a != b)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "floating comparison requires matching FP operands and i64 result");
        return 1;
    case ARCHON_MIR_FCAST:
        if (!an_is_float(dst) || !an_is_float(a) || a == dst)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "FCAST requires different FP source and result types");
        return legal_fp_arithmetic_or_conversion(ctx);
    case ARCHON_MIR_SITOFP:
    case ARCHON_MIR_UITOFP:
        if (!an_is_float(dst) || a != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "integer-to-float conversion requires (i64) -> f32 or f64");
        return legal_fp_arithmetic_or_conversion(ctx);
    case ARCHON_MIR_FPTOSI:
    case ARCHON_MIR_FPTOUI:
        if (dst != ARCHON_MIR_I64 || !an_is_float(a))
            return an_error(ctx, ARCHON_BACKEND_INVALID, "float-to-integer conversion requires (f32 or f64) -> i64");
        return legal_fp_arithmetic_or_conversion(ctx);
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
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_LOAD_S8:
    case ARCHON_MIR_LOAD_U8:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "narrow load requires (ptr) -> i64");
        return legal_displacement(ctx, inst->imm, 1);
    case ARCHON_MIR_LOAD_S16:
    case ARCHON_MIR_LOAD_U16:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "narrow load requires (ptr) -> i64");
        return legal_displacement(ctx, inst->imm, 2);
    case ARCHON_MIR_LOAD_S32:
    case ARCHON_MIR_LOAD_U32:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "narrow load requires (ptr) -> i64");
        return legal_displacement(ctx, inst->imm, 4);
    case ARCHON_MIR_LOAD_PTR:
        if (dst != ARCHON_MIR_PTR || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "LOAD_PTR requires (ptr) -> ptr");
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_LOAD_FUNC_PTR:
        if (dst != ARCHON_MIR_FUNC_PTR || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "LOAD_FUNC_PTR requires (ptr) -> func_ptr");
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_STORE:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID, "STORE requires (ptr, i64)");
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_STORE8:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "STORE8 requires (ptr, i64)");
        return legal_displacement(ctx, inst->imm, 1);
    case ARCHON_MIR_STORE16:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "STORE16 requires (ptr, i64)");
        return legal_displacement(ctx, inst->imm, 2);
    case ARCHON_MIR_STORE32:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "STORE32 requires (ptr, i64)");
        return legal_displacement(ctx, inst->imm, 4);
    case ARCHON_MIR_STORE_PTR:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "STORE_PTR requires (ptr, ptr)");
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_STORE_FUNC_PTR:
        if (a != ARCHON_MIR_PTR || b != ARCHON_MIR_FUNC_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "STORE_FUNC_PTR requires (ptr, func_ptr)");
        return legal_displacement(ctx, inst->imm, 8);
    case ARCHON_MIR_PTR_TO_INT:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "PTR_TO_INT requires (ptr) -> i64");
        return 1;
    case ARCHON_MIR_INT_TO_PTR:
        if (dst != ARCHON_MIR_PTR || a != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "INT_TO_PTR requires (i64) -> ptr");
        return 1;
    case ARCHON_MIR_FUNC_PTR_TO_INT:
        if (dst != ARCHON_MIR_I64 || a != ARCHON_MIR_FUNC_PTR)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "FUNC_PTR_TO_INT requires (func_ptr) -> i64");
        return 1;
    case ARCHON_MIR_INT_TO_FUNC_PTR:
        if (dst != ARCHON_MIR_FUNC_PTR || a != ARCHON_MIR_I64)
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "INT_TO_FUNC_PTR requires (i64) -> func_ptr");
        return 1;
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
        if (inst->dst != ARCHON_MIR_NONE && !an_is_value_type(dst))
            return an_error(ctx, ARCHON_BACKEND_INVALID,
                            "call destination must be a nonvoid scalar");
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
        return legal_integer_operation(ctx, op);
    default:
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "missing legality rule for MIR opcode");
    }
}

#ifndef AN_GENERIC_MIR_VERIFY
static ANProvenBits proven_value(const ANProvenBits *bits, ArchonValue value)
{
    return value == ARCHON_MIR_NONE ? (ANProvenBits){0, 0} : bits[value];
}

/* A small verifier for claims about the original MIR, not a trusted plugin
 * optimization. Every transfer may underapproximate the claimed masks, never
 * overstate them. CFG joins start unknown and require all incoming edges. */
static ANProvenBits prove_inst(const ArchonMIRInst *ins, const ANProvenBits *bits)
{
    ANProvenBits a = proven_value(bits, ins->a), b = proven_value(bits, ins->b);
    uint64_t low, sign;
    switch (ins->op) {
    case ARCHON_MIR_CONST: return (ANProvenBits){~ins->imm, ins->imm};
    case ARCHON_MIR_COPY: return a;
    case ARCHON_MIR_AND: return (ANProvenBits){a.zero | b.zero, a.one & b.one};
    case ARCHON_MIR_OR: return (ANProvenBits){a.zero & b.zero, a.one | b.one};
    case ARCHON_MIR_XOR:
        return (ANProvenBits){(a.zero & b.zero) | (a.one & b.one),
                              (a.zero & b.one) | (a.one & b.zero)};
    case ARCHON_MIR_LOAD_U8: return (ANProvenBits){~UINT64_C(255), 0};
    case ARCHON_MIR_LOAD_U16: return (ANProvenBits){~UINT64_C(65535), 0};
    case ARCHON_MIR_LOAD_U32: return (ANProvenBits){~UINT64_C(0xffffffff), 0};
    case ARCHON_MIR_EQ: case ARCHON_MIR_SLT: case ARCHON_MIR_ULT:
        return (ANProvenBits){~UINT64_C(1), 0};
    case ARCHON_MIR_SHL: case ARCHON_MIR_LSHR:
        if ((b.zero | b.one) != UINT64_MAX || b.one >= 64)
            break;
        if (!b.one)
            return a;
        if (ins->op == ARCHON_MIR_SHL)
            return (ANProvenBits){(a.zero << b.one) | (UINT64_MAX >> (64 - b.one)),
                                  a.one << b.one};
        return (ANProvenBits){(a.zero >> b.one) | (UINT64_MAX << (64 - b.one)),
                              a.one >> b.one};
    case ARCHON_MIR_TRUNC: case ARCHON_MIR_ZEXT: case ARCHON_MIR_SEXT:
        if (ins->imm == 64)
            return a;
        low = UINT64_MAX >> (64 - ins->imm); /* Verified width 1..63. */
        sign = UINT64_C(1) << (ins->imm - 1);
        return (ANProvenBits){(a.zero & low) | ((ins->op != ARCHON_MIR_SEXT || (a.zero & sign)) ? ~low : 0),
                              (a.one & low) | ((ins->op == ARCHON_MIR_SEXT && (a.one & sign)) ? ~low : 0)};
    default:
        break;
    }
    return (ANProvenBits){0, 0};
}

static void prove_value(ANProvenBits *value, ANProvenBits proven, int *changed)
{
    if (value->zero != proven.zero || value->one != proven.one) {
        *value = proven;
        *changed = 1;
    }
}

static void prove_edge(const ArchonMIRFunction *fn, const ArchonMIREdge *edge,
    const ANProvenBits *bits, ANProvenBits *joins, uint8_t *seen)
{
    const ArchonMIRBlock *target = &fn->blocks[edge->target];
    size_t p;
    for (p = 0; p < edge->arg_count; ++p) {
        const ArchonValue value = target->params[p];
        const ANProvenBits incoming = bits[edge->args[p]];
        if (!seen[edge->target]) {
            joins[value] = incoming;
        } else {
            joins[value].zero &= incoming.zero;
            joins[value].one &= incoming.one;
        }
    }
    seen[edge->target] = 1;
}

ANProvenBits *an_prove_known_bits(const ANContext *ctx, const ArchonMIRFunction *fn)
{
    ANProvenBits *bits, *joins;
    uint8_t *seen;
    unsigned sweep;
    if (!ctx->facts || !ctx->facts->known_bits || !fn->value_count)
        return NULL;
    bits = calloc(fn->value_count * 2, sizeof(*bits));
    seen = malloc(fn->block_count);
    if (!bits || !seen) {
        free(bits);
        free(seen);
        return NULL; /* Best-effort analysis; keep ordinary lowering. */
    }
    joins = bits + fn->value_count;
    for (sweep = 0; sweep < 8; ++sweep) {
        size_t b, i;
        int changed = 0;
        memset(seen, 0, fn->block_count);
        for (b = 0; b < fn->block_count; ++b) {
            const ArchonMIRBlock *block = &fn->blocks[b];
            for (i = 0; i < block->inst_count; ++i) {
                const ArchonMIRInst *ins = &block->insts[i];
                if (ins->dst != ARCHON_MIR_NONE && fn->types[ins->dst] == ARCHON_MIR_I64)
                    prove_value(&bits[ins->dst], prove_inst(ins, bits), &changed);
            }
            if (block->term.kind == ARCHON_MIR_BR || block->term.kind == ARCHON_MIR_CBR)
                prove_edge(fn, &block->term.yes, bits, joins, seen);
            if (block->term.kind == ARCHON_MIR_CBR)
                prove_edge(fn, &block->term.no, bits, joins, seen);
        }
        for (b = 0; b < fn->block_count; ++b) {
            const ArchonMIRBlock *block = &fn->blocks[b];
            for (i = 0; seen[b] && i < block->param_count; ++i) {
                const ArchonValue value = block->params[i];
                if (fn->types[value] == ARCHON_MIR_I64)
                    prove_value(&bits[value], joins[value], &changed);
            }
        }
        if (!changed)
            break;
    }
    free(seen);
    return bits;
}

/* Post-verification lowering only: do not use a fact to admit unsupported
 * input, and never remove a producer (its effects are independent of its
 * result bits). MIR integer conversions always read/write 64-bit values. */
int an_fact_conversion_is_copy(ANContext *ctx, const ArchonMIRInst *ins)
{
    uint64_t zero, one, sign, upper;
    if (!is_conversion(ins->op) || ins->imm < 1 || ins->imm >= 64 ||
        !an_query_known_bits(ctx, ins->a, &zero, &one))
        return 0;
    sign = UINT64_C(1) << (ins->imm - 1);
    upper = ~(sign * 2 - 1);
    /* A conversion fixes every low bit. TRUNC/ZEXT fill the high bits with
     * zero; SEXT copies the sign into each high bit. Thus the original bit
     * vector is unchanged exactly when its upper bits match that fill.
     * Test sign and upper bits together for SEXT, including width one. */
    if (ins->op != ARCHON_MIR_SEXT)
        return (zero & upper) == upper;
    upper |= sign;
    return (zero & upper) == upper || (one & upper) == upper;
}

/* For wrapping i64 multiplication, x * 2^k modulo 2^64 has bit i = 0
 * for i < k and x[i-k] otherwise: exactly the nontrapping immediate SHL
 * for k in [0, 63]. Checked multiply, FP and pointer operations are excluded.
 * A one-hot known-one mask alone is NOT sufficient: every other bit must
 * be known zero. The removed use is only an operand materialization. */
ArchonValue an_fact_mul_shift(ANContext *ctx, const ArchonMIRInst *ins, unsigned *shift)
{
    const ArchonValue operands[] = {ins->b, ins->a};
    unsigned i;
    if (ins->op != ARCHON_MIR_MUL)
        return ARCHON_MIR_NONE;
    for (i = 0; i < 2; ++i) {
        uint64_t zero, one;
        if (!an_query_known_bits(ctx, operands[i], &zero, &one) ||
            (zero | one) != UINT64_MAX || !one || (one & (one - 1)))
            continue;
        *shift = 0;
        while ((one >>= 1) != 0)
            ++*shift;
        return operands[1 - i];
    }
    return ARCHON_MIR_NONE;
}
#endif
