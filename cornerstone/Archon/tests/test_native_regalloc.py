"""Strict-C99 tests for the native SSA register allocator."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from native_test_support import write_native_abi

NATIVE = Path(__file__).resolve().parents[1] / "archon" / "irgen" / "native"


class NativeRegalloc(unittest.TestCase):
    def test_register_homes(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None) as directory:
            root = Path(directory)
            source = root / "test.c"
            source.write_text(SOURCE, encoding="ascii")
            (root / "TargetNative.h").write_text("#define AN_RV64_HAS_M 1\n", encoding="ascii")
            abi = write_native_abi(root)
            names = ("ABI", "FrameLowering", "Legalizer", "NativeHomes", "NativeRegalloc",
                     "NativeVerify")
            executable = root / "test"
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(NATIVE), "-I", str(root), *(str(NATIVE / f"{name}.c") for name in names),
                 str(abi), str(source), "-o", str(executable)],
                check=True,
            )
            subprocess.run([str(executable)], check=True)


SOURCE = r"""
#include "NativeInternal.h"
#include <assert.h>
#include <stdlib.h>

#define N ARCHON_MIR_NONE
#define I ARCHON_MIR_I64
#define V ARCHON_MIR_VOID
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define INS(op, dst, a, b) { ARCHON_MIR_##op, dst, a, b, 0, NULL, NULL, 0 }
#define CALL(dst, name, args) \
    { ARCHON_MIR_CALL, dst, N, N, 0, name, args, COUNT(args) }
#define INDIRECT(dst, callee, signature, args) \
    { ARCHON_MIR_CALL_INDIRECT, dst, callee, N, 0, signature, args, COUNT(args) }

/* Use real allocation and frame lowering without linking target emission. */
static unsigned home_loads, home_stores;

void an_call_symbol(ANOut *out, const char *symbol)
{
    (void)out; (void)symbol;
    abort();
}

void an_emit(ANOut *out, ANOpcode op, unsigned rd, unsigned a, unsigned b, int imm)
{
    (void)out; (void)rd; (void)a; (void)b; (void)imm;
    home_loads += op == AN_LD;
    home_stores += op == AN_SD;
}

int an_symbol(const char *name)
{
    return name && *name;
}

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    (void)message;
    ctx->diag->status = status;
    ctx->diag->stage = ctx->stage;
    ctx->diag->block = ctx->block;
    ctx->diag->instruction = ctx->instruction;
    return 0;
}

int an_continue(ANContext *ctx)
{
    if (ctx->options && ctx->options->cancelled &&
        ctx->options->cancelled(ctx->options->user))
        return an_error(ctx, ARCHON_BACKEND_CANCELLED, "cancelled");
    return 1;
}

static void check_read(const ANHome *homes, const ArchonValue *owner,
    ArchonValue value)
{
    if (value != N && homes[value].reg != AN_ZERO)
        assert(owner[homes[value].reg] == value);
}

/* Simulate clobbers at instruction granularity. This rejects collisions
 * without duplicating the allocator's last-use or call-crossing analysis. */
static void check_owners(const ArchonMIRFunction *fn, const ANHome *homes)
{
    size_t b, i, j;
    for (b = 0; b < fn->block_count; ++b) {
        const ArchonMIRBlock *bb = &fn->blocks[b];
        ArchonValue owner[AN_T6 + 1];
        unsigned r;
        for (r = AN_T4; r <= AN_T6; ++r)
            owner[r] = N;
        for (i = 0; i < bb->inst_count; ++i) {
            const ArchonMIRInst *inst = &bb->insts[i];
            unsigned shape = an_shape(inst->op);
            if (shape & 2u)
                check_read(homes, owner, inst->a);
            if (shape & 4u)
                check_read(homes, owner, inst->b);
            if (inst->op == ARCHON_MIR_CALL || inst->op == ARCHON_MIR_CALL_INDIRECT) {
                for (j = 0; j < inst->arg_count; ++j)
                    check_read(homes, owner, inst->args[j]);
                for (r = AN_T4; r <= AN_T6; ++r)
                    owner[r] = N;
            }
            if ((shape & 1u) && inst->dst != N && homes[inst->dst].reg)
                owner[homes[inst->dst].reg] = inst->dst;
        }
        if (bb->term.kind == ARCHON_MIR_RETURN || bb->term.kind == ARCHON_MIR_CBR)
            check_read(homes, owner, bb->term.value);
        for (j = 0; j < bb->term.yes.arg_count; ++j)
            assert(homes[bb->term.yes.args[j]].reg == AN_ZERO);
        for (j = 0; j < bb->term.no.arg_count; ++j)
            assert(homes[bb->term.no.args[j]].reg == AN_ZERO);
    }
}

static size_t allocate(const ArchonMIRFunction *fn, const ArchonMIRImport *imports,
    size_t import_count, ANHome *homes)
{
    const ArchonMIRModule module = {
        .version = ARCHON_MIR_SCHEMA_VERSION, .functions = fn, .function_count = 1,
        .imports = imports, .import_count = import_count
    };
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag };
    size_t i, slots, expected = 0;
    assert(an_verify(&ctx, &module));
    assert(an_allocate(&ctx, fn, homes, &slots));
    for (i = 0; i < fn->value_count; ++i) {
        if (homes[i].reg == AN_ZERO)
            assert(homes[i].slot == expected++);
        else
            assert(homes[i].reg >= AN_T4 && homes[i].reg <= AN_T6);
    }
    assert(slots == expected);
    assert(an_verify_homes(&ctx, fn, homes, slots));
    check_owners(fn, homes);
    return slots;
}

static void pressure(void)
{
    const ArchonMIRType types[] = { I, I, I, I, I, I, I };
    const ArchonMIRInst instructions[] = {
        INS(CONST, 0, N, N), INS(CONST, 1, N, N),
        INS(CONST, 2, N, N), INS(CONST, 3, N, N),
        INS(ADD, 4, 0, 1), INS(ADD, 5, 2, 3), INS(ADD, 6, 4, 5),
    };
    const ArchonMIRBlock blocks[] = {{
        .insts = instructions, .inst_count = COUNT(instructions),
        .term = { .kind = ARCHON_MIR_RETURN, .value = 6 }
    }};
    const ArchonMIRFunction fn = { "pressure", I, types, COUNT(types), 0,
        blocks, COUNT(blocks), NULL, 0 };
    ANHome homes[COUNT(types)];
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag, .stage = ARCHON_BACKEND_ALLOCATE };
    assert(allocate(&fn, NULL, 0, homes) == 1);
    assert(homes[0].reg == AN_T4 && homes[1].reg == AN_T4 + 1);
    assert(homes[2].reg == AN_T6 && homes[3].reg == AN_ZERO);
    assert(homes[4].reg == AN_T4 && homes[5].reg == AN_T4 + 1);
    assert(homes[6].reg == AN_T4);
    homes[1].reg = homes[0].reg;
    assert(!an_verify_homes(&ctx, &fn, homes, 1));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 4);
}

/* Count the stack traffic for homes, not for the MIR operations themselves. */
static void check_home_traffic(const ArchonMIRBlock *block, const ANHome *homes,
    unsigned expected_loads, unsigned expected_stores)
{
    const ANFrame frame = {0};
    ANOut out = {0};
    size_t i;
    home_loads = home_stores = 0;
    for (i = 0; i < block->inst_count; ++i) {
        const ArchonMIRInst *inst = &block->insts[i];
        unsigned shape = an_shape(inst->op);
        if (shape & 2u)
            an_read(&out, &frame, homes, inst->a, AN_T0);
        if (shape & 4u)
            an_read(&out, &frame, homes, inst->b, AN_T1);
        if (shape & 1u)
            an_write(&out, &frame, homes, inst->dst, AN_T2);
    }
    an_read(&out, &frame, homes, block->term.value, AN_A0);
    assert(home_loads == expected_loads && home_stores == expected_stores);
}

static void evict_late_interval(void)
{
    const ArchonMIRType types[] = { ARCHON_MIR_PTR, I, I, I, I, I, I, I };
    const ArchonMIRInst instructions[] = {
        INS(CONST, 1, N, N), INS(CONST, 2, N, N), INS(CONST, 3, N, N),
        INS(STORE, N, 0, 2),
        INS(CONST, 4, N, N),
        INS(STORE, N, 0, 4), INS(STORE, N, 0, 4), INS(STORE, N, 0, 4),
        INS(COPY, 5, 1, N), INS(ADD, 6, 2, 3), INS(ADD, 7, 5, 6),
    };
    const ArchonMIRBlock blocks[] = {{
        .insts = instructions, .inst_count = COUNT(instructions),
        .term = { .kind = ARCHON_MIR_RETURN, .value = 7 }
    }};
    const ArchonMIRFunction fn = { "evict", I, types, COUNT(types), 1,
        blocks, COUNT(blocks), NULL, 0 };
    const ANHome spill_incoming[] = {
        { AN_ZERO, 0 }, { AN_T4, 0 }, { AN_T4 + 1, 0 }, { AN_T6, 0 },
        { AN_ZERO, 1 }, { AN_T4, 0 }, { AN_T4 + 1, 0 }, { AN_T4, 0 },
    };
    ANHome homes[COUNT(types)];
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag, .stage = ARCHON_BACKEND_ALLOCATE };

    assert(allocate(&fn, NULL, 0, homes) == 2);
    /* Values 2 and 3 end at the same later instruction. Evict 2, the
     * lower register, even though it was already read once. */
    assert(homes[2].reg == AN_ZERO && homes[2].slot == 1);
    assert(homes[3].reg == AN_T6 && homes[4].reg == AN_T4 + 1);
    assert(an_verify_homes(&ctx, &fn, spill_incoming, 2));
    check_home_traffic(blocks, spill_incoming, 7, 1);
    check_home_traffic(blocks, homes, 6, 1);

    /* Wrongly keeping the victim in its former register loses its
     * value at its later use, despite the otherwise valid slot count. */
    homes[2].reg = homes[4].reg;
    assert(!an_verify_homes(&ctx, &fn, homes, 1));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 9);
}

static void calls(void)
{
    const ArchonMIRType types[] = { I, I, I, I, I, I, I, I, I };
    const ArchonMIRType signature[] = { I, I, I, I, I, I, I, I, I, I };
    const ArchonMIRImport imports[] = {
        { "ten", I, signature, 10 }, { "two", I, signature, 2 },
        { "lastuse", V, signature, 1 }, { "zero", V, NULL, 0 }
    };
    const ArchonValue ten[] = { 1, 0, 0, 1, 0, 0, 1, 0, 3, 4 };
    const ArchonValue two[] = { 2, 5 }, lastuse[] = { 6 };
    const ArchonMIRInst instructions[] = {
        INS(CONST, 1, N, N), INS(CONST, 2, N, N), INS(CONST, 3, N, N),
        INS(CONST, 4, N, N), CALL(5, "ten", ten), CALL(6, "two", two),
        CALL(N, "lastuse", lastuse), { ARCHON_MIR_CALL, N, N, N, 0, "zero", NULL, 0 },
        INS(CONST, 7, N, N), INS(ADD, 8, 6, 7)
    };
    const ArchonMIRBlock blocks[] = {{
        .insts = instructions, .inst_count = COUNT(instructions),
        .term = { .kind = ARCHON_MIR_RETURN, .value = 8 }
    }};
    const ArchonMIRFunction fn = { "calls", I, types, COUNT(types), 1,
        blocks, COUNT(blocks), NULL, 0 };
    ANHome homes[COUNT(types)];
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag, .stage = ARCHON_BACKEND_ALLOCATE };
    assert(allocate(&fn, imports, COUNT(imports), homes) == 3);
    assert(homes[0].reg == AN_ZERO);
    assert(homes[1].reg == AN_T4 && homes[2].reg == AN_ZERO);
    assert(homes[3].reg == AN_T4 + 1 && homes[4].reg == AN_T6);
    assert(homes[5].reg == AN_T4 && homes[6].reg == AN_ZERO);
    assert(homes[7].reg == AN_T4 && homes[8].reg == AN_T4);
    homes[6].reg = AN_T6;
    assert(!an_verify_homes(&ctx, &fn, homes, 2));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 9);
}

static void indirect_call(void)
{
    const ArchonMIRType types[] = { ARCHON_MIR_FUNC_PTR, ARCHON_MIR_FUNC_PTR, I, I, I, I, I };
    const ArchonMIRType signature[] = { I };
    const ArchonMIRImport imports[] = {{ "callee_signature", I, signature, 1 }};
    const ArchonValue args[] = { 3 };
    const ArchonMIRInst instructions[] = {
        INS(COPY, 1, 0, N), INS(CONST, 2, N, N), INS(CONST, 3, N, N),
        INS(CONST, 6, N, N),
        INDIRECT(4, 1, "callee_signature", args), INS(ADD, 5, 2, 4),
    };
    const ArchonMIRBlock blocks[] = {{
        .insts = instructions, .inst_count = COUNT(instructions),
        .term = { .kind = ARCHON_MIR_RETURN, .value = 5 }
    }};
    const ArchonMIRFunction fn = { "indirect", I, types, COUNT(types), 1,
        blocks, COUNT(blocks), NULL, 0 };
    ANHome homes[COUNT(types)];
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag, .stage = ARCHON_BACKEND_ALLOCATE };

    assert(allocate(&fn, imports, COUNT(imports), homes) == 2);
    assert(homes[1].reg == AN_T4 && homes[3].reg == AN_T4 + 1);
    assert(homes[2].reg == AN_ZERO && homes[4].reg == AN_T4 && homes[6].reg == AN_T6);

    homes[1].reg = homes[3].reg;
    assert(!an_verify_homes(&ctx, &fn, homes, 2));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 4);
    homes[1].reg = AN_T4;
    homes[3].reg = homes[6].reg;
    assert(!an_verify_homes(&ctx, &fn, homes, 2));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 4);
    homes[3].reg = AN_T4 + 1;
    /* Move value 2 into an otherwise unused register: only the call
     * clobber should make the final ADD's use invalid. */
    homes[2].reg = AN_T6;
    homes[6].reg = AN_ZERO;
    homes[6].slot = homes[2].slot;
    assert(!an_verify_homes(&ctx, &fn, homes, 2));
    assert(diag.status == ARCHON_BACKEND_INTERNAL && diag.instruction == 5);
}

static void control_flow(void)
{
    const ArchonMIRType types[] = { I, I, I, I, I, I, I, I, I };
    const ArchonMIRInst entry[] = {
        INS(CONST, 0, N, N), INS(CONST, 1, N, N), INS(CONST, 2, N, N),
        INS(CONST, 3, N, N),
    };
    const ArchonMIRInst exit[] = { INS(ADD, 4, 7, 5), INS(CONST, 8, N, N) };
    const ArchonMIRInst join[] = { INS(CONST, 7, N, N) };
    const ArchonValue yes[] = { 0 }, no[] = { 1 }, phi[] = { 5 }, join_phi[] = { 6 };
    const ArchonMIRBlock blocks[] = {
        { .insts = entry, .inst_count = COUNT(entry),
          .term = { ARCHON_MIR_CBR, 2, { 2, yes, 1 }, { 2, no, 1 } } },
        { .params = phi, .param_count = 1, .insts = exit, .inst_count = COUNT(exit),
          .term = { .kind = ARCHON_MIR_RETURN, .value = 4 } },
        { .params = join_phi, .param_count = 1, .insts = join, .inst_count = COUNT(join),
          .term = { .kind = ARCHON_MIR_BR, .value = N, .yes = { 1, join_phi, 1 } } },
    };
    const ArchonMIRFunction fn = { "control", I, types, COUNT(types), 0,
        blocks, COUNT(blocks), NULL, 0 };
    ANHome homes[COUNT(types)];
    assert(allocate(&fn, NULL, 0, homes) == 5);
    assert(homes[0].reg == AN_ZERO && homes[1].reg == AN_ZERO);
    assert(homes[2].reg == AN_T4 && homes[3].reg == AN_T4 + 1);
    assert(homes[4].reg == AN_T4 && homes[8].reg == AN_T4 + 1);
    assert(homes[5].reg == AN_ZERO && homes[6].reg == AN_ZERO);
    assert(homes[7].reg == AN_ZERO); /* Defined in block 2; live into block 1. */
}

static int stop_after(void *user)
{
    unsigned *remaining = user;
    return --*remaining == 0;
}

static void large_block(void)
{
    ArchonMIRType *types = malloc(AN_MAX_VALUE * sizeof(*types));
    ArchonMIRInst *insts = malloc(AN_MAX_INSTS * sizeof(*insts));
    ANHome *homes = malloc(AN_MAX_VALUE * sizeof(*homes));
    ArchonMIRBlock block = {
        .insts = insts, .inst_count = AN_MAX_INSTS,
        .term = { .kind = ARCHON_MIR_RETURN, .value = AN_MAX_VALUE - 1 }
    };
    const ArchonMIRFunction fn = { "large", I, types, AN_MAX_VALUE, 1,
        &block, 1, NULL, 0 };
    ArchonBackendDiagnostic diag = {0};
    unsigned remaining = 4;
    ArchonBackendOptions options = { 0, stop_after, &remaining };
    ANContext ctx = { .diag = &diag, .options = &options };
    size_t i, slots = SIZE_MAX;

    assert(types && insts && homes);
    types[0] = ARCHON_MIR_PTR;
    types[1] = I;
    insts[0] = (ArchonMIRInst)INS(CONST, 1, N, N);
    /* Maximize the dependent value chain within the value table limit. */
    for (i = 2; i < AN_MAX_VALUE; ++i) {
        types[i] = I;
        insts[i - 1] = (ArchonMIRInst)INS(COPY, (ArchonValue)i, (ArchonValue)i - 1, N);
    }
    for (i = AN_MAX_VALUE - 1; i < AN_MAX_INSTS; ++i)
        insts[i] = (ArchonMIRInst)INS(STORE, N, 0, AN_MAX_VALUE - 1);
    assert(allocate(&fn, NULL, 0, homes) == 1);
    for (i = 1; i < AN_MAX_VALUE; ++i)
        assert(homes[i].reg == AN_T4);
    assert(!an_allocate(&ctx, &fn, homes, &slots));
    assert(diag.status == ARCHON_BACKEND_CANCELLED);
    assert(diag.stage == ARCHON_BACKEND_ALLOCATE && diag.block == 0);
    assert(diag.instruction > 0 && diag.instruction < AN_MAX_INSTS);
    assert(slots == SIZE_MAX);

    /* Same size, maximum pressure: every temporary is live until all
     * definitions complete. This also forces large frame-slot displacements. */
    for (i = 0; i < AN_MAX_VALUE - 1; ++i)
        insts[i] = (ArchonMIRInst)INS(CONST, (ArchonValue)i + 1, N, N);
    for (i = 0; i < AN_MAX_VALUE - 1; ++i)
        insts[AN_MAX_VALUE - 1 + i] = (ArchonMIRInst)INS(STORE, N, 0, (ArchonValue)i + 1);
    assert(allocate(&fn, NULL, 0, homes) == AN_MAX_VALUE - (AN_T6 - AN_T4 + 1));
    free(homes);
    free(insts);
    free(types);
}

int main(void)
{
    pressure();
    evict_late_interval();
    calls();
    indirect_call();
    control_flow();
    large_block();
    return 0;
}
"""


if __name__ == "__main__":
    unittest.main()
