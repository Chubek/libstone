"""Host-side unit coverage for native CFG lowering."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


NATIVE = Path(__file__).resolve().parents[1] / "archon" / "irgen" / "native"
TMP_ROOT = Path("/tmp/opencode")

CLIENT = r"""#include "NativeInternal.h"
#include <assert.h>

const ANABI an_abi = { .result = AN_A0, .stack_pointer = AN_SP };

static uint64_t values[] = { 40, 42, UINT64_MAX, 11, 22 };
static uint64_t regs[32], phi[2];
static unsigned labels, jumps, branch_target, result_reads, returns, traps, halt;
static uint64_t jump_values[2][2];

void an_read(ANOut *o, const ANFrame *f, const ANHome *h, ArchonValue v, unsigned dst)
{
    (void)o; (void)f; (void)h;
    assert(dst == AN_T0 || dst == an_abi.result);
    if (dst == an_abi.result)
        ++result_reads;
    regs[dst] = values[v];
}

void an_write(ANOut *o, const ANFrame *f, const ANHome *h, ArchonValue v, unsigned src)
{
    (void)o; (void)f; (void)h;
    values[v] = regs[src];
}

void an_load(ANOut *o, unsigned dst, unsigned base, int offset)
{
    (void)o;
    assert(base == an_abi.stack_pointer && (offset == 2048 || offset == 2056));
    regs[dst] = phi[(offset - 2048) / 8];
}

void an_store(ANOut *o, unsigned src, unsigned base, int offset)
{
    (void)o;
    assert(base == an_abi.stack_pointer && (offset == 2048 || offset == 2056));
    phi[(offset - 2048) / 8] = regs[src];
}

void an_jump(ANOut *o, size_t fn, unsigned target)
{
    (void)o;
    if (traps) {
        assert(fn == 7 && halt == 4 && target == halt);
        return;
    }
    assert(fn == 7 && target == 1 && jumps < 2);
    jump_values[jumps][0] = values[0];
    jump_values[jumps++][1] = values[1];
}

void an_branch(ANOut *o, int ne, unsigned lhs, unsigned rhs, size_t fn, unsigned target)
{
    (void)o;
    assert(ne && regs[lhs] == UINT64_MAX && rhs == AN_ZERO && fn == 7);
    assert(jumps == 0 && target == 2 + labels);
    branch_target = target;
}

void an_label(ANOut *o, size_t fn, unsigned label)
{
    (void)o;
    if (traps) {
        halt = label;
        return;
    }
    assert(fn == 7 && label == branch_target && jumps == 1);
    ++labels;
}

void an_leave(ANOut *o, const ANFrame *f)
{
    (void)o; (void)f;
    assert(regs[an_abi.result] == UINT64_MAX && result_reads == 1);
    ++returns;
}

void an_emit(ANOut *o, ANOpcode op, unsigned a, unsigned b, unsigned c, int imm)
{
    (void)o; (void)a; (void)b; (void)c; (void)imm;
    assert(op == AN_EBREAK);
    ++traps;
}

int main(void)
{
    const ArchonValue params[] = { 0, 1 }, swap[] = { 1, 0 };
    const ArchonValue yes[] = { 3, 4 }, no[] = { 4, 3 };
    const ArchonMIRBlock blocks[] = { {0}, { .params = params, .param_count = 2 } };
    const ArchonMIRFunction fn = { .blocks = blocks, .block_count = 2 };
    const ANFrame frame = { .phi_base = 2048 };
    const ANHome homes[5] = {{0}};
    ANContext ctx = { .function = 7 };
    ANOut out = {0};
    unsigned next_label = 2, i;
    ArchonMIRTerm term = { .kind = ARCHON_MIR_BR, .value = ARCHON_MIR_NONE,
        .yes = { 1, swap, 2 } };
    an_terminator(&ctx, &out, &fn, &frame, homes, &term, &next_label);
    assert(jumps == 1 && jump_values[0][0] == 42 && jump_values[0][1] == 40);
    assert(next_label == 2);

    term = (ArchonMIRTerm){ ARCHON_MIR_CBR, 2, { 1, yes, 2 }, { 1, no, 2 } };
    for (i = 0; i < 2; ++i) {
        jumps = 0;
        an_terminator(&ctx, &out, &fn, &frame, homes, &term, &next_label);
        assert(jumps == 2 && jump_values[0][0] == 22 && jump_values[0][1] == 11);
        assert(jump_values[1][0] == 11 && jump_values[1][1] == 22);
    }
    assert(labels == 2 && next_label == 4);

    term = (ArchonMIRTerm){ .kind = ARCHON_MIR_RETURN, .value = 2 };
    an_terminator(&ctx, &out, &fn, &frame, homes, &term, &next_label);
    assert(returns == 1);
    term.value = ARCHON_MIR_NONE;
    an_terminator(&ctx, &out, &fn, &frame, homes, &term, &next_label);
    assert(returns == 2);

    term = (ArchonMIRTerm){ .kind = ARCHON_MIR_TRAP, .value = ARCHON_MIR_NONE };
    an_terminator(&ctx, &out, &fn, &frame, homes, &term, &next_label);
    assert(traps == 1 && returns == 2 && halt == 4 && next_label == 5 &&
        out.status == ARCHON_BACKEND_OK);
    return 0;
}
"""


class NativeCFGTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "a host C compiler is required")
    def test_parallel_edges_and_terminators(self):
        with tempfile.TemporaryDirectory(
            prefix="archon-cfg-", dir=TMP_ROOT if TMP_ROOT.is_dir() else None
        ) as raw:
            directory = Path(raw)
            client = directory / "cfg.c"
            client.write_text(CLIENT, encoding="utf-8")
            executable = directory / "cfg"
            result = subprocess.run(
                [
                    "cc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", NATIVE, client, NATIVE / "CFGLowering.c", "-o", executable,
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([executable], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
