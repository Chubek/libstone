"""Strict-C11 unit coverage for native MIR operation legality."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


NATIVE = Path(__file__).resolve().parents[1] / "archon" / "irgen" / "native"

CLIENT = r"""
#include "NativeInternal.h"

#include <assert.h>

#define CHECK(inst, status) check((inst), ARCHON_BACKEND_##status)

enum { I64, PTR, VOID };

static const ArchonMIRType types[] = {ARCHON_MIR_I64, ARCHON_MIR_PTR, ARCHON_MIR_VOID};
static const ArchonMIRFunction fn = {
    .types = types, .value_count = sizeof types / sizeof *types, .local_count = 2
};
static const ArchonValue args[] = {I64, PTR};

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    assert(ctx->stage == ARCHON_BACKEND_VERIFY && message && message[0]);
    ctx->diag->status = status;
    return 0;
}

static void check(ArchonMIRInst inst, ArchonBackendStatus expected)
{
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = {.diag = &diag, .stage = ARCHON_BACKEND_VERIFY};
    assert(!!an_legal(&ctx, &fn, &inst) == (expected == ARCHON_BACKEND_OK));
    assert(diag.status == expected);
}

static ArchonMIRInst inst(ArchonMIROpcode op, ArchonValue dst, ArchonValue a, ArchonValue b)
{
    return (ArchonMIRInst){.op = op, .dst = dst, .a = a, .b = b};
}

int main(void)
{
    const ArchonValue none = ARCHON_MIR_NONE;
    const ArchonMIROpcode integer_ops[] = {
        ARCHON_MIR_ADD, ARCHON_MIR_SUB, ARCHON_MIR_MUL,
        ARCHON_MIR_DIV_S, ARCHON_MIR_REM_S, ARCHON_MIR_DIV_U, ARCHON_MIR_REM_U,
        ARCHON_MIR_AND, ARCHON_MIR_OR, ARCHON_MIR_XOR,
        ARCHON_MIR_SHL, ARCHON_MIR_LSHR, ARCHON_MIR_ASHR,
        ARCHON_MIR_EQ, ARCHON_MIR_SLT, ARCHON_MIR_ULT,
        ARCHON_MIR_ADD_CHECKED_S, ARCHON_MIR_SUB_CHECKED_S, ARCHON_MIR_DIV_CHECKED_S
    };
    const uint64_t offsets[] = {
        0, UINT64_C(0) - 8, UINT64_C(0) - 2048, 2040,
        2048, UINT64_C(0) - 2056, 0xff8,
        1, UINT64_MAX, 2047
    };
    const ArchonBackendStatus offset_status[] = {
        ARCHON_BACKEND_OK, ARCHON_BACKEND_OK, ARCHON_BACKEND_OK, ARCHON_BACKEND_OK,
        ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_UNSUPPORTED, ARCHON_BACKEND_UNSUPPORTED,
        ARCHON_BACKEND_INVALID, ARCHON_BACKEND_INVALID, ARCHON_BACKEND_INVALID
    };
    ArchonMIRInst x;
    size_t i;

    assert(an_shape(ARCHON_MIR_CONST) == 1);
    assert(an_shape(ARCHON_MIR_COPY) == 3);
    assert(an_shape(ARCHON_MIR_PTR_ADD) == 7);
    assert(an_shape(ARCHON_MIR_LOAD) == 3 && an_shape(ARCHON_MIR_STORE) == 6);
    assert(an_shape(ARCHON_MIR_FRAME_ADDR) == 1 && an_shape(ARCHON_MIR_GLOBAL_ADDR) == 1);
    assert(an_shape(ARCHON_MIR_CALL) == 1);
    assert(an_shape((ArchonMIROpcode)0) == 0 && an_shape((ArchonMIROpcode)999) == 0);
    CHECK(inst((ArchonMIROpcode)0, none, none, none), UNSUPPORTED);
    for (i = 0; i < sizeof integer_ops / sizeof *integer_ops; ++i) {
        ArchonMIROpcode op = integer_ops[i];
        assert(an_shape(op) == 7);
        CHECK(inst(op, I64, I64, I64), OK);
        CHECK(inst(op, PTR, I64, I64), INVALID);
        CHECK(inst(op, I64, PTR, I64), INVALID);
        CHECK(inst(op, I64, I64, PTR), INVALID);
    }

    x = inst(ARCHON_MIR_CONST, I64, none, none);
    x.imm = UINT64_MAX;
    CHECK(x, OK);
    x.dst = PTR;
    CHECK(x, INVALID);
    CHECK(inst(ARCHON_MIR_CONST, I64, I64, none), INVALID);
    CHECK(inst(ARCHON_MIR_COPY, PTR, PTR, none), OK);
    CHECK(inst(ARCHON_MIR_COPY, I64, I64, none), OK);
    CHECK(inst(ARCHON_MIR_COPY, I64, PTR, none), INVALID);
    CHECK(inst(ARCHON_MIR_COPY, VOID, VOID, none), INVALID);
    CHECK(inst(ARCHON_MIR_COPY, I64, UINT32_MAX - 1, none), INVALID);
    CHECK(inst(ARCHON_MIR_PTR_ADD, PTR, PTR, I64), OK);
    CHECK(inst(ARCHON_MIR_PTR_ADD, I64, PTR, I64), INVALID);
    CHECK(inst(ARCHON_MIR_PTR_ADD, PTR, I64, I64), INVALID);
    CHECK(inst(ARCHON_MIR_PTR_ADD, PTR, PTR, PTR), INVALID);

    for (i = 0; i < sizeof offsets / sizeof *offsets; ++i) {
        x = inst(ARCHON_MIR_LOAD, I64, PTR, none);
        x.imm = offsets[i];
        check(x, offset_status[i]);
        x = inst(ARCHON_MIR_STORE, none, PTR, I64);
        x.imm = offsets[i];
        check(x, offset_status[i]);
    }
    CHECK(inst(ARCHON_MIR_LOAD, I64, I64, none), INVALID);
    CHECK(inst(ARCHON_MIR_LOAD, PTR, PTR, none), INVALID);
    CHECK(inst(ARCHON_MIR_STORE, none, I64, I64), INVALID);
    CHECK(inst(ARCHON_MIR_STORE, none, PTR, PTR), INVALID);
    CHECK(inst(ARCHON_MIR_STORE, I64, PTR, I64), INVALID);

    x = inst(ARCHON_MIR_FRAME_ADDR, PTR, none, none);
    CHECK(x, OK);
    x.imm = 1;
    CHECK(x, OK);
    x.imm = 2;
    CHECK(x, INVALID);
    x.imm = UINT64_MAX;
    CHECK(x, INVALID);
    CHECK(inst(ARCHON_MIR_FRAME_ADDR, I64, none, none), INVALID);

    x = inst(ARCHON_MIR_GLOBAL_ADDR, PTR, none, none);
    CHECK(x, INVALID);
    x.symbol = "g";
    CHECK(x, OK);
    x.dst = I64;
    CHECK(x, INVALID);
    x = inst(ARCHON_MIR_CALL, none, none, none);
    CHECK(x, INVALID);
    x.symbol = "callee";
    CHECK(x, OK);
    CHECK(inst(ARCHON_MIR_CALL, none, none, I64), INVALID);
    x.arg_count = 2;
    CHECK(x, INVALID);
    x.args = args;
    CHECK(x, OK);
    x.dst = I64;
    CHECK(x, OK);
    x.dst = PTR;
    CHECK(x, OK);
    x.dst = VOID;
    CHECK(x, INVALID);
    x.dst = PTR;
    x.imm = 1;
    CHECK(x, INVALID);

    x = inst(ARCHON_MIR_COPY, I64, I64, none);
    x.imm = 1;
    CHECK(x, INVALID);
    x.imm = 0;
    x.symbol = "g";
    CHECK(x, INVALID);
    x.symbol = NULL;
    x.args = args;
    CHECK(x, INVALID);
    x.args = NULL;
    x.arg_count = 1;
    CHECK(x, INVALID);
    return 0;
}
"""


class NativeLegalizerTests(unittest.TestCase):
    def test_operand_and_immediate_legality(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="archon-native-legalizer-") as tmp:
            client = Path(tmp) / "client.c"
            binary = Path(tmp) / "client"
            client.write_text(CLIENT, encoding="utf-8")
            subprocess.run(
                [
                    *cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(NATIVE), str(NATIVE / "Legalizer.c"),
                    str(client), "-o", str(binary),
                ],
                check=True,
                timeout=30,
            )
            subprocess.run([binary], check=True, timeout=5)
