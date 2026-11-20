"""Strict-C11 compile-and-run coverage for the generated embedding facade."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
TMP_ROOT = Path("/tmp/opencode")

PROGRAM = """\
IR_PROGRAM_BEGIN("embedding-test");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(left, "left", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(sum, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_INT(IR_TYPE_I32, 5);
IR_LET_END();
IR_RETURN(sum);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""

_ORACLE_DECLARATIONS = """\
    ArchonOracleOptions oracle;
    char long_socket[ARCHON_ORACLE_MAX_SOCKET_PATH + 2u];
"""

_ORACLE_INITIALIZE = """\
    archon_oracle_options_init(&oracle);
    options.oracle = &oracle;
"""

_ORACLE_INVALID_TEST = """\
    archon_embed_options_init(&options);
    archon_oracle_options_init(&oracle);
    memset(long_socket, 's', sizeof long_socket - 1u);
    long_socket[sizeof long_socket - 1u] = '\\0';
    oracle.socket_path = long_socket;
    options.oracle = &oracle;
    options.stop_after = ARCHON_EMBED_STAGE_REWRITE;
    if (archon_embed_run(&options, &result) !=
            ARCHON_EMBED_STATUS_PIPELINE_ERROR ||
            !result.stage_valid ||
            result.stage != ARCHON_EMBED_STAGE_REWRITE ||
            strstr(result.diagnostic, "socket path") == NULL ||
            !bounded(&result))
        return 17;
"""

CLIENT = r"""#include "ArchonEmbed.h"

#include <stdio.h>
#include <string.h>

static int exists(const char *path) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return 0;
    return fclose(input) == 0;
}

static int contains(const char *path, const char *text) {
    FILE *input = fopen(path, "rb");
    char line[1024];
    int found = 0;
    if (input == NULL)
        return 0;
    while (fgets(line, sizeof line, input) != NULL)
        if (strstr(line, text) != NULL) {
            found = 1;
            break;
        }
    if (fclose(input) != 0)
        return 0;
    return found;
}

static int bounded(const ArchonEmbedResult *result) {
    return result != NULL &&
        result->diagnostic[ARCHON_EMBED_MAX_DIAGNOSTIC_BYTES] == '\0' &&
        strlen(result->diagnostic) <= ARCHON_EMBED_MAX_DIAGNOSTIC_BYTES;
}

int main(void) {
    ArchonEmbedOptions options;
    ArchonEmbedResult result;
    char long_output[2048];
@ORACLE_DECLARATIONS@

    archon_embed_options_init(&options);
    if (options.stop_after != ARCHON_EMBED_STAGE_EMIT ||
            options.rewrite != ARCHON_EMBED_REWRITE_CONVENTIONAL ||
            options.dialect != ARCHON_EMBED_DIALECT_GAS ||
            options.artifact_policy != ARCHON_EMBED_ARTIFACTS_DISCARD)
        return 1;
@ORACLE_INITIALIZE@
    options.stop_after = ARCHON_EMBED_STAGE_SCHEDULE;
    options.rewrite = ARCHON_EMBED_REWRITE_EGRAPH;
    options.artifact_policy = ARCHON_EMBED_ARTIFACTS_RETAIN;
    if (archon_embed_run(&options, &result) != ARCHON_EMBED_STATUS_OK ||
            result.pipeline_status != ARCHON_PIPELINE_STATUS_OK ||
            !result.stage_valid ||
            result.stage != ARCHON_EMBED_STAGE_SCHEDULE ||
            result.output_emitted ||
            !result.artifacts_retained ||
            !exists("Rewritten.i") || !exists("MIR.i") ||
            exists("Selected.i") ||
            !contains("Rewritten.i", "\"egraph\"") ||
            !bounded(&result))
        return 2;

    if (archon_embed_cleanup_intermediates(&result) !=
            ARCHON_EMBED_STATUS_OK ||
            exists("Rewritten.i") || exists("MIR.i") ||
            !bounded(&result))
        return 3;

    archon_embed_options_init(&options);
@ORACLE_INITIALIZE@
    options.rewrite = ARCHON_EMBED_REWRITE_EGRAPH;
    options.dialect = ARCHON_EMBED_DIALECT_SANDSTONE;
    options.output_path = "embedded.asm";
    if (archon_embed_run(&options, &result) != ARCHON_EMBED_STATUS_OK ||
            !result.output_emitted || result.artifacts_retained ||
            !exists("embedded.asm") || exists("Regallocated.i") ||
            !contains("embedded.asm", "local targets = lsandas.symas.targets") ||
            result.diagnostic[0] != '\0' || !bounded(&result))
        return 4;

    archon_embed_options_init(&options);
    options.stop_after = (ArchonEmbedStage)99;
    if (archon_embed_run(&options, &result) !=
            ARCHON_EMBED_STATUS_INVALID_ARGUMENT ||
            result.pipeline_status !=
                ARCHON_PIPELINE_STATUS_INVALID_ARGUMENT ||
            result.stage_valid ||
            strstr(result.diagnostic, "stopping stage") == NULL ||
            !bounded(&result))
        return 5;

    archon_embed_options_init(&options);
@ORACLE_INITIALIZE@
    memset(long_output, 'x', sizeof long_output - 1u);
    long_output[sizeof long_output - 1u] = '\0';
    options.output_path = long_output;
    if (archon_embed_run(&options, &result) !=
            ARCHON_EMBED_STATUS_PIPELINE_ERROR ||
            result.pipeline_status != ARCHON_PIPELINE_STATUS_STAGE_FAILED ||
            !result.stage_valid ||
            result.stage != ARCHON_EMBED_STAGE_EMIT ||
            result.emitter_status != EMITTER_STATUS_IO_ERROR ||
            result.output_emitted || !bounded(&result))
        return 6;

@ORACLE_INVALID_TEST@
    return 0;
}
"""


class GeneratedEmbeddingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.database = compile_file(RV32)
        cls.compilers = [
            compiler
            for compiler in ("gcc", "clang")
            if shutil.which(compiler)
        ]
        if not cls.compilers:
            raise unittest.SkipTest("GCC or Clang is required")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)

    @staticmethod
    def write_files(directory: Path, files: dict[str, str]) -> None:
        for name, text in files.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="ascii", newline="\n")

    @staticmethod
    def client(oracle_enabled: bool) -> str:
        return (
            CLIENT.replace(
                "@ORACLE_DECLARATIONS@",
                _ORACLE_DECLARATIONS if oracle_enabled else "",
            )
            .replace(
                "@ORACLE_INITIALIZE@",
                _ORACLE_INITIALIZE if oracle_enabled else "",
            )
            .replace(
                "@ORACLE_INVALID_TEST@",
                _ORACLE_INVALID_TEST if oracle_enabled else "",
            )
        )

    def test_generated_headers_expand_oracle_surface_conditionally(self):
        disabled = generate_ir(
            "anf", [self.database], options={"oracle": False}
        )
        enabled = generate_ir(
            "anf", [self.database], options={"oracle": True}
        )
        self.assertTrue({"ArchonEmbed.h", "ArchonEmbed.c"} <= disabled.keys())
        self.assertNotIn("Oracle.c", disabled)
        self.assertNotIn("Oracle.h", disabled)
        for name, text in disabled.items():
            self.assertNotIn("@ORACLE_", text, name)
            self.assertNotIn('"Oracle.h"', text, name)
            self.assertNotIn("ArchonOracle", text, name)
            self.assertNotIn("archon_oracle_", text, name)
        self.assertNotIn("ArchonOracle", disabled["Emitter.h"])
        self.assertNotIn("ArchonOracle", disabled["ArchonEmbed.h"])

        self.assertIn('#include "Oracle.h"', enabled["Emitter.h"])
        self.assertIn(
            "const ArchonOracleOptions *oracle;", enabled["Emitter.h"]
        )
        self.assertIn(
            "const ArchonOracleOptions *oracle;", enabled["ArchonEmbed.h"]
        )
        self.assertNotIn("@ORACLE_OPTION_FIELD@", enabled["Emitter.h"])
        self.assertNotIn("getenv(", enabled["ArchonEmbed.c"])
        self.assertNotIn("getenv(", enabled["Emitter.c"])
        self.assertNotIn("Driver", enabled["ArchonEmbed.c"])

    def test_c11_embedding_client_runs_without_driver_for_both_variants(self):
        for oracle_enabled in (False, True):
            generated = generate_ir(
                "anf",
                [self.database],
                options={"oracle": oracle_enabled},
            )
            with tempfile.TemporaryDirectory(
                dir=TMP_ROOT,
                prefix=(
                    "archon-embed-oracle-"
                    if oracle_enabled
                    else "archon-embed-no-oracle-"
                ),
            ) as raw:
                directory = Path(raw)
                self.write_files(directory, generated)
                (directory / "Program.i").write_text(
                    PROGRAM, encoding="ascii", newline="\n"
                )
                (directory / "embed-client.c").write_text(
                    self.client(oracle_enabled),
                    encoding="ascii",
                    newline="\n",
                )
                # The facade must have no unresolved dependency on Driver.c.
                (directory / "Driver.c").unlink()
                sources = ["IR.c", "Graphs.c"]
                if oracle_enabled:
                    sources.append("Oracle.c")
                sources.extend(
                    (
                        "Rewrite.c",
                        "ISel.c",
                        "Pipeline.c",
                        "ISched.c",
                        "Regalloc.c",
                        "Emitter.c",
                        "ArchonEmbed.c",
                        "embed-client.c",
                    )
                )
                for compiler in self.compilers:
                    with self.subTest(
                        oracle=oracle_enabled, compiler=compiler
                    ):
                        executable = directory / f"embed-{compiler}"
                        subprocess.run(
                            [
                                compiler,
                                "-std=c11",
                                "-pedantic-errors",
                                "-Wall",
                                "-Wextra",
                                "-Werror",
                                '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                                "-DARCHON_ISEL_NO_MAIN",
                                "-DARCHON_REGALLOC_NO_MAIN",
                                "-I.",
                                *sources,
                                "-o",
                                executable.name,
                            ],
                            cwd=directory,
                            check=True,
                            capture_output=True,
                            text=True,
                        )
                        environment = os.environ.copy()
                        # A hostile convenience variable must not configure the
                        # embedding API; only options.oracle may do so.
                        environment["ARCHON_ORACLE_SOCKET"] = str(
                            directory / "must-not-be-used.sock"
                        )
                        completed = subprocess.run(
                            [str(executable)],
                            cwd=directory,
                            env=environment,
                            check=False,
                            capture_output=True,
                            text=True,
                            timeout=30,
                        )
                        self.assertEqual(
                            completed.returncode,
                            0,
                            completed.stdout + completed.stderr,
                        )
                        self.assertEqual(completed.stdout, "")
                        self.assertEqual(completed.stderr, "")


if __name__ == "__main__":
    unittest.main()
