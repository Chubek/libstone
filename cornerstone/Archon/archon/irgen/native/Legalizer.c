#include "NativeInternal.h"
/* Embedders can reuse the structural verifier without a generated provider. */
#ifndef AN_GENERIC_MIR_VERIFY
#include "TargetNative.h"
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
    case ARCHON_MIR_CALL_INDIRECT: /* Optional destination, func_ptr callee in a. */
        return HAS_DST | HAS_A;
    case ARCHON_MIR_AMD64_VA_START:
    case ARCHON_MIR_AARCH64_VA_START:
        return HAS_A;
    case ARCHON_MIR_STORE:
    case ARCHON_MIR_FSTORE:
    case ARCHON_MIR_STORE8:
    case ARCHON_MIR_STORE16:
    case ARCHON_MIR_STORE32:
    case ARCHON_MIR_STORE_PTR:
    case ARCHON_MIR_STORE_FUNC_PTR:
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
