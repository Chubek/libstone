"""Strict-C99 tests of native list scheduling and its independent verifier."""

import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory


ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "archon" / "irgen" / "native"
TMP = "/tmp/opencode" if Path("/tmp/opencode").is_dir() else None
PROGRAM = """\
archon-mir 1
module 1 1 0
function schedule_probe i64 2 10 1 0
types ptr i64 i64 i64 i64 i64 i64 i64 i64 i64
block 0 9
params
xor 2 1 1
sub 3 1 1
mul 4 3 1
store 0 4 0
call 5 observe 1 2
load 6 0 8
or 7 5 5
slt 8 6 5
sub 9 8 1
ret 9
import observe i64 1 i64
end
"""


class NativeListScheduleTests(unittest.TestCase):
    def test_generated_driver_opt_in_reorders_regions(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        cmake = shutil.which("cmake")
        if not cmake or not cc or not shutil.which(cc[0]):
            self.skipTest("CMake and a host C99 compiler are required")
        with tempfile.TemporaryDirectory(prefix="archon-schedule-driver-", dir=TMP) as directory:
            root = Path(directory)
            generated = write_generated_directory(
                generate_ir(
                    "gated", [compile_file(ROOT / "stdspec" / "riscv" / "rv64im.arch")],
                    options={"profile": "native", "abi": "LP64", "oracle": False},
                ),
                root / "generated",
            )
            build = root / "build"
            for args in (("-S", generated, "-B", build, f"-DCMAKE_C_COMPILER={';'.join(cc)}",
                          "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic"),
                         ("--build", build, "--target", "archon-backend")):
                built = subprocess.run(
                    [cmake, *args], capture_output=True, text=True, cwd=root, timeout=60,
                )
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)

            def assembly(*flags):
                result = subprocess.run(
                    [build / "archon-backend", "--input", "-", *flags],
                    input=PROGRAM, capture_output=True, text=True, cwd=root, timeout=10,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout

            def operations(output):
                result = []
                for row in output.splitlines():
                    line = row.strip()
                    op = line.split(" ", 1)[0]
                    if (op in {"xor", "sub", "mul", "or", "slt"} or line == "call observe" or
                            re.fullmatch(r"sd x\d+, 0\(x5\)", line) or
                            re.fullmatch(r"ld x\d+, 8\(x5\)", line)):
                        result.append(op)
                return result

            source_order = assembly()
            scheduled = assembly("--schedule-pure")
            self.assertEqual(source_order, assembly())
            self.assertEqual(scheduled, assembly("--schedule-pure"))
            self.assertEqual(operations(source_order),
                             ["xor", "sub", "mul", "sd", "call", "ld", "or", "slt", "sub"])
            self.assertEqual(operations(scheduled),
                             ["sub", "xor", "mul", "sd", "call", "ld", "slt", "or", "sub"])
            invalid = subprocess.run(
                [build / "archon-backend", "--input", "-", "--schedule-pure"],
                input=PROGRAM.replace("sub 3 1 1", "sub 3 4 1"),
                capture_output=True, text=True, cwd=root, timeout=10,
            )
            self.assertEqual((invalid.returncode, invalid.stdout), (1, ""))
            self.assertIn("does not dominate", invalid.stderr)

    def test_scheduling_and_verifier(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        with tempfile.TemporaryDirectory(prefix="archon-list-schedule-", dir=TMP) as directory:
            root = Path(directory)
            source = root / "test.c"
            source.write_text(SOURCE, encoding="ascii")
            executable = root / "test"
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", NATIVE, *(NATIVE / name for name in (
                     "Legalizer.c", "NativeVerify.c", "NativeSchedule.c",
                 )), source, "-o", executable],
                check=True, timeout=30,
            )
            subprocess.run([executable], check=True, cwd=root, timeout=10)


SOURCE = r"""
#include "NativeInternal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define N ARCHON_MIR_NONE
#define I ARCHON_MIR_I64
#define P ARCHON_MIR_PTR
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define INS(op, dst, a, b, imm) { ARCHON_MIR_##op, dst, a, b, imm, NULL, NULL, 0 }

/* Real SSA, legality and schedule verification without target emission. */
const ANABI an_abi = { .stack_align = 16 };

int an_symbol(const char *name)
{
    return name && *name;
}

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    if (ctx->diag->status == ARCHON_BACKEND_OK) {
        ctx->diag->status = status;
        ctx->diag->instruction = ctx->instruction;
        strcpy(ctx->diag->message, message);
    }
    return 0;
}

int an_continue(ANContext *ctx)
{
    if (ctx->diag->status != ARCHON_BACKEND_OK)
        return 0;
    if (ctx->options && ctx->options->cancelled &&
        ctx->options->cancelled(ctx->options->user))
        return an_error(ctx, ARCHON_BACKEND_CANCELLED, "cancelled");
    return 1;
}

static void same_inst(const ArchonMIRInst *a, const ArchonMIRInst *b)
{
    assert(a->op == b->op && a->dst == b->dst && a->a == b->a && a->b == b->b);
    assert(a->imm == b->imm && a->symbol == b->symbol &&
           a->args == b->args && a->arg_count == b->arg_count);
}

static void reject(ANContext *ctx, const ArchonMIRBlock *block,
    const size_t *order, size_t count, const char *message)
{
    ctx->diag->status = ARCHON_BACKEND_OK;
    assert(!an_verify_schedule(ctx, block, order, count));
    assert(ctx->diag->status == ARCHON_BACKEND_INTERNAL);
    assert(strstr(ctx->diag->message, message));
}

static int cancel_on(void *data)
{
    size_t *remaining = data;
    return --*remaining == 0;
}

static int count_poll(void *data)
{
    size_t *calls = data;
    ++*calls;
    return 0;
}

static void regions(void)
{
    const ArchonMIRType types[] = { P, I, I, I, I, I, I, I, I, I,
                                   I, I, I, I, I, I, I, I, I };
    const ArchonMIRType formal[] = { I, I };
    const ArchonValue args[] = { 2, 9 };
    const ArchonMIRInst insts[] = {
        INS(CONST, 2, N, N, 42),
        INS(CONST, 3, N, N, 7),
        INS(COPY, 4, 3, N, 0),
        INS(SLT, 5, 4, 4, 0), /* Two reads of the same definition. */
        INS(STORE, N, 0, 2, 0),
        INS(LOAD, 6, 0, N, 8),
        INS(XOR, 7, 1, 1, 0),
        INS(DIV_U, 8, 6, 1, 0),
        INS(REM_U, 9, 8, 1, 0),
        { ARCHON_MIR_CALL, 10, N, N, 0, "observe", args, COUNT(args) },
        INS(OR, 11, 10, 1, 0),
        INS(SHL, 12, 10, 1, 0),
        INS(ULT, 13, 12, 5, 0),
        INS(ADD_CHECKED_S, 14, 10, 13, 0),
        INS(ADD, 15, 5, 1, 0),
        INS(COPY, 16, 14, N, 0),
        INS(AND, 17, 16, 2, 0),
        INS(EQ, 18, 17, 1, 0),
    };
    const ArchonMIRBlock block = {
        .insts = insts, .inst_count = COUNT(insts),
        .term = { .kind = ARCHON_MIR_TRAP, .value = N }
    };
    const ArchonMIRImport observe = { "observe", I, formal, COUNT(formal) };
    ArchonMIRFunction fn = {
        "regions", ARCHON_MIR_VOID, types, COUNT(types), 2, &block, 1, NULL, 0
    };
    const ArchonMIRModule module = {
        ARCHON_MIR_SCHEMA_VERSION, &fn, 1, &observe, 1, NULL, 0
    };
    const size_t expected[] = { 1, 2, 0, 3, 4, 5, 7, 6, 8, 9, 11, 10, 12,
                                13, 15, 16, 14, 17 };
    size_t bad[COUNT(insts)], i, polls = 0;
    ArchonMIRInst scheduled[COUNT(insts)], repeat[COUNT(insts)];
    ArchonMIRBlock reordered = block;
    ArchonBackendDiagnostic diag = {0};
    ArchonBackendOptions opts = { .cancelled = count_poll, .user = &polls };
    ANContext ctx = { .diag = &diag };
    assert(an_verify(&ctx, &module));
    ctx.options = &opts;
    assert(an_schedule_block(&ctx, &block, scheduled));
    assert(polls > 1);
    ctx.options = NULL;
    assert(an_schedule_block(&ctx, &block, repeat));
    for (i = 0; i < COUNT(insts); ++i) {
        same_inst(&scheduled[i], &insts[expected[i]]);
        same_inst(&repeat[i], &scheduled[i]);
    }
    assert(an_verify_schedule(&ctx, &block, expected, COUNT(expected)));
    for (i = 0; i < COUNT(insts); ++i)
        bad[i] = i;
    assert(an_verify_schedule(&ctx, &block, bad, COUNT(bad)));

    /* The reordered block, including call arguments and the unchanged
     * terminator, must still satisfy the full MIR verifier. */
    reordered.insts = scheduled;
    fn.blocks = &reordered;
    assert(an_verify(&ctx, &module));
    assert(reordered.term.kind == ARCHON_MIR_TRAP);

    reject(&ctx, &block, expected, COUNT(expected) - 1, "length");
    reject(&ctx, &block, NULL, COUNT(expected), "length");
    memcpy(bad, expected, sizeof(bad));
    bad[0] = SIZE_MAX;
    reject(&ctx, &block, bad, COUNT(bad), "boundary");
    memcpy(bad, expected, sizeof(bad));
    bad[2] = bad[0];
    reject(&ctx, &block, bad, COUNT(bad), "repeated");
    memcpy(bad, expected, sizeof(bad));
    bad[0] = expected[1];
    bad[1] = expected[0];
    reject(&ctx, &block, bad, COUNT(bad), "precedes");
    memcpy(bad, expected, sizeof(bad));
    bad[4] = expected[5];
    bad[5] = expected[4];
    reject(&ctx, &block, bad, COUNT(bad), "boundary"); /* Memory effect order. */
    /* An independent scalar cannot cross a memory, call, or checked-arithmetic
     * boundary, even when all the effectful instructions stay pinned. */
    memcpy(bad, expected, sizeof(bad));
    bad[3] = expected[7];
    bad[7] = expected[3];
    reject(&ctx, &block, bad, COUNT(bad), "boundary");
    memcpy(bad, expected, sizeof(bad));
    bad[7] = expected[11];
    bad[11] = expected[7];
    reject(&ctx, &block, bad, COUNT(bad), "boundary");
    memcpy(bad, expected, sizeof(bad));
    bad[11] = expected[16];
    bad[16] = expected[11];
    reject(&ctx, &block, bad, COUNT(bad), "boundary");

    /* Cancel even at the last checkpoint before publication. */
    diag.status = ARCHON_BACKEND_OK;
    opts.cancelled = cancel_on;
    ctx.options = &opts;
    memcpy(repeat, insts, sizeof(repeat));
    assert(!an_schedule_block(&ctx, &block, repeat));
    assert(diag.status == ARCHON_BACKEND_CANCELLED && !polls);
    for (i = 0; i < COUNT(insts); ++i)
        same_inst(&repeat[i], &insts[i]);
}

static void at_limit(void)
{
    ArchonMIRInst *insts = malloc(AN_MAX_INSTS * sizeof(*insts));
    ArchonMIRInst *out = malloc(AN_MAX_INSTS * sizeof(*out));
    ArchonMIRType *types = malloc(AN_MAX_VALUE * sizeof(*types));
    ArchonMIRBlock block = {
        .insts = insts, .inst_count = AN_MAX_INSTS,
        .term = { .kind = ARCHON_MIR_RETURN, .value = AN_MAX_VALUE - 1 }
    };
    const ArchonMIRFunction fn = {
        "limit", I, types, AN_MAX_VALUE, 1, &block, 1, NULL, 0
    };
    const ArchonMIRModule module = {
        ARCHON_MIR_SCHEMA_VERSION, &fn, 1, NULL, 0, NULL, 0
    };
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag };
    size_t i;
    assert(insts && out && types);
    types[0] = P;
    for (i = 1; i < AN_MAX_VALUE; ++i)
        types[i] = I;
    /* Put a long chain at the end of a maximal-size block. Value 1 is
     * defined before the stores; the last region begins by defining 2. */
    insts[0] = (ArchonMIRInst)INS(CONST, 1, N, N, 0);
    for (i = 1; i < AN_MAX_INSTS - AN_MAX_VALUE + 2u; ++i)
        insts[i] = (ArchonMIRInst)INS(STORE, N, 0, 1, 0);
    for (; i < AN_MAX_INSTS; ++i) {
        const ArchonValue dst = (ArchonValue)(i - (AN_MAX_INSTS - AN_MAX_VALUE));
        insts[i] = (ArchonMIRInst)INS(COPY, dst, dst - 1u, N, 0);
    }
    assert(an_verify(&ctx, &module));
    assert(an_schedule_block(&ctx, &block, out));
    for (i = 0; i < AN_MAX_INSTS; ++i)
        same_inst(&insts[i], &out[i]);
    block.inst_count++;
    assert(!an_schedule_block(&ctx, &block, out));
    assert(diag.status == ARCHON_BACKEND_LIMIT);
    free(types);
    free(out);
    free(insts);
}

int main(void)
{
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag };
    const ArchonMIRBlock empty = { .term = { .kind = ARCHON_MIR_TRAP, .value = N } };
    assert(an_schedule_block(&ctx, &empty, NULL));
    assert(an_verify_schedule(&ctx, &empty, NULL, 0));
    regions();
    at_limit();
    return 0;
}
"""


if __name__ == "__main__":
    unittest.main()
