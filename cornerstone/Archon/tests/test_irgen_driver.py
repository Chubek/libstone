"""Strict-C99 integration coverage for supercombinators and final emission."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import (
    anf,
    emitter,
    graphs,
    isched,
    isel,
    oracle,
    pipeline,
    regalloc,
    rewrite,
    scheduler,
)
from archon.irgen import supercombinators


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
WASM = ROOT / "stdspec" / "wasm" / "root.arch"

PROGRAM = """\
IR_PROGRAM_BEGIN("pipeline-test");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(left, "left", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(sum, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_INT(IR_TYPE_I32, 5);
IR_LET_END();
IR_LET_BEGIN(twice, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(sum);
IR_ARG_VALUE(sum);
IR_LET_END();
IR_RETURN(twice);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""

SUPERCOMBINATORS = """\
SC_PROGRAM_BEGIN();
SC_LITERAL(0, 2);
SC_LITERAL(1, 3);
SC_INSTRUCTION_2(2, "add", 0, 1);
SC_FUNCTION(3, "lifted", 1, 1, 2);
SC_CLOSURE_1(4, 3, 0);
SC_APPLY_1(5, 4, 1);
SC_CASE_BEGIN(6, 0);
SC_CASE_ARM(2, 5);
SC_CASE_DEFAULT(2);
SC_CASE_END();
SC_ENTRY(6);
SC_PROGRAM_END();
"""


class IRGenDriver(unittest.TestCase):
    def compiler(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")
        return command

    def compile(self, root, output, *sources, defines=()):
        subprocess.run(
            [
                *self.compiler(),
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                *(f"-D{define}" for define in defines),
                *sources,
                "-I.",
                "-o",
                output,
            ],
            cwd=root,
            check=True,
        )

    @staticmethod
    def write_files(root, files):
        for name, text in files.items():
            (root / name).write_text(text, encoding="ascii")

    def test_supercombinator_builder_decoder_verifier_and_printer(self):
        self.assertEqual(supercombinators.NAME, "supercombinators")
        self.assertIn("supercomb", supercombinators.ALIASES)
        generated = supercombinators.emit_ir(None)
        self.assertEqual(set(generated), {"IR.c", "IR.h"})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, generated)
            (root / "Program.i").write_text(
                SUPERCOMBINATORS, encoding="ascii"
            )
            (root / "main.c").write_text(
                """\
#include "IR.h"
#include <assert.h>
#include <string.h>

int main(void) {
    IR_Program program;
    IR_Status status;
    FILE *output;
    char text[1024];
    size_t size;
    IR_program_init(&program);
    status = IR_decode_program(&program);
    assert(status == IR_STATUS_OK);
    assert(program.count == 7u);
    assert(program.nodes[3].kind == IR_KIND_FUNCTION);
    assert(program.nodes[4].kind == IR_KIND_CLOSURE);
    assert(program.nodes[6].kind == IR_KIND_CASE);
    output = tmpfile();
    assert(output != NULL);
    assert(IR_print_program(&program, output) == IR_STATUS_OK);
    assert(fseek(output, 0, SEEK_END) == 0);
    size = (size_t)ftell(output);
    assert(size < sizeof text);
    rewind(output);
    assert(fread(text, 1u, size, output) == size);
    text[size] = '\\0';
    assert(strstr(text, "function \\"lifted\\"") != NULL);
    assert(strstr(text, "closure %3 [%0]") != NULL);
    assert(strstr(text, "case %0") != NULL);
    assert(fclose(output) == 0);
    IR_program_clear(&program);
    return 0;
}
""",
                encoding="ascii",
            )
            self.compile(
                root,
                "test",
                "IR.c",
                "main.c",
                defines=('ARCHON_IR_PROGRAM_FILE="Program.i"',),
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)
            (root / "standalone.c").write_text(
                """\
#include "IR.h"
#include <assert.h>
#include <string.h>

int main(void) {
    IR_Program program;
    IR_program_init(&program);
    assert(IR_decode_program(&program) == IR_STATUS_BAD_ARTIFACT);
    assert(strstr(IR_program_diagnostic(&program), "not compiled") != NULL);
    IR_program_clear(&program);
    return 0;
}
""",
                encoding="ascii",
            )
            self.compile(root, "standalone", "IR.c", "standalone.c")
            subprocess.run([str(root / "standalone")], cwd=root, check=True)

    def pipeline_files(self, database):
        context = {
            "databases": [database],
            "db": database,
            "ir": "anf",
            "options": {"oracle": False},
        }
        generated = {}
        for files in (
            anf.emit_ir(None),
            graphs.emit(context),
            rewrite.emit(context),
            isel.emit(context),
            scheduler.emit(context),
            isched.emit(context),
            regalloc.emit(context),
            pipeline.emit(context),
            emitter.emit(context),
        ):
            generated.update(files)
        return generated

    def test_full_riscv_pipeline_stage_stopping_and_cleanup(self):
        database = compile_file(RV32)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, self.pipeline_files(database))
            (root / "Program.i").write_text(PROGRAM, encoding="ascii")
            self.compile(
                root,
                "driver",
                "IR.c",
                "Graphs.c",
                "Rewrite.c",
                "ISel.c",
                "Pipeline.c",
                "ISched.c",
                "Regalloc.c",
                "Emitter.c",
                "Driver.c",
                defines=(
                    'ARCHON_IR_PROGRAM_FILE="Program.i"',
                    "ARCHON_ISEL_NO_MAIN",
                    "ARCHON_REGALLOC_NO_MAIN",
                ),
            )
            self.assertNotIn(
                "ARCHON_DRIVER_ANF_PIPELINE",
                (root / "Driver.c").read_text(encoding="ascii"),
            )

            listed = subprocess.run(
                [str(root / "driver"), "--list-artifacts"],
                cwd=root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.splitlines()
            self.assertIn("MIR.i", listed)
            self.assertIn("Regallocated.i", listed)
            self.assertIn("Program.s", listed)

            subprocess.run(
                [
                    str(root / "driver"),
                    "--stop-after",
                    "isel",
                    "--keep-artifacts",
                ],
                cwd=root,
                check=True,
            )
            self.assertTrue((root / "Selected.i").is_file())
            self.assertFalse((root / "Scheduled.i").exists())

            subprocess.run(
                [str(root / "driver"), "--clean"], cwd=root, check=True
            )
            self.assertTrue((root / "Program.i").is_file())
            self.assertFalse((root / "Selected.i").exists())

            subprocess.run(
                [str(root / "driver"), "--keep-artifacts"],
                cwd=root,
                check=True,
            )
            assembly = (root / "Program.s").read_text(encoding="ascii")
            self.assertIn(".Larchon_0:", assembly)
            self.assertIn("addi x2, x1, 5", assembly)
            self.assertIn("add x1, x2, x2", assembly)
            self.assertIn(
                'ARCHON_SELECTED_OP(0, "ADDI"',
                (root / "Selected.i").read_text(encoding="ascii"),
            )
            self.assertIn(
                'ARCHON_SELECTED_OP(1, "ADD"',
                (root / "Selected.i").read_text(encoding="ascii"),
            )
            first_allocation = (root / "Regallocated.i").read_bytes()
            subprocess.run(
                [str(root / "driver"), "--keep-artifacts"],
                cwd=root,
                check=True,
            )
            self.assertEqual(
                first_allocation, (root / "Regallocated.i").read_bytes()
            )

            subprocess.run(
                [
                    str(root / "driver"),
                    "--dialect",
                    "sandstone",
                    "--output",
                    "custom.asm",
                    "--keep-artifacts",
                ],
                cwd=root,
                check=True,
            )
            sandstone = (root / "custom.asm").read_text(encoding="ascii")
            self.assertIn("local targets = lsandas.symas.targets", sandstone)
            self.assertIn("addi x2, x1, 5", sandstone)

            subprocess.run(
                [str(root / "driver"), "--clean"], cwd=root, check=True
            )
            self.assertTrue((root / "Program.i").is_file())
            for artifact in (
                "Rewritten.i",
                "MIR.i",
                "Selected.i",
                "InstrMap.i",
                "Scheduled.i",
                "RegMap.i",
                "Regallocated.i",
                "Program.s",
                "Program.asm",
            ):
                self.assertFalse((root / artifact).exists(), artifact)

            subprocess.run([str(root / "driver")], cwd=root, check=True)
            self.assertTrue((root / "Program.s").is_file())
            self.assertFalse((root / "Regallocated.i").exists())

    def test_emitter_rejects_malformed_runtime_artifact(self):
        database = compile_file(RV32)
        self.assertEqual(
            emitter.emit({"db": database, "options": {"oracle": False}}),
            emitter.emit({
                "databases": [database],
                "options": {"oracle": False},
            }),
        )
        with self.assertRaisesRegex(ArchonError, "exactly one"):
            emitter.emit({"databases": []})
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            generated = emitter.emit({
                "db": database,
                "options": {"oracle": False},
            })
            self.write_files(root, generated)
            (root / "emit.c").write_text(
                """\
#include "Emitter.h"
#include <string.h>

int main(void) {
    Emitter_Options options;
    Emitter_Diagnostic diagnostic;
    memset(&options, 0, sizeof options);
    options.dialect = EMITTER_DIALECT_GAS;
    options.diagnostics = stderr;
    return Emitter_emit(&options, &diagnostic) == EMITTER_STATUS_BAD_ARTIFACT
        ? 0 : 1;
}
""",
                encoding="ascii",
            )
            (root / "Regallocated.i").write_text(
                """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(1)
REGALLOC_INSTRUCTION_END()
""",
                encoding="ascii",
            )
            self.compile(root, "emit", "Emitter.c", "emit.c")
            completed = subprocess.run(
                [str(root / "emit")],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(completed.returncode, 0)
            self.assertIn("invalid instruction end", completed.stderr)
            self.assertFalse((root / "Program.s").exists())

    def test_emitter_comments_labels_and_hook_validation(self):
        database = compile_file(RV32)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                emitter.emit({
                    "databases": [database],
                    "options": {"oracle": False},
                }),
            )
            (root / "emit.c").write_text(
                """\
#include "Emitter.h"
#include <string.h>

static int calls;

static int instruction_hook(
        FILE *output, Emitter_Dialect dialect, const char *opcode,
        const Emitter_Operand *operands, size_t operand_count, void *user) {
    (void)dialect;
    (void)opcode;
    (void)operands;
    (void)operand_count;
    (void)user;
    ++calls;
    return fputs("hook\\n", output) == EOF ? -1 : 0;
}

int main(int argc, char **argv) {
    Emitter_Options options;
    Emitter_Diagnostic diagnostic;
    Emitter_Hooks hooks;
    Emitter_Status status;
    if (argc != 4)
        return 2;
    memset(&options, 0, sizeof options);
    memset(&hooks, 0, sizeof hooks);
    options.dialect = EMITTER_DIALECT_GAS;
    options.input_path = argv[1];
    options.output_path = argv[2];
    if (strcmp(argv[3], "range") == 0) {
        hooks.instruction = instruction_hook;
        options.hooks = &hooks;
    }
    status = Emitter_emit(&options, &diagnostic);
    if (strcmp(argv[3], "ok") == 0)
        return status == EMITTER_STATUS_OK ? 0 : 1;
    if (strcmp(argv[3], "range") == 0)
        return status == EMITTER_STATUS_OPERAND_RANGE && calls == 0 ? 0 : 1;
    if (strcmp(argv[3], "undefined") == 0)
        return status == EMITTER_STATUS_UNDEFINED_LABEL ? 0 : 1;
    return 2;
}
""",
                encoding="ascii",
            )
            self.compile(root, "emit", "Emitter.c", "emit.c")

            (root / "good.i").write_text(
                """\
/* A canonical allocator record can contain nested macro arguments.
 * The runtime emitter ignores allocation facts and consumes the section. */
ARCHON_REGALLOCATED_BEGIN(UINT64_C(1), ARCHON_LOCATION(1, 2))
REGALLOC_EMISSION_BEGIN(1) // one selected instruction
REGALLOC_LABEL(9)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(1)
REGALLOC_OPERAND_REGISTER(2)
REGALLOC_OPERAND_LABEL(9)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [str(root / "emit"), "good.i", "good.s", "ok"],
                cwd=root,
                check=True,
            )
            assembly = (root / "good.s").read_text(encoding="ascii")
            self.assertIn(".Larchon_9:", assembly)
            self.assertIn("addi x1, x2, .Larchon_9", assembly)

            (root / "range.i").write_text(
                """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(99)
REGALLOC_OPERAND_REGISTER(2)
REGALLOC_OPERAND_IMMEDIATE(1)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [str(root / "emit"), "range.i", "range.s", "range"],
                cwd=root,
                check=True,
            )
            self.assertFalse((root / "range.s").exists())

            (root / "undefined.i").write_text(
                """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(1)
REGALLOC_OPERAND_REGISTER(2)
REGALLOC_OPERAND_LABEL(10)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [
                    str(root / "emit"),
                    "undefined.i",
                    "undefined.s",
                    "undefined",
                ],
                cwd=root,
                check=True,
            )
            self.assertFalse((root / "undefined.s").exists())

    def test_registerless_emitter_and_unavailable_driver_are_strict_c99(self):
        database = compile_file(WASM)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                emitter.emit({
                    "architectures": [database],
                    "options": {"oracle": False},
                }),
            )
            self.compile(root, "driver", "Emitter.c", "Driver.c")
            subprocess.run(
                [str(root / "driver"), "--help"],
                cwd=root,
                check=True,
                stdout=subprocess.PIPE,
            )
            completed = subprocess.run(
                [str(root / "driver")],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(completed.returncode, 1)
            self.assertIn("full pipeline unavailable", completed.stderr)

    def test_oracle_emitter_metadata_is_bounded_and_never_injected(self):
        database = compile_file(RV32)
        context = {
            "db": database,
            "options": {"oracle": True},
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, oracle.emit(context))
            generated = emitter.emit(context)
            self.assertIn('#include "Oracle.h"', generated["Emitter.c"])
            self.assertIn('#include "Oracle.h"', generated["Driver.c"])
            self.write_files(root, generated)
            (root / "emit.c").write_text(
                """\
#include "Emitter.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    ArchonOracleOptions oracle;
    Emitter_Options options;
    Emitter_Diagnostic diagnostic;
    Emitter_Status status;
    if (argc != 5)
        return 2;
    archon_oracle_options_init(&oracle);
    oracle.socket_path = getenv("ARCHON_ORACLE_SOCKET");
    memset(&options, 0, sizeof options);
    options.dialect = EMITTER_DIALECT_GAS;
    options.input_path = argv[1];
    options.output_path = argv[2];
    options.diagnostics = stderr;
    options.oracle = &oracle;
    status = Emitter_emit(&options, &diagnostic);
    if (strcmp(argv[3], "ok") == 0)
        return status == EMITTER_STATUS_OK ? 0 : 1;
    if (strcmp(argv[3], "range") == 0)
        return status == EMITTER_STATUS_OPERAND_RANGE &&
               strstr(diagnostic.message, argv[4]) != NULL ? 0 : 1;
    if (strcmp(argv[3], "oracle") == 0)
        return status == EMITTER_STATUS_ORACLE_ERROR ? 0 : 1;
    if (strcmp(argv[3], "bad-artifact") == 0)
        return status == EMITTER_STATUS_BAD_ARTIFACT ? 0 : 1;
    return 2;
}
""",
                encoding="ascii",
            )
            self.compile(
                root, "emit", "Emitter.c", "Oracle.c", "emit.c"
            )

            good = root / "good.i"
            good.write_text(
                """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(1)
REGALLOC_OPERAND_REGISTER(2)
REGALLOC_OPERAND_IMMEDIATE(1)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
""",
                encoding="ascii",
            )
            (root / "good.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 6)
ARCHON_ORACLE_STRING("emitter.comment", "reviewed ticket 7")
ARCHON_ORACLE_STRING("emitter.dialect", "sandstone")
ARCHON_ORACLE_STRING("emitter.raw_assembly", ".byte 255")
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [str(root / "emit"), "good.i", "good.asm", "ok", "-"],
                cwd=root,
                check=True,
            )
            assembly = (root / "good.asm").read_text(encoding="ascii")
            self.assertIn("; Oracle: reviewed ticket 7", assembly)
            self.assertIn("local targets = lsandas.symas.targets", assembly)
            self.assertIn("addi x1, x2, 1", assembly)
            self.assertNotIn(".byte 255", assembly)

            bad = root / "bad.i"
            bad.write_text(
                """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("ADDI", 3)
REGALLOC_OPERAND_REGISTER(99)
REGALLOC_OPERAND_REGISTER(2)
REGALLOC_OPERAND_IMMEDIATE(1)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
""",
                encoding="ascii",
            )
            (root / "bad.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 6)
ARCHON_ORACLE_STRING("emitter.diagnostic", "review ticket 9")
ARCHON_ORACLE_STRING("emitter.raw_assembly", "bypass validation")
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [
                    str(root / "emit"),
                    "bad.i",
                    "bad.s",
                    "range",
                    "review ticket 9",
                ],
                cwd=root,
                check=True,
            )
            self.assertFalse((root / "bad.s").exists())

            (root / "bad.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 6)
ARCHON_ORACLE_STRING("emitter.dialect", "raw-assembly")
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            subprocess.run(
                [
                    str(root / "emit"),
                    "bad.i",
                    "illegal.s",
                    "oracle",
                    "-",
                ],
                cwd=root,
                check=True,
            )
            self.assertFalse((root / "illegal.s").exists())

            (root / "bad.oracle.i").write_text(
                "ARCHON_ORACLE_BEGIN(1, 6) trailing\n",
                encoding="ascii",
            )
            subprocess.run(
                [
                    str(root / "emit"),
                    "bad.i",
                    "malformed.s",
                    "oracle",
                    "-",
                ],
                cwd=root,
                check=True,
            )
            self.assertFalse((root / "malformed.s").exists())

            (root / "worker.c").write_text(
                """\
#include "Oracle.h"
#include <string.h>

static ArchonOracleStatus transform(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *user_data,
        ArchonOracleDiagnostic *diagnostic) {
    static const char replacement[] = "NOT_A_REGALLOC_RECORD()\\n";
    (void)user_data;
    (void)diagnostic;
    if (request->artifact_capacity < sizeof replacement - 1u)
        return ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED;
    memcpy(request->artifact_data, replacement, sizeof replacement - 1u);
    response->artifact_size = sizeof replacement - 1u;
    response->artifact_changed = 1;
    return archon_oracle_response_add_string(
        response, "emitter.raw_assembly", ".byte 255");
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    if (argc != 2)
        return 2;
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 1u, 5000u, &diagnostic);
    if (status == ARCHON_ORACLE_STATUS_OK)
        status = archon_oracle_server_serve_once(
            &server, transform, NULL, &diagnostic);
    archon_oracle_server_dispose(&server);
    return status == ARCHON_ORACLE_STATUS_OK ? 0 : 1;
}
""",
                encoding="ascii",
            )
            self.compile(root, "worker", "Oracle.c", "worker.c")
            transformed = root / "transformed.i"
            transformed.write_text(good.read_text(encoding="ascii"), encoding="ascii")
            socket_path = root / "oracle.sock"
            worker = subprocess.Popen(
                [str(root / "worker"), str(socket_path)],
                cwd=root,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
            )
            for _ in range(500):
                if socket_path.exists() or worker.poll() is not None:
                    break
                time.sleep(0.01)
            self.assertTrue(socket_path.exists())
            environment = os.environ.copy()
            environment["ARCHON_ORACLE_SOCKET"] = str(socket_path)
            subprocess.run(
                [
                    str(root / "emit"),
                    "transformed.i",
                    "transformed.s",
                    "bad-artifact",
                    "-",
                ],
                cwd=root,
                env=environment,
                check=True,
            )
            stdout, stderr = worker.communicate(timeout=10)
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            self.assertEqual(
                transformed.read_text(encoding="ascii"),
                "NOT_A_REGALLOC_RECORD()\n",
            )
            self.assertFalse((root / "transformed.s").exists())

    def test_oracle_driver_manifest_preserves_source_sidecar(self):
        database = compile_file(RV32)
        context = {
            "db": database,
            "databases": [database],
            "ir": "anf",
            "options": {"oracle": True},
        }
        enabled = emitter.emit(context)
        disabled = emitter.emit({
            "db": database,
            "ir": "anf",
            "options": {"oracle": False},
        })
        driver = enabled["Driver.c"]
        self.assertIn('"Regallocated.oracle.i"', driver)
        self.assertIn('"Scheduled.oracle.i"', driver)
        self.assertNotIn('"Program.oracle.i"', driver)
        self.assertNotIn("archon_oracle_process_file", driver)
        self.assertIn("pipeline.oracle = &oracle;", driver)
        self.assertIn("emitter.oracle = &oracle;", driver)
        self.assertIn('getenv("ARCHON_ORACLE_SOCKET")', driver)
        self.assertNotIn("Oracle.h", disabled["Emitter.c"])
        self.assertNotIn("Oracle.h", disabled["Driver.c"])
        self.assertNotIn("archon_oracle_", disabled["Emitter.c"])
        self.assertNotIn("ArchonOracle", disabled["Driver.c"])
        self.assertNotIn("pipeline.oracle", disabled["Driver.c"])
        self.assertNotIn("emitter.oracle", disabled["Driver.c"])
        self.assertNotIn(".oracle.i", disabled["Driver.c"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, enabled)
            self.write_files(root, oracle.emit(context))
            self.write_files(
                root, {"Pipeline.h": pipeline.emit(context)["Pipeline.h"]}
            )
            (root / "stub.c").write_text(
                """\
#include "Pipeline.h"
#include <stdlib.h>
#include <string.h>

int archon_pipeline_run(const ArchonPipelineOptions *options) {
    const char *expected = getenv("ARCHON_TEST_ORACLE_SOCKET");
    FILE *artifact;
    if (expected != NULL &&
            (options == NULL || options->oracle == NULL ||
             options->oracle->socket_path == NULL ||
             strcmp(options->oracle->socket_path, expected) != 0))
        return 1;
    if (getenv("ARCHON_TEST_PIPELINE_FAIL") == NULL)
        return 0;
    artifact = fopen("MIR.i", "wb");
    if (artifact != NULL)
        (void)fclose(artifact);
    artifact = fopen("MIR.oracle.i", "wb");
    if (artifact != NULL)
        (void)fclose(artifact);
    artifact = fopen("Selected.oracle.i", "wb");
    if (artifact != NULL)
        (void)fclose(artifact);
    return 1;
}

Emitter_Status Emitter_emit(
        const Emitter_Options *options,
        Emitter_Diagnostic *diagnostic) {
    (void)options;
    if (diagnostic != NULL)
        diagnostic->status = EMITTER_STATUS_OK;
    return EMITTER_STATUS_OK;
}
""",
                encoding="ascii",
            )
            self.compile(
                root, "driver", "Driver.c", "Oracle.c", "stub.c"
            )
            listed = subprocess.run(
                [str(root / "driver"), "--list-artifacts"],
                cwd=root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            ).stdout.splitlines()
            self.assertIn("Regallocated.oracle.i", listed)
            self.assertNotIn("Program.oracle.i", listed)

            (root / "Program.i").write_text("input", encoding="ascii")
            environment = os.environ.copy()
            environment["ARCHON_ORACLE_SOCKET"] = str(root / "configured.sock")
            environment["ARCHON_TEST_ORACLE_SOCKET"] = str(
                root / "configured.sock"
            )
            subprocess.run(
                [str(root / "driver"), "--keep-artifacts"],
                cwd=root,
                env=environment,
                check=True,
            )

            environment["ARCHON_TEST_PIPELINE_FAIL"] = "1"
            failed = subprocess.run(
                [str(root / "driver"), "--keep-artifacts"],
                cwd=root,
                env=environment,
            )
            self.assertEqual(failed.returncode, 1)
            self.assertTrue((root / "MIR.i").is_file())
            self.assertTrue((root / "MIR.oracle.i").is_file())
            self.assertFalse((root / "Selected.oracle.i").exists())

            for name in (
                "Program.i",
                "Program.oracle.i",
                "Regallocated.i",
                "Regallocated.oracle.i",
                "Program.s",
            ):
                (root / name).write_text(name, encoding="ascii")
            subprocess.run(
                [str(root / "driver"), "--clean"], cwd=root, check=True
            )
            self.assertTrue((root / "Program.i").is_file())
            self.assertTrue((root / "Program.oracle.i").is_file())
            self.assertFalse((root / "Regallocated.i").exists())
            self.assertFalse((root / "Regallocated.oracle.i").exists())
            self.assertFalse((root / "Program.s").exists())


if __name__ == "__main__":
    unittest.main()
