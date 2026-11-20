"""Exercise the native verifier through the borrowed C API."""

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NONE ARCHON_MIR_NONE
#define I64 ARCHON_MIR_I64
#define PTR ARCHON_MIR_PTR
#define VOID ARCHON_MIR_VOID
#define VALID ARCHON_BACKEND_OK
#define INVALID ARCHON_BACKEND_INVALID
#define LIMIT ARCHON_BACKEND_LIMIT
#define UNSUPPORTED ARCHON_BACKEND_UNSUPPORTED

/* The verifier must reject bad inputs before reaching this lowering stub. */
const ANABI an_abi = { .stack_align = 16 };
static unsigned compilations;

int an_compile_module(ANContext *ctx, const ArchonMIRModule *module, ANOut *out)
{
    (void)ctx;
    (void)module;
    out->data = malloc(2);
    assert(out->data);
    memcpy(out->data, "x", 2);
    out->size = 1;
    out->capacity = 2;
    ++compilations;
    return 1;
}

static void check(const ArchonMIRModule *module, ArchonBackendStatus status,
    const char *message)
{
    ArchonBackendDiagnostic diag;
    ArchonBackendArtifact artifact = {0};
    unsigned before = compilations;
    ArchonBackendStatus actual = archon_backend_compile(module, NULL, &artifact, &diag);
    if (actual != status)
        fprintf(stderr, "expected %d (%s), got %d (%s)\n", (int)status, message,
                (int)actual, diag.message);
    assert(actual == status);
    assert(diag.status == status);
    assert(compilations == before + (status == VALID));
    if (status != VALID) {
        assert(!artifact.data && !artifact.size);
        assert(diag.stage == ARCHON_BACKEND_VERIFY);
        assert(strstr(diag.message, message) != NULL);
    } else
        assert(artifact.data && artifact.size == 1);
    archon_backend_artifact_destroy(&artifact);
}

static int cancel(void *user)
{
    unsigned *count = user;
    return ++*count == 6;
}

static void test_dominance(void)
{
    const ArchonMIRType types[] = {I64, I64};
    const ArchonMIRInst def = {ARCHON_MIR_CONST, 1, NONE, NONE, 1, NULL, NULL, 0};
    ArchonMIRBlock blocks[130] = {0};
    ArchonMIRFunction fn = {"dom", I64, types, 2, 1, blocks, 130, NULL, 0};
    const ArchonMIRModule module = {
        .version = ARCHON_MIR_SCHEMA_VERSION, .functions = &fn, .function_count = 1
    };
    size_t i;

    /* Entry follows decreasing block IDs; definition block crosses both
       word boundaries in the dominator bitset and dominates the last use. */
    for (i = 2; i < fn.block_count; ++i)
        blocks[i].term = (ArchonMIRTerm){
            .kind = ARCHON_MIR_BR, .value = NONE, .yes = { (uint32_t)i - 1, NULL, 0 }
        };
    blocks[0].term = (ArchonMIRTerm){
        .kind = ARCHON_MIR_BR, .value = NONE, .yes = {129, NULL, 0}
    };
    blocks[128].insts = &def;
    blocks[128].inst_count = 1;
    blocks[1].term = (ArchonMIRTerm){ .kind = ARCHON_MIR_RETURN, .value = 1 };
    check(&module, VALID, "");

    /* Keep every block reachable but introduce a second path around def. */
    blocks[129].term = (ArchonMIRTerm){
        .kind = ARCHON_MIR_CBR, .value = 0, .yes = {128, NULL, 0}, .no = {127, NULL, 0}
    };
    check(&module, INVALID, "dominate");
}

int main(void)
{
    ArchonMIRType types[] = {I64, PTR, I64, I64, I64, I64, PTR};
    ArchonMIRType log_types[] = {PTR, I64};
    ArchonMIRType other_types[] = {I64};
    ArchonMIRImport logger = {"logger", VOID, log_types, 2};
    ArchonMIRGlobal global = {"data", NULL, 1};
    ArchonValue initial[] = {2}, next[] = {4}, arg[] = {3}, log_args[] = {6, 5};
    ArchonValue counter[] = {3};
    ArchonMIRInst init[] = { {ARCHON_MIR_CONST, 2, NONE, NONE, 42, NULL, NULL, 0} };
    ArchonMIRInst step[] = { {ARCHON_MIR_ADD, 4, 3, 0, 0, NULL, NULL, 0} };
    ArchonMIRInst result[] = {
        {ARCHON_MIR_CALL, 5, NONE, NONE, 0, "other", arg, 1},
        {ARCHON_MIR_GLOBAL_ADDR, 6, NONE, NONE, 0, "data", NULL, 0},
        {ARCHON_MIR_CALL, NONE, NONE, NONE, 0, "logger", log_args, 2},
    };
    /* Layout order deliberately differs from CFG order: entry -> body (2).
       The body reaches exit (1), or feeds its own parameter on a back edge. */
    ArchonMIRBlock blocks[] = {
        {NULL, 0, init, 1, { .kind = ARCHON_MIR_BR, .value = NONE, .yes = {2, initial, 1} }},
        {NULL, 0, result, 3, { .kind = ARCHON_MIR_RETURN, .value = 5 }},
        {counter, 1, step, 1,
            { .kind = ARCHON_MIR_CBR, .value = 0, .yes = {2, next, 1}, .no = {1, NULL, 0} }},
    };
    const ArchonMIRBlock other[] = {
        { .term = { .kind = ARCHON_MIR_RETURN, .value = 0 } }
    };
    ArchonMIRFunction fn[] = {
        {"run", I64, types, 7, 2, blocks, 3, NULL, 0},
        {"other", I64, other_types, 1, 1, other, 1, NULL, 0},
    };
    ArchonMIRModule module = {
        ARCHON_MIR_SCHEMA_VERSION, fn, 2, &logger, 1, &global, 1
    };
    const ArchonMIRModule initial_module = module;
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    ArchonBackendOptions options = { .cancelled = cancel };
    unsigned polls = 0;

    check(&module, VALID, "");
    assert(module.functions == initial_module.functions && module.imports == initial_module.imports);
    options.user = &polls;
    assert(archon_backend_compile(&module, &options, &artifact, &diag) == ARCHON_BACKEND_CANCELLED);
    assert(diag.stage == ARCHON_BACKEND_VERIFY && !artifact.data && !artifact.size);

    module.version = 99;
    check(&module, UNSUPPORTED, "version");
    module = initial_module;
    module.function_count = SIZE_MAX;
    check(&module, LIMIT, "function table");
    module = initial_module;
    module.globals = NULL;
    check(&module, INVALID, "global table");
    module = initial_module;
    global.word_count = SIZE_MAX;
    check(&module, LIMIT, "global word limit");
    global.word_count = 0;
    check(&module, VALID, "");
    global.name = "logger";
    check(&module, INVALID, "duplicate");
    global.name = "bad name";
    check(&module, INVALID, "symbol name");
    global.name = "data";

    fn[0].value_count = SIZE_MAX;
    check(&module, LIMIT, "value table");
    fn[0].value_count = 7;
    fn[0].types = NULL;
    check(&module, INVALID, "value table");
    fn[0].types = types;
    types[1] = VOID;
    check(&module, INVALID, "value type");
    types[1] = PTR;
    fn[0].result = (ArchonMIRType)99;
    check(&module, INVALID, "function declaration");
    fn[0].result = I64;
    blocks[0].inst_count = SIZE_MAX;
    check(&module, LIMIT, "instruction table");
    blocks[0].inst_count = 1;

    blocks[0].param_count = 1;
    blocks[0].params = counter;
    check(&module, INVALID, "entry block has block parameters");
    blocks[0].param_count = 0;
    blocks[0].params = NULL;
    blocks[2].term.yes.target = 0;
    check(&module, INVALID, "edge to entry");
    blocks[2].term.yes.target = 2;
    blocks[2].term.no.target = 2;
    check(&module, INVALID, "unreachable");
    blocks[2].term.no.target = 1;

    blocks[0].term.yes.arg_count = 0;
    check(&module, INVALID, "edge argument count");
    blocks[0].term.yes.arg_count = 1;
    blocks[0].term.yes.args = NULL;
    check(&module, INVALID, "edge argument table");
    blocks[0].term.yes.args = initial;
    initial[0] = 1;
    check(&module, INVALID, "edge argument type");
    initial[0] = 4;
    check(&module, INVALID, "dominate");
    initial[0] = 2;
    blocks[2].term.value = 1;
    check(&module, INVALID, "branch condition");
    blocks[2].term.value = 0;
    blocks[2].term.kind = (ArchonMIRTermKind)99;
    check(&module, INVALID, "unknown terminator");
    blocks[2].term.kind = ARCHON_MIR_CBR;
    blocks[0].term.value = 0;
    check(&module, INVALID, "unexpected terminator value");
    blocks[0].term.value = NONE;
    blocks[0].term.no.args = initial;
    check(&module, INVALID, "inactive edge arguments");
    blocks[0].term.no.args = NULL;

    step[0].a = 4;
    check(&module, INVALID, "dominate");
    step[0].a = 3;
    step[0].dst = 3;
    check(&module, INVALID, "duplicate SSA");
    step[0].dst = 4;
    init[0].a = NONE - 1u;
    check(&module, INVALID, "unexpected fixed value operand");
    init[0].a = NONE;
    init[0].op = (ArchonMIROpcode)99;
    check(&module, UNSUPPORTED, "unknown instruction");
    init[0].op = ARCHON_MIR_CONST;
    blocks[0].inst_count = 0;
    check(&module, INVALID, "no definition");
    blocks[0].inst_count = 1;

    result[0].symbol = "missing";
    check(&module, INVALID, "declared function or import");
    result[0].symbol = "data";
    check(&module, INVALID, "declared function or import");
    result[0].symbol = "other";
    result[0].arg_count = 0;
    check(&module, INVALID, "call argument count");
    result[0].arg_count = 1;
    result[0].dst = NONE;
    check(&module, INVALID, "call result");
    result[0].dst = 5;
    result[1].symbol = "other";
    check(&module, INVALID, "declared global");
    result[1].symbol = "data";
    log_args[0] = 0;
    check(&module, INVALID, "call argument type");
    log_args[0] = 6;
    logger.result = I64;
    check(&module, INVALID, "call result");
    logger.result = VOID;
    blocks[1].term.value = 1;
    check(&module, INVALID, "return type");
    test_dominance();
    return 0;
}
"""


class NativeVerifyTests(unittest.TestCase):
    def test_verification_contract(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler is unavailable")
        with tempfile.TemporaryDirectory(prefix="archon-native-verify-") as directory:
            root = Path(directory)
            (root / "TargetNative.h").write_text(
                '#define AN_TARGET_NAME "RISCV64IM/LP64"\n', encoding="ascii"
            )
            (root / "client.c").write_text(CLIENT, encoding="ascii")
            subprocess.run(
                [
                    *cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-I", str(NATIVE), "-I", str(root),
                    str(NATIVE / "Backend.c"), str(NATIVE / "NativeVerify.c"),
                    str(NATIVE / "Legalizer.c"), "client.c", "-o", "verify",
                ],
                cwd=root, check=True, timeout=30,
            )
            subprocess.run([str(root / "verify")], check=True, timeout=5)
