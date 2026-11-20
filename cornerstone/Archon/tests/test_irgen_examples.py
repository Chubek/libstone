"""Checkout-wrapper and versioned-artifact integration tests for IR generation."""

from __future__ import annotations

import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
WRAPPER = ROOT / "tools" / "archon-irgen"
ARCHONC = ROOT / "tools" / "archonc"
RV32I = ROOT / "stdspec" / "riscv" / "rv32i.arch"
EXAMPLE = ROOT / "examples" / "ir-pipeline"
FIXTURES = EXAMPLE / "fixtures" / "v1"
EXPECTED = EXAMPLE / "expected" / "v1"
BUILD_SCRIPT = EXAMPLE / "build.py"


def _has_irgen_command() -> bool:
    result = subprocess.run(
        [sys.executable, "-m", "archon", "--help"],
        cwd=ROOT,
        capture_output=True,
        check=False,
        text=True,
    )
    return result.returncode == 0 and re.search(
        r"(?:^|[,{]\s*)irgen(?:[,}\s]|$)", result.stdout
    ) is not None


IRGEN_AVAILABLE = _has_irgen_command()
CC = shlex.split(os.environ.get("CC", "cc"))
CC_AVAILABLE = bool(CC) and shutil.which(CC[0]) is not None


class IRGeneratorExamples(unittest.TestCase):
    maxDiff = None

    def run_checked(self, command, *, cwd, stdout=None):
        result = subprocess.run(
            [str(item) for item in command],
            cwd=cwd,
            stdout=stdout if stdout is not None else subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        self.assertEqual(
            result.returncode,
            0,
            result.stderr.decode("utf-8", "replace"),
        )
        return result

    def test_checkout_wrapper_and_versioned_fixtures_exist(self):
        self.assertTrue(WRAPPER.is_file())
        self.assertTrue(os.access(WRAPPER, os.X_OK))
        source = WRAPPER.read_text(encoding="utf-8")
        self.assertIn("Path(__file__).resolve().parents[1]", source)
        self.assertIn('main(["irgen", *sys.argv[1:]])', source)

        for representation in ("anf", "untyped-tac", "sea-of-nodes"):
            fixture = FIXTURES / representation / "Program.i"
            self.assertTrue(fixture.is_file(), fixture)
            self.assertIn("schema v1", fixture.read_text(encoding="utf-8"))
        self.assertNotEqual(
            (EXPECTED / "scheduler" / "MIR.i").read_bytes(),
            (EXPECTED / "selector" / "MIR.i").read_bytes(),
            "distinct MIR schemas must not be presented as interchangeable",
        )

    @unittest.skipUnless(
        IRGEN_AVAILABLE and CC_AVAILABLE,
        "the registered irgen command and a C99 compiler are required",
    )
    def test_program_fixtures_compile_as_strict_c99(self):
        from archon.irgen import anf, sea_of_nodes, untyped_tac

        cases = (
            (
                "anf",
                anf,
                ['-DARCHON_IR_PROGRAM_FILE="Program.i"'],
                """#include "IR.h"
int main(void) {
    struct archon_ir_program program;
    char diagnostic[256];
    enum archon_ir_status status =
        archon_ir_load_embedded(&program, diagnostic, sizeof diagnostic);
    if (status != ARCHON_IR_OK)
        return 1;
    if (program.function_count != 1u) {
        archon_ir_program_dispose(&program);
        return 2;
    }
    archon_ir_program_dispose(&program);
    return 0;
}
""",
            ),
            (
                "untyped-tac",
                untyped_tac,
                ['-DARCHON_IR_PROGRAM="Program.i"'],
                """#include "IR.h"
int main(void) {
    struct IR_program program;
    struct IR_diagnostic diagnostic;
    enum IR_status status = IR_load_program(&program, &diagnostic);
    if (status != IR_OK)
        return 1;
    if (program.instruction_count != 4u || program.temporary_count != 3u) {
        IR_program_destroy(&program);
        return 2;
    }
    IR_program_destroy(&program);
    return 0;
}
""",
            ),
            (
                "sea-of-nodes",
                sea_of_nodes,
                ["-DARCHON_IR_INCLUDE_PROGRAM"],
                """#include "IR.h"
int main(void) {
    ArchonIRGraph graph;
    ArchonIRNodeId root = ARCHON_IR_NODE_NONE;
    ArchonIRStatus status = archon_ir_graph_init(&graph, NULL);
    if (status != ARCHON_IR_OK)
        return 1;
    status = archon_ir_decode_program(&graph, &root);
    if (status != ARCHON_IR_OK || graph.node_count != 1u || root != 0u) {
        archon_ir_graph_destroy(&graph);
        return 2;
    }
    archon_ir_graph_destroy(&graph);
    return 0;
}
""",
            ),
        )
        with tempfile.TemporaryDirectory(prefix="archon-ir-fixtures-") as raw:
            temporary = Path(raw)
            for name, module, definitions, runner in cases:
                with self.subTest(representation=name):
                    directory = temporary / name
                    directory.mkdir()
                    for filename, text in module.emit_ir(None).items():
                        (directory / filename).write_text(
                            text, encoding="utf-8", newline="\n"
                        )
                    runner_path = directory / "fixture_runner.c"
                    runner_path.write_text(runner, encoding="utf-8", newline="\n")
                    executable = directory / "fixture-runner"
                    self.run_checked(
                        [
                            *CC,
                            "-std=c99",
                            "-pedantic-errors",
                            "-Wall",
                            "-Wextra",
                            "-Werror",
                            *definitions,
                            "-I",
                            directory,
                            "-I",
                            FIXTURES / name,
                            directory / "IR.c",
                            runner_path,
                            "-o",
                            executable,
                        ],
                        cwd=temporary,
                    )
                    self.run_checked([executable], cwd=temporary)

    @unittest.skipUnless(
        IRGEN_AVAILABLE and CC_AVAILABLE,
        "the registered irgen command and a C99 compiler are required",
    )
    def test_versioned_regallocation_is_a_strict_c99_xmacro_stream(self):
        from archon.irgen.macros import macro_abi_header

        with tempfile.TemporaryDirectory(prefix="archon-regallocated-") as raw:
            temporary = Path(raw)
            (temporary / "MacroABI.h").write_text(
                macro_abi_header(),
                encoding="utf-8",
                newline="\n",
            )
            source = temporary / "check.c"
            source.write_text(
                '#include "Regallocated.i"\n'
                "int main(void) { return 0; }\n",
                encoding="utf-8",
                newline="\n",
            )
            executable = temporary / "check"
            self.run_checked(
                [
                    *CC,
                    "-std=c99",
                    "-pedantic-errors",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    temporary,
                    "-I",
                    EXPECTED / "regalloc",
                    source,
                    "-o",
                    executable,
                ],
                cwd=temporary,
            )
            self.run_checked([executable], cwd=temporary)

    def test_example_manifest_cleanup_preserves_unrelated_files(self):
        with tempfile.TemporaryDirectory(prefix="archon cleanup with spaces ") as raw:
            directory = Path(raw)
            generated = directory / "made-by-example.txt"
            unrelated = directory / "keep-me.txt"
            generated.write_text("generated\n", encoding="utf-8")
            unrelated.write_text("user data\n", encoding="utf-8")
            (directory / ".archon-ir-pipeline-manifest.json").write_text(
                json.dumps({"schema": 1, "files": [generated.name]}),
                encoding="utf-8",
            )
            self.run_checked(
                [sys.executable, BUILD_SCRIPT, "clean", "--build-dir", directory],
                cwd=directory.parent,
            )
            self.assertFalse(generated.exists())
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "user data\n")
            refused = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_SCRIPT),
                    "build",
                    "--build-dir",
                    str(directory),
                ],
                cwd=directory.parent,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(refused.returncode, 0)
            self.assertNotIn(b"Traceback", refused.stderr)
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "user data\n")

            unsafe = directory / "unsafe manifest"
            unsafe.mkdir()
            (unsafe / ".archon-ir-pipeline-manifest.json").write_text(
                json.dumps({"schema": 1, "files": ["../keep-me.txt"]}),
                encoding="utf-8",
            )
            rejected = subprocess.run(
                [
                    sys.executable,
                    str(BUILD_SCRIPT),
                    "clean",
                    "--build-dir",
                    str(unsafe),
                ],
                cwd=directory,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertNotIn(b"Traceback", rejected.stderr)
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "user data\n")

    @unittest.skipUnless(
        IRGEN_AVAILABLE,
        "archon irgen is not registered yet; failures are required once it appears",
    )
    def test_wrapper_from_spaced_directory_is_atomic_and_deterministic(self):
        with tempfile.TemporaryDirectory(prefix="archon irgen wrapper ") as raw:
            directory = Path(raw)
            database = directory / "RV32 target.archdb"
            output = directory / "generated tree"
            self.run_checked(
                [ARCHONC, RV32I, "-o", database],
                cwd=directory,
            )
            command = [WRAPPER, "anf", database, "-o", output]
            self.run_checked(command, cwd=directory)
            first = {
                path.relative_to(output).as_posix(): (
                    path.read_bytes(),
                    path.stat().st_mtime_ns,
                )
                for path in output.rglob("*")
                if path.is_file()
            }
            self.assertIn("IR.c", first)
            self.assertIn("IR.h", first)

            self.run_checked(command, cwd=directory)
            second = {
                path.relative_to(output).as_posix(): (
                    path.read_bytes(),
                    path.stat().st_mtime_ns,
                )
                for path in output.rglob("*")
                if path.is_file()
            }
            self.assertEqual(first, second)

            unsafe = subprocess.run(
                [
                    str(WRAPPER),
                    "anf",
                    str(database),
                    "-o",
                    str(directory),
                    "--force",
                ],
                cwd=directory,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(unsafe.returncode, 0)
            self.assertNotIn(b"Traceback", unsafe.stderr)
            self.assertIn(b"refusing", unsafe.stderr)
            self.assertTrue(database.is_file())

    @unittest.skipUnless(
        IRGEN_AVAILABLE and CC_AVAILABLE,
        "the registered irgen command and a C99 compiler are required",
    )
    def test_rv32i_demo_and_generated_driver_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="archon test build with spaces ") as raw:
            build = Path(raw)
            self.run_checked(
                [sys.executable, BUILD_SCRIPT, "demo", "--build-dir", build],
                cwd=build.parent,
            )
            self.run_checked(
                [
                    sys.executable,
                    BUILD_SCRIPT,
                    "rebuild-check",
                    "--build-dir",
                    build,
                ],
                cwd=build.parent,
            )

            actual = build / "actual"
            listed = self.run_checked(
                [build / "pipeline-driver", "--list-artifacts"],
                cwd=actual,
            ).stdout.decode()
            self.assertIn("Program.i\n", listed)
            self.assertIn("MIR.i\n", listed)
            self.assertIn("InstrMap.i\n", listed)
            self.assertIn("RegMap.i\n", listed)
            self.assertIn("Regallocated.i\n", listed)
            self.assertIn("Program.s\n", listed)

            with tempfile.TemporaryDirectory(
                prefix="archon stop-after schedule "
            ) as stopped_raw:
                stopped = Path(stopped_raw)
                self.run_checked(
                    [build / "pipeline-driver", "--stop-after", "schedule"],
                    cwd=stopped,
                )
                self.assertTrue((stopped / "Rewritten.i").is_file())
                self.assertEqual(
                    (stopped / "MIR.i").read_bytes(),
                    (EXPECTED / "pipeline" / "MIR.i").read_bytes(),
                )
                self.assertFalse((stopped / "Selected.i").exists())
                self.assertFalse((stopped / "Regallocated.i").exists())

            with tempfile.TemporaryDirectory(
                prefix="archon default cleanup "
            ) as cleaned_raw:
                cleaned = Path(cleaned_raw)
                custom_output = cleaned / "custom output.s"
                self.run_checked(
                    [
                        build / "pipeline-driver",
                        "--dialect",
                        "gas",
                        "--output",
                        custom_output,
                    ],
                    cwd=cleaned,
                )
                self.assertTrue(custom_output.is_file())
                self.assertFalse((cleaned / "MIR.i").exists())
                self.assertFalse((cleaned / "Regallocated.i").exists())

            removable = actual / "Program.s"
            pipeline_input = actual / "Program.i"
            unrelated = actual / "unrelated.txt"
            removable.write_text("temporary output\n", encoding="utf-8")
            pipeline_input.write_text("must be preserved\n", encoding="utf-8")
            unrelated.write_text("must survive\n", encoding="utf-8")
            self.assertTrue((actual / "MIR.i").is_file())
            self.run_checked([build / "pipeline-driver", "--clean"], cwd=actual)
            self.assertFalse(removable.exists())
            self.assertFalse((actual / "MIR.i").exists())
            self.assertFalse((actual / "Regallocated.i").exists())
            self.assertEqual(
                pipeline_input.read_text(encoding="utf-8"),
                "must be preserved\n",
            )
            self.assertTrue((actual / "Program.gas.s").is_file())
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "must survive\n")

            self.run_checked(
                [sys.executable, BUILD_SCRIPT, "clean", "--build-dir", build],
                cwd=build.parent,
            )
            self.assertEqual(
                pipeline_input.read_text(encoding="utf-8"),
                "must be preserved\n",
            )
            self.assertEqual(unrelated.read_text(encoding="utf-8"), "must survive\n")


if __name__ == "__main__":
    unittest.main()
