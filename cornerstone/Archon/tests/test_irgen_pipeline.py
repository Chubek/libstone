"""Universal strict-C99 pipeline coverage for every generated IR form."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest

from archon.compiler import compile_file
from archon.irgen import EXPECTED_REPRESENTATIONS, generate_ir
from archon.irgen.macros import validate_stream


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
PDP11 = ROOT / "stdspec" / "pdp11" / "root.arch"


PROGRAMS = {
    "anf": """\
IR_PROGRAM_BEGIN("pipeline-test");
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
""",
    "gated": """\
ARCHON_IR_PROGRAM_BEGIN(1, "gated")
ARCHON_IR_BLOCK(1, 0, "entry")
ARCHON_IR_NODE(1, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 32, UINT64_C(0x0))
ARCHON_IR_NODE(2, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 32, UINT64_C(0x1))
ARCHON_IR_NODE(3, 1, ARCHON_IR_OP_RISCV32_ADD, ARCHON_IR_TYPE_UINT, 32, UINT64_C(0x0))
ARCHON_IR_INPUT(3, 1, 0)
ARCHON_IR_INPUT(3, 2, 0)
ARCHON_IR_PROGRAM_END()
""",
    "memory-ssa": """\
ARCHON_IR_DECLARE_BLOCK(0, "entry");
ARCHON_IR_ENTRY(0);
ARCHON_IR_BLOCK(0);
ARCHON_IR_PARAMETER(0, IR_TYPE_UINT, 32, "x");
ARCHON_IR_PARAMETER(1, IR_TYPE_UINT, 32, "y");
{
    const IR_value_id values[2] = {0, 1};
    ARCHON_IR_TARGET(
        2, IR_NO_VALUE, ARCHON_IR_OP_RISCV32_ADD,
        IR_TYPE_BITS, 32, IR_NO_VALUE, 2, values);
}
ARCHON_IR_RETURN(2, IR_NO_VALUE);
""",
    "sea-of-nodes": """\
ARCHON_IR_PROGRAM_BEGIN(1u, 2u, 1u)
ARCHON_IR_NODE(
    0u, ARCHON_IR_NODE_CONSTANT, 0u, ARCHON_IR_FLAG_PURE,
    ARCHON_IR_NODE_NONE, UINT64_C(1), 0u)
ARCHON_IR_NODE(
    1u, ARCHON_IR_NODE_GENERIC, ARCHON_IR_OP_RISCV32_LUI,
    ARCHON_IR_FLAG_PURE, ARCHON_IR_NODE_NONE, UINT64_C(0), 1u)
ARCHON_IR_INPUT(1u, 0u, ARCHON_IR_INPUT_DATA, 0u)
ARCHON_IR_ROOT(1u)
ARCHON_IR_PROGRAM_END()
""",
    "typed-tac": """\
IR_PROGRAM_BEGIN(1, 3, 4)
IR_BLOCK(0, "entry")
IR_OP(0, 0, 0, 0, 3, 32, 0, 0, 0, 4294967295, 4294967295, "x")
IR_OP(1, 0, 2, 1, 3, 32, 0, 0, 1, 4294967295, 4294967295, "entry")
IR_OP(2, 0, 4, 2, 3, 32, 0, 2, 0, 4294967295, 4294967295, "entry")
IR_INPUT(2, 0, 0, 4294967295)
IR_INPUT(2, 1, 1, 4294967295)
IR_OP(3, 0, 21, 4294967295, 0, 0, 0, 1, 0, 4294967295, 4294967295, "entry")
IR_INPUT(3, 0, 2, 4294967295)
IR_TARGETS(3, 4294967295, 4294967295)
IR_PROGRAM_END()
""",
    "untyped-tac": """\
PROGRAM_PARAMETER(0u, 0u);
PROGRAM_CONSTANT(1u, 1);
PROGRAM_BINARY(2u, IR_OPERATOR_ADD, 0u, 1u);
PROGRAM_RETURN(2u);
""",
    "cps": """\
CPS_PROGRAM_BEGIN(1)
CPS_TYPE(1, ARCHON_CPS_TYPE_BITS, 32, 0)
CPS_TYPE(2, ARCHON_CPS_TYPE_EFFECT, 0, 0)
CPS_TYPE(3, ARCHON_CPS_TYPE_CONTROL, 0, 0)
CPS_SIGNATURE_BEGIN(1, 1, 2, 3)
CPS_SIGNATURE_PARAMETER(1)
CPS_SIGNATURE_PARAMETER(2)
CPS_SIGNATURE_PARAMETER(3)
CPS_SIGNATURE_END()
CPS_FUNCTION(1, "identity", 1, 1)
CPS_BLOCK(1, 1, 1)
CPS_PARAMETER(1, 1, 1)
CPS_PARAMETER(2, 1, 2)
CPS_PARAMETER(3, 1, 3)
CPS_NODE_BEGIN(
    1, 0, 1, ARCHON_CPS_OP_RETURN, 0, 0, 0, 0, 0, 2, 2, 3, 3)
CPS_NODE_OPERAND(1)
CPS_NODE_END()
CPS_PROGRAM_END()
""",
    "supercombinators": """\
SC_PROGRAM_BEGIN();
SC_LITERAL(0, 1);
SC_INSTRUCTION_1(1, "target:LUI", 0);
SC_LITERAL(2, 5);
SC_INSTRUCTION_2(3, "target:ADDI", 1, 2);
SC_ENTRY(3);
SC_PROGRAM_END();
""",
}


FULL_PIPELINES = {
    "anf",
    "gated",
    "memory-ssa",
    "sea-of-nodes",
    "typed-tac",
    "untyped-tac",
    "supercombinators",
}


class UniversalPipelineTests(unittest.TestCase):
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

    @staticmethod
    def write_files(directory, files):
        for name, text in files.items():
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="ascii", newline="\n")

    def compile_driver(self, compiler, directory, *, oracle_enabled=True):
        executable = directory / f"driver-{compiler}"
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
                "Driver.c",
            )
        )
        subprocess.run(
            [
                compiler,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
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
        )
        return executable

    def compile_artifact_api(self, compiler, directory):
        source = directory / "ArtifactAPI.c"
        source.write_text(
            """\
#include "Pipeline.h"

#include <stdio.h>

static int emit(
        const char *path,
        int (*function)(FILE *, char *, size_t)) {
    char diagnostic[256];
    FILE *output = fopen(path, "wb");
    int status;
    if (output == NULL)
        return 1;
    status = function(output, diagnostic, sizeof diagnostic);
    if (fclose(output) != 0)
        status = 1;
    if (status != 0) {
        (void)fprintf(stderr, "%s\\n", diagnostic);
        (void)remove(path);
    }
    return status;
}

int main(void) {
    if (emit("CanonicalProgram.i", archon_pipeline_emit_program) != 0)
        return 1;
    if (emit("API-MIR.i", archon_pipeline_emit_mir) != 0) {
        (void)remove("CanonicalProgram.i");
        return 1;
    }
    return 0;
}
""",
            encoding="ascii",
            newline="\n",
        )
        executable = directory / f"artifact-api-{compiler}"
        subprocess.run(
            [
                compiler,
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                "-DARCHON_ISEL_NO_MAIN",
                "-DARCHON_REGALLOC_NO_MAIN",
                "-I.",
                "IR.c",
                "Graphs.c",
                "Oracle.c",
                "Rewrite.c",
                "ISel.c",
                "Pipeline.c",
                "ISched.c",
                "Regalloc.c",
                "Emitter.c",
                source.name,
                "-o",
                executable.name,
            ],
            cwd=directory,
            check=True,
        )
        return executable

    def test_oracle_generation_gate_and_closed_boundary_stages(self):
        disabled = generate_ir(
            "anf", [self.database], options={"oracle": False}
        )
        self.assertNotIn("Oracle.c", disabled)
        for name in ("Pipeline.c", "Pipeline.h", "Rewrite.c", "Scheduler.c"):
            self.assertNotIn('"Oracle.h"', disabled[name])
            self.assertNotIn("archon_oracle_", disabled[name])
            self.assertNotIn("@ORACLE_", disabled[name])

        enabled = generate_ir("anf", [self.database])
        pipeline_source = enabled["Pipeline.c"]
        self.assertIn('#include "Oracle.h"', pipeline_source)
        self.assertNotIn("@ORACLE_", pipeline_source)
        for artifact, stage in (
            ("Rewritten.i", "REWRITE"),
            ("MIR.i", "SCHEDULE"),
            ("MIR.i", "ISEL"),
            ("InstrMap.i", "ISCHED"),
            ("Scheduled.i", "ISCHED"),
            ("RegMap.i", "REGALLOC"),
        ):
            self.assertIn(
                f'"{artifact}", ARCHON_ORACLE_STAGE_{stage}',
                pipeline_source,
            )
        self.assertIn(
            "archon_isel_select_operation_with_annotations",
            pipeline_source,
        )
        self.assertIn(
            "isched_schedule_with_annotations", pipeline_source
        )

        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-oracle-disabled-"
        ) as raw:
            directory = Path(raw)
            self.write_files(directory, disabled)
            (directory / "Program.i").write_text(
                PROGRAMS["anf"], encoding="ascii", newline="\n"
            )
            executable = self.compile_driver(
                self.compilers[0], directory, oracle_enabled=False
            )
            result = subprocess.run(
                [
                    str(executable),
                    "--stop-after",
                    "rewrite",
                    "--keep-artifacts",
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_linked_pipeline_consumes_typed_sidecars_at_each_boundary(self):
        generated = generate_ir("anf", [self.database])
        pattern = re.search(
            r'ARCHON_ISEL_PATTERN\("RISCV32", ([0-9]+)u, '
            r'"ADDI", "add"',
            generated["ISelPattern_RISCV32.i"],
        )
        self.assertIsNotNone(pattern)
        addi_pattern = int(pattern.group(1))
        driver = r'''#include "Pipeline.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    ArchonPipelineOptions options;
    memset(&options, 0, sizeof options);
    options.rewrite = ARCHON_PIPELINE_REWRITE_CONVENTIONAL;
    options.diagnostics = stderr;
    if (argc != 2)
        return 2;
    if (strcmp(argv[1], "rewrite") == 0)
        options.stop_after = ARCHON_PIPELINE_REWRITE;
    else if (strcmp(argv[1], "isel") == 0)
        options.stop_after = ARCHON_PIPELINE_ISEL;
    else if (strcmp(argv[1], "isched") == 0)
        options.stop_after = ARCHON_PIPELINE_ISCHED;
    else if (strcmp(argv[1], "regalloc") == 0)
        options.stop_after = ARCHON_PIPELINE_REGALLOC;
    else
        return 2;
    return archon_pipeline_run(&options);
}
'''
        sources = (
            "IR.c",
            "Graphs.c",
            "Oracle.c",
            "Rewrite.c",
            "ISel.c",
            "Pipeline.c",
            "ISched.c",
            "Regalloc.c",
            "Emitter.c",
            "SidecarDriver.c",
        )
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-linked-sidecars-"
        ) as raw:
            directory = Path(raw)
            self.write_files(directory, generated)
            (directory / "Program.i").write_text(
                PROGRAMS["anf"], encoding="ascii", newline="\n"
            )
            (directory / "SidecarDriver.c").write_text(
                driver, encoding="ascii", newline="\n"
            )
            for compiler in self.compilers:
                executable = directory / f"sidecar-driver-{compiler}"
                subprocess.run(
                    [
                        compiler,
                        "-std=c99",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-pedantic",
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
                )

                rewrite_sidecar = """\
ARCHON_ORACLE_BEGIN(1, 1)
ARCHON_ORACLE_STRING("rewrite.engine", "egraph")
ARCHON_ORACLE_U64("rewrite.max_iterations", UINT64_C(2))
ARCHON_ORACLE_STRING("proof", "untrusted-and-ignored")
ARCHON_ORACLE_END()
"""
                (directory / "Rewritten.oracle.i").write_text(
                    rewrite_sidecar, encoding="ascii"
                )
                rewritten = subprocess.run(
                    [str(executable), "rewrite"],
                    cwd=directory,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(
                    rewritten.returncode, 0, rewritten.stderr
                )
                rewrite_output = (
                    directory / "Rewritten.i"
                ).read_text(encoding="ascii")
                self.assertIn('"egraph"', rewrite_output)
                self.assertIn(
                    "ARCHON_EQUIVALENCE_UNKNOWN", rewrite_output
                )
                self.assertEqual(
                    (directory / "Rewritten.oracle.i").read_text(
                        encoding="ascii"
                    ),
                    rewrite_sidecar,
                )
                (directory / "Rewritten.oracle.i").unlink()

                (directory / "MIR.oracle.i").write_text(
                    f"""\
ARCHON_ORACLE_BEGIN(1, 3)
ARCHON_ORACLE_BOOL("isel.pattern.{addi_pattern}.deny", 1)
ARCHON_ORACLE_END()
""",
                    encoding="ascii",
                )
                rejected_isel = subprocess.run(
                    [str(executable), "isel"],
                    cwd=directory,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(rejected_isel.returncode, 1)
                self.assertIn(
                    "pipeline: isel stage:", rejected_isel.stderr
                )
                self.assertIn(
                    "no legal instruction selection", rejected_isel.stderr
                )
                (directory / "MIR.oracle.i").unlink()

                (directory / "InstrMap.oracle.i").write_text(
                    """\
ARCHON_ORACLE_BEGIN(1, 4)
ARCHON_ORACLE_STRING("isched.operation.0.priority", "bad")
ARCHON_ORACLE_END()
""",
                    encoding="ascii",
                )
                rejected_isched = subprocess.run(
                    [str(executable), "isched"],
                    cwd=directory,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(rejected_isched.returncode, 1)
                self.assertIn(
                    "pipeline: isched stage:", rejected_isched.stderr
                )
                self.assertIn("type mismatch", rejected_isched.stderr)
                (directory / "InstrMap.oracle.i").unlink()

                (directory / "RegMap.oracle.i").write_text(
                    """\
ARCHON_ORACLE_BEGIN(1, 5)
ARCHON_ORACLE_STRING("regalloc.spill_cost.1", "bad")
ARCHON_ORACLE_END()
""",
                    encoding="ascii",
                )
                rejected_regalloc = subprocess.run(
                    [str(executable), "regalloc"],
                    cwd=directory,
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(rejected_regalloc.returncode, 1)
                self.assertIn(
                    "pipeline: regalloc stage:",
                    rejected_regalloc.stderr,
                )
                self.assertIn(
                    "spill-cost annotation is not a bounded U64",
                    rejected_regalloc.stderr,
                )
                (directory / "RegMap.oracle.i").unlink()

    def test_linked_worker_reaches_regmap_and_advice_is_applied(self):
        generated = generate_ir("anf", [self.database])
        driver = r'''#include "Pipeline.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    ArchonOracleOptions oracle;
    ArchonPipelineOptions options;
    if (argc != 2)
        return 2;
    archon_oracle_options_init(&oracle);
    oracle.socket_path = argv[1];
    oracle.annotation_path = "Shared.oracle.i";
    memset(&options, 0, sizeof options);
    options.stop_after = ARCHON_PIPELINE_REGALLOC;
    options.rewrite = ARCHON_PIPELINE_REWRITE_CONVENTIONAL;
    options.diagnostics = stderr;
    options.oracle = &oracle;
    return archon_pipeline_run(&options);
}
'''
        worker = r'''#include "Oracle.h"

#include <stdio.h>
#include <string.h>

typedef struct WorkerContext {
    size_t index;
    int transform;
} WorkerContext;

static ArchonOracleStatus advise(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *opaque,
        ArchonOracleDiagnostic *diagnostic) {
    static const ArchonOracleStage stages[] = {
        ARCHON_ORACLE_STAGE_REWRITE,
        ARCHON_ORACLE_STAGE_SCHEDULE,
        ARCHON_ORACLE_STAGE_ISEL,
        ARCHON_ORACLE_STAGE_ISCHED,
        ARCHON_ORACLE_STAGE_ISCHED,
        ARCHON_ORACLE_STAGE_REGALLOC
    };
    static const char *const paths[] = {
        "Rewritten.i", "MIR.i", "MIR.i", "InstrMap.i",
        "Scheduled.i", "RegMap.i"
    };
    WorkerContext *context = (WorkerContext *)opaque;
    ArchonOracleStatus status = ARCHON_ORACLE_STATUS_OK;
    if (request == NULL || response == NULL || context == NULL ||
            context->index >= sizeof stages / sizeof stages[0] ||
            request->stage != stages[context->index] ||
            strcmp(request->artifact_path, paths[context->index]) != 0) {
        archon_oracle_diagnostic_set(
            diagnostic, ARCHON_ORACLE_STATUS_INVALID_ARGUMENT, 0,
            "unexpected linked-pipeline Oracle boundary");
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    }
    response->artifact_size = request->artifact_size;
    response->artifact_changed = 0;
    if (context->transform) {
        if (request->artifact_size == 0u)
            return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
        request->artifact_data[0] ^= 1u;
        response->artifact_changed = 1;
    } else if (request->stage == ARCHON_ORACLE_STAGE_REWRITE) {
        status = archon_oracle_response_add_string(
            response, "rewrite.engine", "egraph");
    } else if (request->stage == ARCHON_ORACLE_STAGE_REGALLOC) {
        status = archon_oracle_response_add_string(
            response, "regalloc.spill_cost.1", "wrong-type");
    }
    ++context->index;
    return status;
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    WorkerContext context;
    size_t request, request_count;
    if (argc != 2 && argc != 3)
        return 2;
    memset(&context, 0, sizeof context);
    context.transform = argc == 3;
    request_count = context.transform ? 1u : 6u;
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 1u, 0u, &diagnostic);
    if (status != ARCHON_ORACLE_STATUS_OK)
        goto failed;
    for (request = 0u; request < request_count; ++request) {
        status = archon_oracle_server_serve_once(
            &server, advise, &context, &diagnostic);
        if (status != ARCHON_ORACLE_STATUS_OK)
            goto failed;
    }
    archon_oracle_server_dispose(&server);
    return context.index == request_count ? 0 : 1;

failed:
    (void)fprintf(
        stderr, "%s: %s\n",
        archon_oracle_status_string(status), diagnostic.message);
    archon_oracle_server_dispose(&server);
    return 1;
}
'''
        compiler = self.compilers[0]
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-linked-worker-"
        ) as raw:
            directory = Path(raw)
            self.write_files(directory, generated)
            (directory / "Program.i").write_text(
                PROGRAMS["anf"], encoding="ascii", newline="\n"
            )
            (directory / "IPCDriver.c").write_text(
                driver, encoding="ascii", newline="\n"
            )
            (directory / "OracleWorker.c").write_text(
                worker, encoding="ascii", newline="\n"
            )
            common_flags = (
                "-std=c99",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-I.",
            )
            subprocess.run(
                [
                    compiler,
                    *common_flags,
                    "Oracle.c",
                    "OracleWorker.c",
                    "-o",
                    "oracle-worker",
                ],
                cwd=directory,
                check=True,
            )
            subprocess.run(
                [
                    compiler,
                    *common_flags,
                    '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                    "-DARCHON_ISEL_NO_MAIN",
                    "-DARCHON_REGALLOC_NO_MAIN",
                    "IR.c",
                    "Graphs.c",
                    "Oracle.c",
                    "Rewrite.c",
                    "ISel.c",
                    "Pipeline.c",
                    "ISched.c",
                    "Regalloc.c",
                    "Emitter.c",
                    "IPCDriver.c",
                    "-o",
                    "ipc-driver",
                ],
                cwd=directory,
                check=True,
            )
            socket_path = directory / "oracle.sock"
            worker_process = subprocess.Popen(
                [str(directory / "oracle-worker"), str(socket_path)],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            for _ in range(200):
                if socket_path.exists() or worker_process.poll() is not None:
                    break
                time.sleep(0.01)
            self.assertTrue(
                socket_path.exists(),
                "Oracle worker did not create its socket",
            )
            result = subprocess.run(
                [str(directory / "ipc-driver"), str(socket_path)],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=20,
            )
            worker_stdout, worker_stderr = worker_process.communicate(
                timeout=20
            )
            self.assertEqual(
                worker_process.returncode,
                0,
                f"worker stdout={worker_stdout!r}, "
                f"stderr={worker_stderr!r}",
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("pipeline: regalloc stage:", result.stderr)
            self.assertIn(
                "spill-cost annotation is not a bounded U64",
                result.stderr,
            )
            rewritten = (directory / "Rewritten.i").read_text(
                encoding="ascii"
            )
            self.assertIn('"egraph"', rewritten)
            self.assertIn(
                "ARCHON_EQUIVALENCE_UNKNOWN", rewritten
            )
            shared_sidecar = (directory / "Shared.oracle.i").read_text(
                encoding="ascii"
            )
            self.assertIn("ARCHON_ORACLE_BEGIN(1, 4)", shared_sidecar)
            regmap_sidecar = (
                directory / "RegMap.oracle.i"
            ).read_text(encoding="ascii")
            self.assertIn("ARCHON_ORACLE_BEGIN(1, 5)", regmap_sidecar)

            transformed_socket = directory / "transform.sock"
            transform_worker = subprocess.Popen(
                [
                    str(directory / "oracle-worker"),
                    str(transformed_socket),
                    "transform",
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            for _ in range(200):
                if (
                    transformed_socket.exists()
                    or transform_worker.poll() is not None
                ):
                    break
                time.sleep(0.01)
            self.assertTrue(
                transformed_socket.exists(),
                "transforming Oracle worker did not create its socket",
            )
            transformed = subprocess.run(
                [
                    str(directory / "ipc-driver"),
                    str(transformed_socket),
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=20,
            )
            transform_stdout, transform_stderr = (
                transform_worker.communicate(timeout=20)
            )
            self.assertEqual(
                transform_worker.returncode,
                0,
                f"worker stdout={transform_stdout!r}, "
                f"stderr={transform_stderr!r}",
            )
            self.assertEqual(transformed.returncode, 1)
            self.assertIn("pipeline: rewrite stage:", transformed.stderr)
            self.assertIn(
                "artifact transformations are unsupported",
                transformed.stderr,
            )
            self.assertFalse((directory / "Rewritten.i").exists())
            self.assertFalse((directory / "Shared.oracle.i").exists())

            missing_socket = directory / "missing.sock"
            transport = subprocess.run(
                [
                    str(directory / "ipc-driver"),
                    str(missing_socket),
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                timeout=20,
            )
            self.assertEqual(transport.returncode, 1)
            self.assertIn("pipeline: rewrite stage:", transport.stderr)
            self.assertIn("Oracle rejected Rewritten.i", transport.stderr)
            self.assertFalse((directory / "Rewritten.i").exists())
            self.assertFalse((directory / "Shared.oracle.i").exists())

    def test_every_representation_has_a_linked_conservative_pipeline(self):
        self.assertEqual(set(PROGRAMS), set(EXPECTED_REPRESENTATIONS))
        artifacts = (
            ("MIR.i", "mir"),
            ("InstrMap.i", "instr_map"),
            ("Scheduled.i", "scheduled"),
            ("RegMap.i", "reg_map"),
            ("Regallocated.i", "regallocated"),
        )
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-all-pipelines-"
        ) as raw:
            base = Path(raw)
            for representation in EXPECTED_REPRESENTATIONS:
                with self.subTest(representation=representation):
                    directory = base / representation
                    directory.mkdir()
                    generated = generate_ir(
                        representation, [self.database]
                    )
                    self.write_files(directory, generated)
                    (directory / "Program.i").write_text(
                        PROGRAMS[representation],
                        encoding="ascii",
                        newline="\n",
                    )
                    self.assertIn("Pipeline.c", generated)
                    self.assertNotIn(
                        "ARCHON_DRIVER_ANF_PIPELINE",
                        generated["Driver.c"],
                    )
                    native_program = (directory / "Program.i").read_bytes()
                    baselines = None
                    api_baselines = None
                    for compiler in self.compilers:
                        executable = self.compile_driver(
                            compiler, directory
                        )
                        artifact_api = self.compile_artifact_api(
                            compiler, directory
                        )
                        api_result = subprocess.run(
                            [str(artifact_api)],
                            cwd=directory,
                            text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                        )
                        self.assertEqual(
                            api_result.returncode, 0, api_result.stderr
                        )
                        self.assertEqual(
                            (directory / "Program.i").read_bytes(),
                            native_program,
                        )
                        api_current = (
                            (directory / "CanonicalProgram.i").read_bytes(),
                            (directory / "API-MIR.i").read_bytes(),
                        )
                        if api_baselines is None:
                            api_baselines = api_current
                        else:
                            self.assertEqual(api_current, api_baselines)
                        validate_stream(
                            api_current[0].decode("ascii"), "program"
                        )
                        validate_stream(
                            api_current[1].decode("ascii"), "mir"
                        )
                        (directory / "CanonicalConsumer.c").write_text(
                            """\
#include "CanonicalProgram.i"
#include "API-MIR.i"
int main(void) { return 0; }
""",
                            encoding="ascii",
                            newline="\n",
                        )
                        subprocess.run(
                            [
                                compiler,
                                "-std=c99",
                                "-Wall",
                                "-Wextra",
                                "-Werror",
                                "-pedantic",
                                "-I.",
                                "CanonicalConsumer.c",
                                "-o",
                                f"canonical-consumer-{compiler}",
                            ],
                            cwd=directory,
                            check=True,
                        )
                        decoded = subprocess.run(
                            [
                                str(executable),
                                "--stop-after",
                                "decode",
                                "--keep-artifacts",
                            ],
                            cwd=directory,
                            text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                        )
                        self.assertEqual(
                            decoded.returncode, 0, decoded.stderr
                        )
                        if representation not in FULL_PIPELINES:
                            rejected = subprocess.run(
                                [
                                    str(executable),
                                    "--stop-after",
                                    "isel",
                                    "--keep-artifacts",
                                ],
                                cwd=directory,
                                text=True,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE,
                            )
                            self.assertEqual(rejected.returncode, 1)
                            self.assertIn(
                                "pipeline: isel stage:", rejected.stderr
                            )
                            continue
                        completed = subprocess.run(
                            [
                                str(executable),
                                "--rewrite",
                                "egraph",
                                "--keep-artifacts",
                            ],
                            cwd=directory,
                            text=True,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                        )
                        self.assertEqual(
                            completed.returncode, 0, completed.stderr
                        )
                        self.assertTrue(
                            (directory / "Program.s").is_file()
                        )
                        rewritten = (directory / "Rewritten.i").read_text(
                            encoding="ascii"
                        )
                        self.assertIn(
                            "ARCHON_EQUIVALENCE_UNKNOWN", rewritten
                        )
                        self.assertIn(
                            "ARCHON_REWRITE_ENGINE_STATS", rewritten
                        )
                        current = tuple(
                            (directory / name).read_bytes()
                            for name, _ in artifacts
                        )
                        if baselines is None:
                            baselines = current
                        else:
                            self.assertEqual(current, baselines)
                        for name, kind in artifacts:
                            validate_stream(
                                (directory / name).read_text(
                                    encoding="ascii"
                                ),
                                kind,
                            )

    def test_multiblock_cfg_is_rejected_instead_of_linearized(self):
        program = """\
IR_PROGRAM_BEGIN("cfg-test");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(condition, "condition", IR_TYPE_BOOL);
IR_PARAM(value, "value", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_BRANCH(condition, yes, no);
IR_BLOCK_END();
IR_BLOCK_BEGIN(yes, "yes");
IR_RETURN(value);
IR_BLOCK_END();
IR_BLOCK_BEGIN(no, "no");
IR_RETURN(value);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-cfg-rejection-"
        ) as raw:
            directory = Path(raw)
            self.write_files(
                directory, generate_ir("anf", [self.database])
            )
            (directory / "Program.i").write_text(
                program, encoding="ascii", newline="\n"
            )
            compiler = self.compilers[0]
            executable = self.compile_driver(compiler, directory)
            decoded = subprocess.run(
                [str(executable), "--stop-after", "decode"],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(decoded.returncode, 0, decoded.stderr)
            rejected = subprocess.run(
                [str(executable), "--stop-after", "schedule"],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("pipeline: schedule stage:", rejected.stderr)
            self.assertIn("CFG lowering is unavailable", rejected.stderr)
            self.assertFalse((directory / "MIR.i").exists())

            (directory / "CFGAPI.c").write_text(
                """\
#include "Pipeline.h"

#include <stdio.h>
#include <string.h>

static int rejects(
        int (*function)(FILE *, char *, size_t),
        const char *expected) {
    char diagnostic[256];
    FILE *output = tmpfile();
    int status;
    if (output == NULL)
        return 0;
    status = function(output, diagnostic, sizeof diagnostic);
    (void)fclose(output);
    return status != 0 && strstr(diagnostic, expected) != NULL;
}

int main(void) {
    return !(rejects(
                 archon_pipeline_emit_program,
                 "canonical Program CFG lowering is unavailable") &&
             rejects(
                 archon_pipeline_emit_mir,
                 "canonical MIR CFG lowering is unavailable"));
}
""",
                encoding="ascii",
                newline="\n",
            )
            api = directory / "cfg-api"
            subprocess.run(
                [
                    compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                    "-DARCHON_ISEL_NO_MAIN",
                    "-DARCHON_REGALLOC_NO_MAIN",
                    "-I.",
                    "IR.c",
                    "Graphs.c",
                    "Oracle.c",
                    "Rewrite.c",
                    "ISel.c",
                    "Pipeline.c",
                    "ISched.c",
                    "Regalloc.c",
                    "Emitter.c",
                    "CFGAPI.c",
                    "-o",
                    api.name,
                ],
                cwd=directory,
                check=True,
            )
            subprocess.run([str(api)], cwd=directory, check=True)

    def test_call_without_abi_facts_remains_a_selection_barrier(self):
        program = """\
IR_PROGRAM_BEGIN("abi-barrier");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(value, "value", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_CALL_BEGIN(result, IR_TYPE_I32, main, ARCHON_IR_EFFECT_CALL);
IR_ARG_VALUE(value);
IR_CALL_END();
IR_RETURN(result);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-abi-barrier-"
        ) as raw:
            directory = Path(raw)
            self.write_files(
                directory, generate_ir("anf", [self.database])
            )
            (directory / "Program.i").write_text(
                program, encoding="ascii", newline="\n"
            )
            executable = self.compile_driver(
                self.compilers[0], directory
            )
            rejected = subprocess.run(
                [
                    str(executable),
                    "--stop-after",
                    "isel",
                    "--keep-artifacts",
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("pipeline: isel stage:", rejected.stderr)
            self.assertIn(
                "ANF call requires validated ABI lowering",
                rejected.stderr,
            )
            self.assertTrue((directory / "MIR.i").is_file())
            self.assertFalse((directory / "InstrMap.i").exists())

    def test_spills_require_explicit_target_lowering(self):
        lines = [
            'IR_PROGRAM_BEGIN("spill-barrier");',
            'IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);',
            'IR_PARAM(base, "base", IR_TYPE_I32);',
            'IR_BLOCK_BEGIN(entry, "entry");',
        ]
        for index in range(40):
            lines.extend(
                (
                    f"IR_LET_BEGIN(v{index}, IR_TYPE_I32, "
                    "IR_PRIM_ADD, IR_EFFECT_NONE);",
                    "IR_ARG_VALUE(base);",
                    f"IR_ARG_INT(IR_TYPE_I32, {index});",
                    "IR_LET_END();",
                )
            )
        accumulator = "v0"
        for index in range(1, 40):
            result = f"acc{index}"
            lines.extend(
                (
                    f"IR_LET_BEGIN({result}, IR_TYPE_I32, "
                    "IR_PRIM_ADD, IR_EFFECT_NONE);",
                    f"IR_ARG_VALUE({accumulator});",
                    f"IR_ARG_VALUE(v{index});",
                    "IR_LET_END();",
                )
            )
            accumulator = result
        lines.extend(
            (
                f"IR_RETURN({accumulator});",
                "IR_BLOCK_END();",
                "IR_FUNCTION_END();",
                "IR_PROGRAM_END();",
            )
        )

        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-spill-barrier-"
        ) as raw:
            directory = Path(raw)
            self.write_files(
                directory, generate_ir("anf", [self.database])
            )
            (directory / "Program.i").write_text(
                "\n".join(lines) + "\n",
                encoding="ascii",
                newline="\n",
            )
            executable = self.compile_driver(
                self.compilers[0], directory
            )
            rejected = subprocess.run(
                [
                    str(executable),
                    "--stop-after",
                    "regalloc",
                    "--keep-artifacts",
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("pipeline: regalloc stage:", rejected.stderr)
            self.assertIn(
                "spills, tied operands, or implicit results require "
                "target-specific lowering",
                rejected.stderr,
            )
            self.assertFalse((directory / "Regallocated.i").exists())

    def test_tied_target_operands_require_explicit_lowering(self):
        program = """\
ARCHON_IR_PROGRAM_BEGIN(1, "gated")
ARCHON_IR_BLOCK(1, 0, "entry")
ARCHON_IR_NODE(
    1, 1, ARCHON_IR_OP_PARAMETER,
    ARCHON_IR_TYPE_UINT, 16, UINT64_C(0))
ARCHON_IR_NODE(
    2, 1, ARCHON_IR_OP_PDP11_ADC,
    ARCHON_IR_TYPE_UINT, 16, UINT64_C(0))
ARCHON_IR_INPUT(2, 1, 0)
ARCHON_IR_PROGRAM_END()
"""
        with tempfile.TemporaryDirectory(
            dir="/tmp/opencode", prefix="archon-tied-barrier-"
        ) as raw:
            directory = Path(raw)
            self.write_files(
                directory,
                generate_ir("gated", [compile_file(PDP11)]),
            )
            (directory / "Program.i").write_text(
                program, encoding="ascii", newline="\n"
            )
            executable = self.compile_driver(
                self.compilers[0], directory
            )
            rejected = subprocess.run(
                [
                    str(executable),
                    "--stop-after",
                    "isel",
                    "--keep-artifacts",
                ],
                cwd=directory,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("pipeline: isel stage:", rejected.stderr)
            self.assertIn(
                "tied operands require target-specific lowering",
                rejected.stderr,
            )
            self.assertFalse((directory / "InstrMap.i").exists())


if __name__ == "__main__":
    unittest.main()
