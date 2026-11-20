"""Test the RV64 argument moves independently of the assembler."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from native_test_support import write_native_abi

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "archon" / "irgen" / "native"


class NativeABITests(unittest.TestCase):
    def test_lp64_word_locations(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None) as root:
            tmp_path = Path(root)
            abi = write_native_abi(tmp_path)
            subprocess.run(
                [
                    *cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(NATIVE), str(NATIVE / "ABI.c"), str(abi), "-x", "c", "-",
                    "-o", "test",
                ],
                input=HARNESS, text=True, cwd=tmp_path, check=True, timeout=30,
            )
            subprocess.run([str(tmp_path / "test")], check=True, timeout=30)


HARNESS = r"""
#include "NativeInternal.h"
#include <assert.h>
#include <string.h>

static unsigned step, argc;
static uint64_t reg[32], incoming[64], outgoing[64], home[64];

void an_load(ANOut *o, unsigned dst, unsigned base, int disp)
{
    (void)o;
    assert(step >= 8 && step < argc);
    assert(base == 2 && disp == 4096 + (int)(step - 8) * 8);
    reg[dst] = incoming[step];
}

void an_store(ANOut *o, unsigned src, unsigned base, int disp)
{
    (void)o;
    assert(argc > 8 && step < argc - 8);
    assert(base == 2 && disp == (int)step * 8);
    outgoing[step + 8] = reg[src];
    ++step;
}

void an_write(ANOut *o, const ANFrame *f, const ANHome *h,
    ArchonValue v, unsigned src)
{
    (void)o; (void)f; (void)h;
    assert(v == step);
    home[v] = reg[src];
    ++step;
}

void an_read(ANOut *o, const ANFrame *f, const ANHome *h,
    ArchonValue v, unsigned dst)
{
    (void)o; (void)f; (void)h;
    if (argc > 8 && step < argc - 8) {
        assert(dst >= AN_T0 && dst <= AN_T2);
    } else {
        unsigned i = step - (argc > 8 ? argc - 8 : 0);
        assert(dst == 10 + i && i < argc);
        ++step;
    }
    reg[dst] = home[v];
}

void an_call_symbol(ANOut *o, const char *symbol)
{
    unsigned i;
    (void)o;
    assert(strcmp(symbol, "target") == 0);
    assert(step == argc);
    for (i = 0; i < argc; ++i)
        assert((i < 8 ? reg[10 + i] : outgoing[i]) == home[argc - i - 1]);
    reg[10] = UINT64_MAX;
}

void an_emit(ANOut *o, ANOpcode opcode, unsigned rd, unsigned rs1, unsigned rs2, int imm)
{
    (void)o; (void)opcode; (void)rd; (void)rs1; (void)rs2; (void)imm;
    assert(0 && "the direct-call harness must not emit an indirect call");
}

int main(void)
{
    ANOut out = {0};
    ANFrame frame = {.frame_size = 4096};
    ANHome homes[64] = {{0}};
    ArchonMIRFunction fn = {0};
    ArchonValue args[64];
    ArchonMIRInst call = {.op = ARCHON_MIR_CALL, .symbol = "target", .args = args};
    unsigned i;

    assert(an_abi.stack_align == 16 && an_abi.return_address == 1);
    for (argc = 0; argc <= 64; ++argc) {
        assert(an_outgoing_words(argc) == (argc > 8 ? argc - 8 : 0));
        fn.param_count = argc;
        for (i = 0; i < argc; ++i) {
            incoming[i] = (UINT64_C(1) << 63) | i;
            args[i] = argc - i - 1;
            if (i < 8)
                reg[10 + i] = incoming[i];
        }
        step = 0;
        an_parameters(&out, &fn, &frame, homes);
        assert(step == argc);
        for (i = 0; i < argc; ++i)
            assert(home[i] == incoming[i]);

        call.arg_count = argc;
        call.dst = ARCHON_MIR_NONE;
        step = 0;
        an_call(&out, &call, &frame, homes);
        assert(step == argc);
    }
    argc = 10;
    call.arg_count = argc;
    call.dst = argc;
    for (i = 0; i < argc; ++i)
        args[i] = argc - i - 1;
    step = 0;
    an_call(&out, &call, &frame, homes);
    assert(step == argc + 1 && home[argc] == UINT64_MAX);
    return 0;
}
"""
