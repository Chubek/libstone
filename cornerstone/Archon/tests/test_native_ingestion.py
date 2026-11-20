"""Exercise the native C API boundary with controlled lower-pipeline outcomes."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


NATIVE = Path(__file__).resolve().parents[1] / "archon" / "irgen" / "native"


class NativeIngestion(unittest.TestCase):
    def test_typed_api(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "TargetNative.h").write_text(
                '#define AN_TARGET_NAME "RISCV64IM/LP64"\n', encoding="ascii"
            )
            (root / "test.c").write_text(
                r"""
#include "NativeInternal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Test-only stand-ins isolate the API boundary from target-specific lowering. */
int an_verify(ANContext *ctx, const ArchonMIRModule *module)
{
    assert(ctx->stage == ARCHON_BACKEND_VERIFY);
    if (module->version != ARCHON_MIR_SCHEMA_VERSION) {
        ctx->function = 2;
        ctx->block = 3;
        ctx->instruction = 4;
        an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "first failure");
        ctx->instruction = 5;
        return an_error(ctx, ARCHON_BACKEND_INTERNAL, "later failure");
    }
    return 1;
}

int an_compile_module(ANContext *ctx, const ArchonMIRModule *module, ANOut *out)
{
    assert(module->version == ARCHON_MIR_SCHEMA_VERSION);
    assert(ctx->function == SIZE_MAX && ctx->block == SIZE_MAX &&
           ctx->instruction == SIZE_MAX);
    assert(out->physical == !!(ctx->flags & ARCHON_BACKEND_PHYSICAL_RECORDS));
    assert(out->machine_code == !!(ctx->flags & ARCHON_BACKEND_MACHINE_CODE));
    assert(out->pic == !!(ctx->flags & ARCHON_BACKEND_PIC));
    assert(out->limit == 1 || out->limit == 16u * 1024 * 1024 ||
           out->limit == 64u * 1024 * 1024);
    out->data = malloc(4);
    assert(out->data);
    out->capacity = 4;
    if (module->import_count)
        an_error(ctx, ARCHON_BACKEND_UNSUPPORTED, "prior lowering failure");
    if (module->global_count) {
        out->status = ARCHON_BACKEND_NOMEM;
        return 1;
    }
    if (out->limit == 1) {
        out->status = ARCHON_BACKEND_LIMIT;
        return 0;
    }
    memcpy(out->data, "asm", 4);
    out->size = 3;
    return 1;
}

static int reenter_and_cancel(void *user)
{
    unsigned *polls = user;
    const ArchonMIRModule nested = {.version = ARCHON_MIR_SCHEMA_VERSION};
    ArchonBackendArtifact artifact = {0};
    assert(archon_backend_compile(&nested, NULL, &artifact, NULL) == ARCHON_BACKEND_OK);
    archon_backend_artifact_destroy(&artifact);
    return ++*polls == 3;
}

int main(void)
{
    ArchonMIRModule module = {.version = ARCHON_MIR_SCHEMA_VERSION};
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    ArchonBackendOptions options = {0};
    unsigned polls = 0;
    char name[257];

    assert(strcmp(archon_backend_target(), "RISCV64IM/LP64") == 0);
    assert(archon_backend_compile(NULL, NULL, &artifact, &diag) == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_INPUT && diag.function == SIZE_MAX);
    assert(archon_backend_compile(&module, NULL, NULL, NULL) == ARCHON_BACKEND_INVALID);
    assert(!artifact.data && !artifact.size);

    module.version = 0;
    assert(archon_backend_compile(&module, NULL, &artifact, NULL) == ARCHON_BACKEND_UNSUPPORTED);
    assert(archon_backend_compile(&module, NULL, &artifact, &diag) == ARCHON_BACKEND_UNSUPPORTED);
    assert(diag.status == ARCHON_BACKEND_UNSUPPORTED && diag.stage == ARCHON_BACKEND_VERIFY);
    assert(diag.function == 2 && diag.block == 3 && diag.instruction == 4);
    assert(strcmp(diag.message, "first failure") == 0);
    assert(!artifact.data && !artifact.size);

    module.version = ARCHON_MIR_SCHEMA_VERSION;
    module.global_count = 1;
    assert(archon_backend_compile(&module, NULL, &artifact, NULL) == ARCHON_BACKEND_NOMEM);
    assert(!artifact.data && !artifact.size);
    module.import_count = 1;
    assert(archon_backend_compile(&module, NULL, &artifact, NULL) == ARCHON_BACKEND_UNSUPPORTED);
    assert(!artifact.data && !artifact.size);
    module.import_count = 0;
    module.global_count = 0;
    assert(archon_backend_compile_ex(&module, NULL,
        ARCHON_BACKEND_PIC | ARCHON_BACKEND_PHYSICAL_RECORDS,
        &artifact, &diag) == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_INPUT && strstr(diag.message, "assembly output"));
    assert(!artifact.data && !artifact.size);
    assert(archon_backend_compile_ex(&module, NULL,
        ARCHON_BACKEND_PIC | ARCHON_BACKEND_MACHINE_CODE,
        &artifact, NULL) == ARCHON_BACKEND_INVALID);
    assert(!artifact.data && !artifact.size);
    assert(archon_backend_compile_ex(&module, NULL, 16u,
        &artifact, NULL) == ARCHON_BACKEND_UNSUPPORTED);
    assert(!artifact.data && !artifact.size);
    assert(archon_backend_compile_ex(&module, NULL, ARCHON_BACKEND_PIC,
        &artifact, &diag) == ARCHON_BACKEND_OK);
    assert(artifact.size == 3 && strcmp(artifact.data, "asm") == 0);
    archon_backend_artifact_destroy(&artifact);

    options.max_output_bytes = 1;
    assert(archon_backend_compile(&module, &options, &artifact, &diag) == ARCHON_BACKEND_LIMIT);
    assert(diag.status == ARCHON_BACKEND_LIMIT && diag.stage == ARCHON_BACKEND_EMIT);
    assert(!artifact.data && !artifact.size);

    options.max_output_bytes = SIZE_MAX;
    options.cancelled = reenter_and_cancel;
    options.user = &polls;
    assert(archon_backend_compile(&module, &options, &artifact, &diag) == ARCHON_BACKEND_CANCELLED);
    assert(diag.status == ARCHON_BACKEND_CANCELLED && !artifact.data && !artifact.size);
    assert(polls == 3);

    options.cancelled = NULL;
    assert(archon_backend_compile(&module, &options, &artifact, &diag) == ARCHON_BACKEND_OK);
    assert(diag.status == ARCHON_BACKEND_OK && diag.message[0] == '\0');
    assert(diag.function == SIZE_MAX && diag.block == SIZE_MAX && diag.instruction == SIZE_MAX);
    assert(artifact.size == 3 && strcmp(artifact.data, "asm") == 0);
    assert(archon_backend_compile(&module, NULL, &artifact, NULL) == ARCHON_BACKEND_INVALID);
    assert(artifact.size == 3 && strcmp(artifact.data, "asm") == 0);
    archon_backend_artifact_destroy(&artifact);
    archon_backend_artifact_destroy(&artifact);
    archon_backend_artifact_destroy(NULL);
    assert(!artifact.data && !artifact.size);

    assert(an_symbol("_good_2"));
    assert(!an_symbol(NULL) && !an_symbol("") && !an_symbol("2bad"));
    assert(!an_symbol(".Lfoo") && !an_symbol("name\n") && !an_symbol("\377"));
    memset(name, 'a', sizeof name);
    name[256] = '\0';
    assert(!an_symbol(name));
    name[255] = '\0';
    assert(an_symbol(name));
    return 0;
}
""",
                encoding="ascii",
            )
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "-I", str(root), "-I", str(NATIVE), str(NATIVE / "Backend.c"),
                 "test.c", "-o", "test"],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "test")], check=True)
