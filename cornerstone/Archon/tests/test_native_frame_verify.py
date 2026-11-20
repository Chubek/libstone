"""Test the RV64 frame layout and its independent, pre-emission verifier."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from native_test_support import write_native_abi

NATIVE = Path(__file__).resolve().parents[1] / "archon" / "irgen" / "native"


class NativeFrameVerifyTests(unittest.TestCase):
    def test_frame_faults_and_exact_displacement_limits(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None) as root:
            names = ("ABI", "FrameLowering", "NativeFrameVerify")
            abi = write_native_abi(root)
            subprocess.run(
                [
                    *cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(NATIVE), *(str(NATIVE / f"{name}.c") for name in names),
                    str(abi), "-x", "c", "-", "-o", "test",
                ],
                input=HARNESS, text=True, cwd=root, check=True, timeout=30,
            )
            subprocess.run([str(Path(root) / "test")], check=True, timeout=10)


HARNESS = r"""
#include "NativeInternal.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define NONE ARCHON_MIR_NONE
#define I64 ARCHON_MIR_I64

/* Exercise layout and verification without allocating the frame or emitting
 * code. Spots that would emit in FrameLowering and ABI are stubs. */
void an_emit(ANOut *out, ANOpcode op, unsigned a, unsigned b, unsigned c, int imm)
{
    (void)out; (void)op; (void)a; (void)b; (void)c; (void)imm;
    assert(0 && "unexpected emission");
}

void an_call_symbol(ANOut *out, const char *symbol)
{
    (void)out; (void)symbol;
    assert(0 && "unexpected call emission");
}

int an_error(ANContext *ctx, ArchonBackendStatus status, const char *message)
{
    ctx->diag->status = status;
    ctx->diag->stage = ctx->stage;
    (void)snprintf(ctx->diag->message, sizeof ctx->diag->message, "%s", message);
    return 0;
}

int an_continue(ANContext *ctx)
{
    return ctx->diag->status == ARCHON_BACKEND_OK;
}

static void reject(ANContext *ctx, const ArchonMIRFunction *fn, size_t slots,
    size_t phi, size_t outgoing, int calls, const ANFrame *frame, const char *why)
{
    *ctx->diag = (ArchonBackendDiagnostic){0};
    assert(!an_verify_frame(ctx, fn, slots, phi, outgoing, calls, frame));
    assert(ctx->diag->status == ARCHON_BACKEND_INTERNAL);
    assert(ctx->diag->stage == ARCHON_BACKEND_ALLOCATE);
    if (!strstr(ctx->diag->message, why))
        fprintf(stderr, "expected '%s', got '%s'\n", why, ctx->diag->message);
    assert(strstr(ctx->diag->message, why));
}

static void mixed_regions(ANContext *ctx, ArchonMIROpcode call_op)
{
    const ArchonMIRType types[13] = {
        I64, I64, I64, I64, I64, I64, I64, I64, I64,
        ARCHON_MIR_FUNC_PTR, I64, I64, I64
    };
    const ArchonValue call_args[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1};
    const ArchonMIRInst call = {
        call_op, NONE, call_op == ARCHON_MIR_CALL_INDIRECT ? 9 : NONE,
        NONE, 0, "callee", call_args, 11
    };
    const ArchonValue yes_args[] = {0}, no_args[] = {0, 1};
    const ArchonValue yes_params[] = {10}, no_params[] = {11, 12};
    const ArchonMIRBlock blocks[] = {
        { .insts = &call, .inst_count = 1,
          .term = {ARCHON_MIR_CBR, 0, {1, yes_args, 1}, {2, no_args, 2}} },
        { .params = yes_params, .param_count = 1,
          .term = { .kind = ARCHON_MIR_RETURN, .value = NONE } },
        { .params = no_params, .param_count = 2,
          .term = { .kind = ARCHON_MIR_RETURN, .value = NONE } },
    };
    const ArchonMIRLocal locals[] = { {3, 1}, {40, 16} };
    const ArchonMIRFunction fn = {
        "mixed", ARCHON_MIR_VOID, types, 13, 10, blocks, 3, locals, 2
    };
    ANFrame frame = {0}, bad;

    *ctx->diag = (ArchonBackendDiagnostic){0};
    assert(an_frame(ctx, &fn, 13, 2, 3, 1, &frame));
    assert(frame.frame_size == 208 && frame.slots_base == 24 && frame.phi_base == 128);
    assert(frame.local_offsets[0] == 144 && frame.local_offsets[1] == 160);
    assert(frame.return_offset == 200);
    assert(an_verify_frame(ctx, &fn, 13, 2, 3, 1, &frame));

#define BAD_FRAME(change, why) do { \
        bad = frame; bad.change; \
        reject(ctx, &fn, 13, 2, 3, 1, &bad, why); \
    } while (0)
    BAD_FRAME(frame_size = INT_MIN, "stack alignment");
    BAD_FRAME(frame_size += 8, "stack alignment");
    BAD_FRAME(frame_size -= 16, "local region");
    BAD_FRAME(slots_base -= 8, "slot region");
    BAD_FRAME(slots_base += 1, "slot region");
    BAD_FRAME(phi_base -= 8, "phi region");
    BAD_FRAME(phi_base = frame.frame_size, "phi region");
    BAD_FRAME(local_offsets[0] = frame.phi_base, "local region");
    BAD_FRAME(local_offsets[0] = -1, "local region");
    BAD_FRAME(local_offsets[1] -= 16, "local region");
    BAD_FRAME(local_offsets[1] += 8, "local region");
    BAD_FRAME(return_offset = -1, "saved return address");
    BAD_FRAME(return_offset -= 8, "saved return address");
    BAD_FRAME(return_offset += 4, "saved return address");
    BAD_FRAME(return_offset = frame.frame_size, "saved return address");
#undef BAD_FRAME

    reject(ctx, &fn, SIZE_MAX, 2, 3, 1, &frame, "slot count");
    reject(ctx, &fn, 12, 2, 3, 1, &frame, "slots for block parameters");
    reject(ctx, &fn, 13, SIZE_MAX, 3, 1, &frame, "phi region");
    reject(ctx, &fn, 13, 2, SIZE_MAX, 1, &frame, "outgoing region");

    /* These undercounts fit geometrically, but actual accesses need more. */
    reject(ctx, &fn, 13, 1, 3, 1, &frame, "phi words for edge");
    reject(ctx, &fn, 13, 2, 2, 1, &frame, "outgoing words for call");
    bad = frame;
    bad.return_offset = -1;
    reject(ctx, &fn, 13, 2, 3, 0, &bad, "saved return address for call");
}

static void exact_limits(ANContext *ctx)
{
    const ArchonMIRInst call = {ARCHON_MIR_CALL, NONE, NONE, NONE, 0, "callee", NULL, 0};
    ArchonMIRBlock block = { .term = { .kind = ARCHON_MIR_RETURN, .value = NONE } };
    const size_t param_counts[] = {0, 9, AN_MAX_PARAMS};
    ArchonMIRType types[AN_MAX_PARAMS];
    ArchonMIRLocal local = {0, 8};
    ArchonMIRFunction fn = {"large", ARCHON_MIR_VOID, types, 0, 0, &block, 1, &local, 1};
    ANFrame frame = {0};
    size_t i;

    for (i = 0; i < AN_MAX_PARAMS; ++i)
        types[i] = I64;
    /* An empty leaf takes no stack space, and -1 is its only valid RA sentinel. */
    *ctx->diag = (ArchonBackendDiagnostic){0};
    fn.local_count = 0;
    assert(an_frame(ctx, &fn, 0, 0, 0, 0, &frame));
    assert(frame.frame_size == 0 && frame.return_offset == -1);
    assert(an_verify_frame(ctx, &fn, 0, 0, 0, 0, &frame));
    frame.return_offset = -2;
    reject(ctx, &fn, 0, 0, 0, 0, &frame, "saved return address");
    frame.frame_size = 16;
    frame.return_offset = 8;
    reject(ctx, &fn, 0, 0, 0, 1, &frame, "saved return address in leaf");
    fn.local_count = 1;

    for (i = 0; i < sizeof param_counts / sizeof param_counts[0]; ++i) {
        size_t params = param_counts[i];
        size_t last_offset = params > 8 ? (params - 9) * 8 : 0;
        int limit = (INT_MAX - (int)last_offset) & ~15;
        int calls = params == AN_MAX_PARAMS;
        ANFrame bad;
        fn.value_count = fn.param_count = params;
        block.insts = calls ? &call : NULL;
        block.inst_count = (size_t)calls;
        local.size = (uint32_t)(limit - (int)params * 8 - calls * 8);
        *ctx->diag = (ArchonBackendDiagnostic){0};
        assert(an_frame(ctx, &fn, params, 0, 0, calls, &frame));
        assert(frame.frame_size == limit && frame.local_offsets[0] == (int)params * 8);
        assert(frame.return_offset == (calls ? limit - 8 : -1));
        assert(an_verify_frame(ctx, &fn, params, 0, 0, calls, &frame));
        if (last_offset >= 16) {
            /* The regions still fit, but the last incoming load no longer does. */
            bad = frame;
            bad.frame_size += 16;
            reject(ctx, &fn, params, 0, 0, calls, &bad, "incoming stack argument displacement");
        }
        ++local.size;
        *ctx->diag = (ArchonBackendDiagnostic){0};
        assert(!an_frame(ctx, &fn, params, 0, 0, calls, &frame));
        assert(ctx->diag->status == ARCHON_BACKEND_LIMIT);
    }
}

int main(void)
{
    ArchonBackendDiagnostic diag = {0};
    ANContext ctx = { .diag = &diag, .stage = ARCHON_BACKEND_ALLOCATE };
    mixed_regions(&ctx, ARCHON_MIR_CALL);
    mixed_regions(&ctx, ARCHON_MIR_CALL_INDIRECT);
    exact_limits(&ctx);
    return 0;
}
"""
