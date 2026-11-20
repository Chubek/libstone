"""Strict-C99 integration tests for CPS IR and instruction selection."""

from copy import deepcopy
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
from archon.irgen import anf, cps, isched, isel, oracle, scheduler


ROOT = Path(__file__).resolve().parents[1]
TOY8 = ROOT / "examples" / "toy8.arch"
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"


_ISEL_ANNOTATION_DRIVER = r"""#include "ISel.h"
#include "Oracle.h"
#include <assert.h>
#include <string.h>

int main(void) {
    ArchonOracleAnnotations annotations;
    ArchonOracleDiagnostic diagnostic;
    ArchonISelOperand operands[2];
    ArchonISelOperation operation;
    ArchonISelChoice choice;
    char message[128];

    archon_oracle_annotations_init(&annotations);
    archon_oracle_diagnostic_init(&diagnostic);
    assert(archon_oracle_annotations_load(
               "MIR.oracle.i", &annotations, &diagnostic) ==
           ARCHON_ORACLE_STATUS_OK);
    memset(operands, 0, sizeof operands);
    operands[0].kind = ARCHON_ISEL_OPERAND_VALUE;
    operands[0].width = 32u;
    operands[1] = operands[0];
    memset(&operation, 0, sizeof operation);
    operation.operation = "add";
    operation.width = 32u;
    operation.operands = operands;
    operation.operand_count = 2u;
    assert(archon_isel_select_operation_with_annotations(
               "RISCV32", &operation, &annotations, &choice,
               message, sizeof message) == ARCHON_ISEL_NO_MATCH);
    annotations.protocol_version = 0u;
    assert(archon_isel_select_operation_with_annotations(
               "RISCV32", &operation, &annotations, &choice,
               message, sizeof message) == ARCHON_ISEL_ORACLE_ERROR);
    archon_oracle_annotations_dispose(&annotations);
    return 0;
}
"""


_ISEL_ORACLE_WORKER = r"""#include "Oracle.h"
#include <stdio.h>
#include <string.h>

static ArchonOracleStatus annotate(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *context,
        ArchonOracleDiagnostic *diagnostic) {
    (void)diagnostic;
    if (!request || !response ||
            request->stage != ARCHON_ORACLE_STAGE_ISEL)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    if (context && *(const int *)context == 1) {
        static const unsigned char suffix[] = "/* worker transformed */\n";
        if (!request->artifact_data ||
                request->artifact_capacity < sizeof suffix - 1u ||
                request->artifact_size >
                    request->artifact_capacity - (sizeof suffix - 1u))
            return ARCHON_ORACLE_STATUS_LIMIT_EXCEEDED;
        memcpy(
            request->artifact_data + request->artifact_size,
            suffix, sizeof suffix - 1u);
        response->artifact_size =
            request->artifact_size + sizeof suffix - 1u;
        response->artifact_changed = 1;
        return ARCHON_ORACLE_STATUS_OK;
    }
    if (context && *(const int *)context == 2)
        return ARCHON_ORACLE_STATUS_OK;
    return archon_oracle_response_add_bool(
        response, "isel.pattern.0.deny", 1);
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status;
    int mode = 0;
    unsigned request, request_count = 1u;
    if (argc != 2 && argc != 3)
        return 2;
    if (argc == 3) {
        if (strcmp(argv[2], "transform") == 0)
            mode = 1;
        else if (strcmp(argv[2], "passthrough") == 0) {
            mode = 2;
            request_count = 2u;
        } else
            return 2;
    }
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 1u, 0u, &diagnostic);
    for (request = 0u;
            request < request_count &&
                status == ARCHON_ORACLE_STATUS_OK;
            ++request)
        status = archon_oracle_server_serve_once(
            &server, annotate, &mode, &diagnostic);
    if (status != ARCHON_ORACLE_STATUS_OK)
        (void)fprintf(stderr, "%s\n", diagnostic.message);
    archon_oracle_server_dispose(&server);
    return status == ARCHON_ORACLE_STATUS_OK ? 0 : 1;
}
"""


class IRGenISel(unittest.TestCase):
    def compiler(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        return cc

    def compile(self, root, *sources, defines=()):
        executable = root / "test"
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
                executable.name,
            ],
            cwd=root,
            check=True,
        )
        return executable

    def write_files(self, root, files):
        for name, contents in files.items():
            (root / name).write_text(contents, encoding="ascii")

    def test_oracle_option_and_state_explicit_influence_api(self):
        rv32 = compile_file(RV32)
        for value in (0, 1, "false", None):
            with self.subTest(invalid_oracle_option=value):
                with self.assertRaises(ArchonError):
                    isel.emit({
                        "databases": [rv32],
                        "options": {"oracle": value},
                    })
        duplicate = deepcopy(
            next(row for row in rv32["instructions"] if row["name"] == "ADD")
        )
        duplicate["id"] = "instruction:ZADD"
        duplicate["name"] = "ZADD"
        duplicate["assembly"] = "zadd {rd}, {rs1}, {rs2}"
        rv32["instructions"].append(duplicate)

        enabled = isel.emit({"databases": [rv32]})
        disabled = isel.emit({
            "databases": [rv32],
            "options": {"oracle": False},
        })
        self.assertIn('#include "Oracle.h"', enabled["ISel.c"])
        self.assertIn(
            "archon_isel_select_operation_with_annotations",
            enabled["ISel.h"],
        )
        self.assertNotIn("Oracle.h", disabled["ISel.c"])
        self.assertNotIn("archon_oracle_", disabled["ISel.c"])

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, disabled)
            (root / "main.c").write_text(
                r"""#include "ISel.h"
#include <assert.h>
#include <string.h>

static enum ArchonISelStatus influence(
        void *context, const char *architecture, unsigned pattern_id,
        const char *target, int *deny, int *cost,
        char *message, size_t message_capacity) {
    (void)context;
    (void)architecture;
    (void)pattern_id;
    (void)message;
    (void)message_capacity;
    *deny = 0;
    *cost = strcmp(target, "ZADD") == 0 ? -1 : 0;
    return ARCHON_ISEL_OK;
}

static enum ArchonISelStatus deny_all(
        void *context, const char *architecture, unsigned pattern_id,
        const char *target, int *deny, int *cost,
        char *message, size_t message_capacity) {
    (void)context;
    (void)architecture;
    (void)pattern_id;
    (void)target;
    (void)message;
    (void)message_capacity;
    *deny = 1;
    *cost = 0;
    return ARCHON_ISEL_OK;
}

int main(void) {
    ArchonISelOperand registers[2];
    ArchonISelOperand immediate[2];
    ArchonISelOperation operation;
    ArchonISelChoice choice;
    char message[128];
    memset(registers, 0, sizeof registers);
    registers[0].kind = ARCHON_ISEL_OPERAND_VALUE;
    registers[0].width = 32u;
    registers[1] = registers[0];
    memset(&operation, 0, sizeof operation);
    operation.operation = "add";
    operation.width = 32u;
    operation.operands = registers;
    operation.operand_count = 2u;
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, influence, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_OK);
    assert(strcmp(choice.target, "ZADD") == 0);
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, deny_all, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_NO_MATCH);

    /* Influence cannot bypass effects or operand constraints. */
    operation.effects = 1u;
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, influence, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_NO_MATCH);
    operation.operation = NULL;
    operation.explicit_target = "ADD";
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, deny_all, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_NO_MATCH);
    operation.operation = "add";
    operation.explicit_target = NULL;
    operation.effects = 0u;
    immediate[0] = registers[0];
    memset(&immediate[1], 0, sizeof immediate[1]);
    immediate[1].kind = ARCHON_ISEL_OPERAND_CONSTANT;
    immediate[1].width = 32u;
    immediate[1].is_signed = 1u;
    immediate[1].bits = 7u;
    operation.operands = immediate;
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, influence, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_OK);
    assert(strcmp(choice.target, "ADDI") == 0);
    immediate[1].bits = 4096u;
    assert(archon_isel_select_operation_with_influence(
               "RISCV32", &operation, influence, NULL, &choice,
               message, sizeof message) == ARCHON_ISEL_NO_MATCH);
    return 0;
}
""",
                encoding="ascii",
            )
            executable = self.compile(
                root, "ISel.c", "main.c",
                defines=("ARCHON_ISEL_NO_MAIN",),
            )
            subprocess.run([executable], cwd=root, check=True)

    def test_oracle_sidecar_denies_only_an_otherwise_legal_pattern(self):
        rv32 = compile_file(RV32)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, oracle.emit({"options": {"oracle": True}}))
            self.write_files(root, isel.emit({"databases": [rv32]}))
            (root / "MIR.i").write_text(
                """\
MIR_BEGIN(1)
ARCHON_MIR_VALUE(1, 32, 0, 0)
ARCHON_MIR_VALUE(2, 32, 0, 0)
MIR_NODE(3, "add", 32, 0, 0)
MIR_OPERAND(3, 1)
MIR_OPERAND(3, 2)
MIR_ROOT(3)
MIR_END()
""",
                encoding="ascii",
            )
            (root / "MIR.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 3)
ARCHON_ORACLE_BOOL("isel.pattern.0.deny", 1)
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            executable = root / "isel"
            subprocess.run(
                [
                    *self.compiler(),
                    "-D_POSIX_C_SOURCE=200809L",
                    "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    '-DARCHON_MIR_FILE="MIR.i"',
                    "ISel.c", "Oracle.c", "-lrt", "-o", executable.name,
                ],
                cwd=root,
                check=True,
            )
            result = subprocess.run(
                [executable, "RISCV32", "Denied.i"],
                cwd=root,
                text=True,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("no legal instruction selection", result.stderr)
            self.assertFalse((root / "Denied.i").exists())
            (root / "linked.c").write_text(
                _ISEL_ANNOTATION_DRIVER,
                encoding="ascii",
            )
            subprocess.run(
                [
                    *self.compiler(),
                    "-D_POSIX_C_SOURCE=200809L",
                    "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-DARCHON_ISEL_NO_MAIN",
                    "ISel.c", "Oracle.c", "linked.c",
                    "-lrt", "-o", "isel-linked",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run(
                [str(root / "isel-linked")], cwd=root, check=True
            )
            (root / "MIR.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 3)
ARCHON_ORACLE_I64("isel.pattern.0.deny", 0)
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            wrong_type = subprocess.run(
                [executable, "RISCV32", "WrongType.i"],
                cwd=root,
                text=True,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(wrong_type.returncode, 1)
            self.assertIn("wrong type", wrong_type.stderr)
            self.assertFalse((root / "WrongType.i").exists())
            (root / "MIR.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 3)
ARCHON_ORACLE_BOOL("isel.pattern.0.deny", 0)
ARCHON_ORACLE_I64("isel.pattern.0.cost", 1048577)
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            out_of_range = subprocess.run(
                [executable, "RISCV32", "OutOfRange.i"],
                cwd=root,
                text=True,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(out_of_range.returncode, 1)
            self.assertIn("outside the bounded range", out_of_range.stderr)
            self.assertFalse((root / "OutOfRange.i").exists())
            if os.name != "posix" or not Path("/dev/shm").is_dir():
                return
            (root / "worker.c").write_text(
                _ISEL_ORACLE_WORKER,
                encoding="ascii",
            )
            subprocess.run(
                [
                    *self.compiler(),
                    "-D_POSIX_C_SOURCE=200809L",
                    "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "Oracle.c", "worker.c", "-lrt", "-o", "worker",
                ],
                cwd=root,
                check=True,
            )
            (root / "MIR.oracle.i").unlink()
            environment = os.environ.copy()
            passthrough_socket = root / "passthrough.sock"
            worker = subprocess.Popen(
                [
                    str(root / "worker"),
                    str(passthrough_socket),
                    "passthrough",
                ],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                for _ in range(500):
                    if passthrough_socket.exists() or worker.poll() is not None:
                        break
                    time.sleep(0.01)
                self.assertTrue(passthrough_socket.exists())
                environment["ARCHON_ORACLE_SOCKET"] = str(
                    passthrough_socket
                )
                selected = subprocess.run(
                    [executable, "RISCV32", "WorkerSelected.i"],
                    cwd=root,
                    env=environment,
                    text=True,
                    stderr=subprocess.PIPE,
                )
                stdout, stderr = worker.communicate(timeout=10)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            self.assertEqual(selected.returncode, 0, selected.stderr)
            self.assertTrue((root / "WorkerSelected.i").exists())
            self.assertTrue((root / "WorkerSelected.oracle.i").exists())
            self.assertTrue((root / "MIR.oracle.i").exists())

            (root / "MIR.oracle.i").unlink()
            socket_path = root / "oracle.sock"
            worker = subprocess.Popen(
                [str(root / "worker"), str(socket_path)],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                for _ in range(500):
                    if socket_path.exists() or worker.poll() is not None:
                        break
                    time.sleep(0.01)
                self.assertTrue(socket_path.exists())
                environment["ARCHON_ORACLE_SOCKET"] = str(socket_path)
                result = subprocess.run(
                    [executable, "RISCV32", "WorkerDenied.i"],
                    cwd=root,
                    env=environment,
                    text=True,
                    stderr=subprocess.PIPE,
                )
                stdout, stderr = worker.communicate(timeout=10)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            self.assertEqual(result.returncode, 1)
            self.assertIn("no legal instruction selection", result.stderr)
            self.assertFalse((root / "WorkerDenied.i").exists())
            self.assertTrue((root / "MIR.oracle.i").exists())

            transform_socket = root / "transform.sock"
            worker = subprocess.Popen(
                [
                    str(root / "worker"),
                    str(transform_socket),
                    "transform",
                ],
                cwd=root,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                for _ in range(500):
                    if transform_socket.exists() or worker.poll() is not None:
                        break
                    time.sleep(0.01)
                self.assertTrue(transform_socket.exists())
                environment["ARCHON_ORACLE_SOCKET"] = str(transform_socket)
                transformed = subprocess.run(
                    [executable, "RISCV32", "Transformed.i"],
                    cwd=root,
                    env=environment,
                    text=True,
                    stderr=subprocess.PIPE,
                )
                stdout, stderr = worker.communicate(timeout=10)
            finally:
                if worker.poll() is None:
                    worker.kill()
                    worker.communicate()
            self.assertEqual(worker.returncode, 0, stdout + stderr)
            self.assertEqual(transformed.returncode, 1)
            self.assertIn("transformed compiled-in MIR", transformed.stderr)
            self.assertFalse((root / "Transformed.i").exists())
            self.assertFalse((root / "MIR.oracle.i").exists())
            self.assertIn(
                "worker transformed",
                (root / "MIR.i").read_text(encoding="ascii"),
            )

    def test_cps_program_macro_decoder_call_tokens_and_printer(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, cps.emit_ir(None))
            (root / "Program.i").write_text(
                """\
CPS_PROGRAM_BEGIN(1)
CPS_TYPE(1, ARCHON_CPS_TYPE_BITS, 32, 0)
CPS_TYPE(2, ARCHON_CPS_TYPE_EFFECT, 0, 0)
CPS_TYPE(3, ARCHON_CPS_TYPE_CONTROL, 0, 0)
CPS_SIGNATURE_BEGIN(1, 1, 2, 3)
CPS_SIGNATURE_PARAMETER(1)
CPS_SIGNATURE_PARAMETER(2)
CPS_SIGNATURE_PARAMETER(3)
CPS_SIGNATURE_END()
CPS_TYPE(4, ARCHON_CPS_TYPE_CONTINUATION, 0, 1)
CPS_FUNCTION(1, "callee", 1, 1)
CPS_FUNCTION(2, "caller", 1, 2)
CPS_BLOCK(1, 1, 1)
CPS_PARAMETER(1, 1, 1)
CPS_PARAMETER(2, 1, 2)
CPS_PARAMETER(3, 1, 3)
CPS_NODE_BEGIN(1, 0, 1, ARCHON_CPS_OP_RETURN, 0, 0, 0, 0, 0, 2, 2, 3, 3)
CPS_NODE_OPERAND(1)
CPS_NODE_END()
CPS_BLOCK(2, 2, 1)
CPS_PARAMETER(4, 2, 1)
CPS_PARAMETER(5, 2, 2)
CPS_PARAMETER(6, 2, 3)
CPS_NODE_BEGIN(2, 0, 2, ARCHON_CPS_OP_CALL, 0, 3, 0, 1, 0, 5, 5, 6, 6)
CPS_NODE_OPERAND(4)
CPS_NODE_OPERAND(5)
CPS_NODE_OPERAND(6)
CPS_NODE_END()
CPS_BLOCK(3, 2, 1)
CPS_PARAMETER(7, 3, 1)
CPS_PARAMETER(8, 3, 2)
CPS_PARAMETER(9, 3, 3)
CPS_NODE_BEGIN(3, 0, 3, ARCHON_CPS_OP_TAIL_CALL, 0, 0, 0, 1, 0, 8, 8, 9, 9)
CPS_NODE_OPERAND(7)
CPS_NODE_OPERAND(8)
CPS_NODE_OPERAND(9)
CPS_NODE_END()
CPS_PROGRAM_END()
""",
                encoding="ascii",
            )
            (root / "main.c").write_text(
                """\
#include "IR.h"
#include <assert.h>
#include <string.h>

static void check_forward_continuations(void) {
    struct ArchonCPSProgram program;
    struct ArchonCPSBuilder builder;
    ArchonCPSTypeId bit;
    ArchonCPSSignatureId entry_signature, target_signature;
    ArchonCPSFunctionId function;
    ArchonCPSBlockId entry, yes_block, no_block;
    ArchonCPSValueId condition;
    char message[128] = {0};

    archon_cps_program_init(&program);
    assert(archon_cps_add_type(
               &program, ARCHON_CPS_TYPE_BITS, 1, 0, &bit) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_signature(
               &program, &bit, 1, 0, 0, 0, &entry_signature) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_signature(
               &program, NULL, 0, 0, 0, 0, &target_signature) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_function(
               &program, "choose", entry_signature, 1, &function) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_block(
               &program, function, entry_signature, &entry) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_block(
               &program, function, target_signature, &yes_block) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_block(
               &program, function, target_signature, &no_block) ==
           ARCHON_CPS_OK);
    assert(archon_cps_add_parameter(
               &program, entry, bit, &condition) == ARCHON_CPS_OK);

    archon_cps_builder_init(&builder, &program);
    assert(archon_cps_builder_position(
               &builder, function, entry, 0, 0) == ARCHON_CPS_OK);
    assert(archon_cps_builder_branch(
               &builder, condition, yes_block, no_block) == ARCHON_CPS_OK);
    assert(archon_cps_builder_position(
               &builder, function, yes_block, 0, 0) == ARCHON_CPS_OK);
    assert(archon_cps_builder_return(&builder, 0) == ARCHON_CPS_OK);
    assert(archon_cps_builder_position(
               &builder, function, no_block, 0, 0) == ARCHON_CPS_OK);
    assert(archon_cps_builder_return(&builder, 0) == ARCHON_CPS_OK);
    assert(archon_cps_verify(&program, message, sizeof message) ==
           ARCHON_CPS_OK);
    archon_cps_program_dispose(&program);
}

int main(void) {
    struct ArchonCPSProgram program;
    char message[128] = {0};
    FILE *output = tmpfile();
    assert(output != NULL);
    assert(archon_cps_decode_program(&program, message, sizeof message) ==
           ARCHON_CPS_OK);
    assert(program.type_count == 4 && program.function_count == 2);
    assert(program.blocks[0].continuation_signature == 1);
    assert(program.nodes[1].opcode == ARCHON_CPS_OP_CALL);
    assert(program.nodes[2].opcode == ARCHON_CPS_OP_TAIL_CALL);
    assert(archon_cps_print(output, &program) == ARCHON_CPS_OK);
    assert(ftell(output) > 0);
    fclose(output);
    archon_cps_program_dispose(&program);
    check_forward_continuations();
    return 0;
}
""",
                encoding="ascii",
            )
            executable = self.compile(
                root,
                "IR.c",
                "main.c",
                defines=('ARCHON_CPS_PROGRAM_FILE="Program.i"',),
            )
            subprocess.run([executable], check=True)

    def test_cps_runtime_is_standalone_without_program_stream(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(root, cps.emit_ir(None))
            (root / "main.c").write_text(
                """\
#include "IR.h"
#include <assert.h>

int main(void) {
    struct ArchonCPSProgram program;
    char message[64] = {0};
    assert(archon_cps_decode_program(
               &program, message, sizeof message) ==
           ARCHON_CPS_PROGRAM_UNAVAILABLE);
    return 0;
}
""",
                encoding="ascii",
            )
            executable = self.compile(root, "IR.c", "main.c")
            subprocess.run([executable], check=True)

    def _run_selector(self, db, mir, expected):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                isel.emit({
                    "databases": [db],
                    "options": {"oracle": False},
                }),
            )
            (root / "MIR.i").write_text(mir, encoding="ascii")
            executable = self.compile(
                root,
                "ISel.c",
                defines=('ARCHON_MIR_FILE="MIR.i"',),
            )
            result = subprocess.run(
                [executable, db["architecture"]],
                check=True,
                text=True,
                stdout=subprocess.PIPE,
            )
            self.assertIn(expected, result.stdout)
            self.assertIn(
                f'ARCHON_INSTR_MAP_BEGIN("{db["architecture"]}")',
                result.stdout,
            )
            return result.stdout

    def test_toy8_and_riscv_semantic_patterns_select(self):
        toy = compile_file(TOY8)
        output = self._run_selector(
            toy,
            """\
MIR_BEGIN(1)
ARCHON_MIR_VALUE(1, 8, 0, 11)
ARCHON_MIR_VALUE(2, 8, 0, 11)
MIR_NODE(3, "add", 8, 0, 11)
MIR_OPERAND(3, 1)
MIR_OPERAND(3, 2)
MIR_ROOT(3)
MIR_END()
""",
            '"ADD"',
        )
        self.assertIn("2u, 0u, 11u)", output)  # size, effects, definedness

        rv32 = compile_file(RV32)
        output = self._run_selector(
            rv32,
            """\
MIR_SCHEMA(2)
MIR_PROGRAM_BEGIN("canonical-scheduler-stream")
MIR_TARGET(0, "RISCV32", 32)
MIR_TARGET_INSTRUCTION(0, 0, "ADD", 4, 1, 0, 0, 0, 0, "", 0)
MIR_TARGET_INSTRUCTION(0, 1, "ADDI", 4, 1, 0, 0, 0, 0, "", 0)
MIR_FUNCTION_BEGIN(0, "main", "main", MIR_TYPE_I32, 32)
MIR_VALUE(0, 0, "x", "x", MIR_TYPE_I32, 32, 1)
MIR_VALUE(0, 1, "sum", "sum", MIR_TYPE_I32, 32, 0)
MIR_BLOCK_BEGIN(0, 0, "entry", "entry")
MIR_CANDIDATE_SET_BEGIN(0, MIR_OPCODE_ADD, 2)
MIR_CANDIDATE(0, 0, 0, 0, "RISCV32", "ADD",
              4, 1, 0, 0, 0, 0, "", 0)
MIR_CANDIDATE(0, 1, 0, 1, "RISCV32", "ADDI",
              4, 1, 0, 0, 0, 0, "", 0)
MIR_CANDIDATE_SET_END(0)
MIR_OP(0, 0, 0, 0, 1, MIR_OPCODE_ADD, 1, MIR_EFFECT_NONE, 0)
MIR_INPUT_VALUE(0, 0, 0)
MIR_INPUT_INTEGER(0, 1, MIR_TYPE_I32, 32, UINT64_C(0xffffffff))
MIR_CANDIDATE_SET_BEGIN(1, MIR_OPCODE_RETURN, 0)
MIR_CANDIDATE_SET_END(1)
MIR_OP(1, 0, 0, 1, MIR_NO_VALUE, MIR_OPCODE_RETURN, 1,
       MIR_EFFECT_CONTROL, 1)
MIR_INPUT_VALUE(1, 0, 1)
MIR_DEPENDENCY(1, 0, MIR_DEP_DATA | MIR_DEP_CONTROL)
MIR_BLOCK_END(0, 0)
MIR_FUNCTION_END(0)
MIR_PROGRAM_END()
""",
            '"ADDI"',
        )
        self.assertNotIn('"ADD",', output)
        self.assertIn('"MIR_OPCODE_RETURN"', output)
        self.assertIn("ISCHED_MIR_DEP(0u, 1u", output)

    def test_in_memory_egraph_operation_api_uses_semantic_patterns(self):
        rv32 = compile_file(RV32)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.write_files(
                root,
                isel.emit({
                    "databases": [rv32],
                    "options": {"oracle": False},
                }),
            )
            (root / "main.c").write_text(
                """\
#include "ISel.h"
#include <assert.h>
#include <string.h>

int main(void) {
    ArchonISelOperand operands[2];
    ArchonISelOperation operation;
    ArchonISelChoice choice;
    char message[128];
    memset(&operation, 0, sizeof operation);
    memset(operands, 0, sizeof operands);
    operands[0].kind = ARCHON_ISEL_OPERAND_VALUE;
    operands[0].width = 32u;
    operands[1].kind = ARCHON_ISEL_OPERAND_CONSTANT;
    operands[1].width = 32u;
    operands[1].is_signed = 1u;
    operands[1].bits = 5u;
    operation.operation = "add";
    operation.width = 32u;
    operation.operands = operands;
    operation.operand_count = 2u;
    assert(archon_isel_select_operation(
               "RISCV32", &operation, &choice,
               message, sizeof message) == ARCHON_ISEL_OK);
    assert(strcmp(choice.target, "ADDI") == 0);
    assert(choice.encoding_bytes == 4u);
    return 0;
}
""",
                encoding="ascii",
            )
            executable = self.compile(
                root,
                "ISel.c",
                "main.c",
                defines=("ARCHON_ISEL_NO_MAIN",),
            )
            subprocess.run([executable], check=True)

    def test_multiple_targets_default_all_and_explicit_only_pattern(self):
        toy = compile_file(TOY8)
        rv32 = compile_file(RV32)
        files = isel.emit({
            "databases": [rv32, toy],
            "options": {"oracle": False},
        })
        self.assertIn("ISelPattern_Toy8.i", files)
        self.assertIn("ISelPattern_RISCV32.i", files)

        encoding_only = compile_file(TOY8)
        add = next(row for row in encoding_only["instructions"] if row["name"] == "ADD")
        add["semantics"] = None
        patterns = isel.emit({
            "databases": [encoding_only],
            "options": {"oracle": False},
        })[
            "ISelPattern_Toy8.i"
        ]
        self.assertIn('"target:ADD"', patterns)
        self.assertNotIn('"add"', patterns)
        explicit = self._run_selector(
            encoding_only,
            """\
MIR_BEGIN(1)
ARCHON_MIR_VALUE(1, 8, 0, 0)
ARCHON_MIR_VALUE(2, 8, 0, 0)
ARCHON_MIR_TARGET(3, "ADD", 8, 0, 0)
MIR_OPERAND(3, 1)
MIR_OPERAND(3, 2)
MIR_ROOT(3)
MIR_END()
""",
            '"ADD"',
        )
        self.assertNotIn('"add"', explicit)

        selected = isel.emit({
            "databases": [toy, rv32],
            "selected_architectures": ["Toy8"],
            "options": {"oracle": False},
        })
        self.assertIn("ISelPattern_Toy8.i", selected)
        self.assertNotIn("ISelPattern_RISCV32.i", selected)

    def test_scheduler_machine_ir_v2_flows_into_selector(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rv32 = compile_file(RV32)
            self.write_files(root, anf.emit_ir(None))
            self.write_files(
                root,
                scheduler.emit({
                    "architectures": [rv32],
                    "options": {"oracle": False},
                }),
            )
            self.write_files(
                root,
                isel.emit({
                    "databases": [rv32],
                    "options": {"oracle": False},
                }),
            )
            self.write_files(
                root,
                isched.emit({
                    "databases": [rv32],
                    "options": {"oracle": False},
                }),
            )
            (root / "Program.i").write_text(
                """\
IR_PROGRAM_BEGIN("scheduler-to-isel");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(left, "left", IR_TYPE_I32);
IR_PARAM(right, "right", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(sum, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_VALUE(right);
IR_LET_END();
IR_RETURN(sum);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
""",
                encoding="ascii",
            )
            (root / "lower.c").write_text(
                """\
#include "Scheduler.h"
#include <assert.h>

int main(void) {
    char message[128] = {0};
    FILE *output = fopen("MIR.i", "w");
    assert(output != NULL);
    assert(archon_scheduler_emit_embedded(
               output, NULL, message, sizeof message) ==
           ARCHON_SCHEDULER_OK);
    assert(fclose(output) == 0);
    return 0;
}
""",
                encoding="ascii",
            )
            lower = self.compile(
                root,
                "IR.c",
                "Scheduler.c",
                "lower.c",
                defines=('ARCHON_IR_PROGRAM_FILE="Program.i"',),
            )
            subprocess.run([lower], cwd=root, check=True)
            mir = (root / "MIR.i").read_text(encoding="ascii")
            self.assertIn("MIR_SCHEMA(2)", mir)
            self.assertIn("MIR_OP(0, 0, 0, 0, 2, MIR_OPCODE_ADD", mir)

            selector = self.compile(
                root,
                "ISel.c",
                defines=('ARCHON_MIR_FILE="MIR.i"',),
            )
            subprocess.run(
                [selector, "RISCV32", "InstrMap.i"],
                cwd=root,
                check=True,
            )
            selected = (root / "InstrMap.i").read_text(encoding="ascii")
            self.assertIn('ARCHON_INSTR(1u, 0u, "ADD"', selected)
            self.assertIn('ISCHED_INSTRUCTION("ADD"', selected)
            self.assertIn("ISCHED_MIR_DEP(0u, 1u", selected)

            instruction_scheduler = self.compile(
                root,
                "ISched.c",
                defines=("ISCHED_STANDALONE",),
            )
            subprocess.run(
                [
                    instruction_scheduler,
                    "MIR.i",
                    "InstrMap.i",
                    "Scheduled.i",
                    "RegMap.i",
                ],
                cwd=root,
                check=True,
            )
            scheduled = (root / "Scheduled.i").read_text(encoding="ascii")
            self.assertIn('ISCHED_SCHEDULE_OP(1, 0, 0, 0, "ADD")', scheduled)

    def test_malformed_mir_is_rejected_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            toy = compile_file(TOY8)
            self.write_files(
                root,
                isel.emit({
                    "databases": [toy],
                    "options": {"oracle": False},
                }),
            )
            # Operand 2 is forward/undefined and the stream has no root.
            (root / "MIR.i").write_text(
                """\
MIR_BEGIN(1)
MIR_NODE(1, "add", 8, 0, 0)
MIR_OPERAND(1, 2)
MIR_END()
""",
                encoding="ascii",
            )
            (root / "main.c").write_text(
                """\
#include "ISel.h"
#include <assert.h>

int main(void) {
    char message[80] = {0};
    FILE *output = tmpfile();
    enum ArchonISelStatus status;
    assert(output != NULL);
    status = archon_isel_run(output, "Toy8", message, sizeof message);
    assert(status == ARCHON_ISEL_MALFORMED_MIR);
    assert(ftell(output) == 0);
    fclose(output);
    return 0;
}
""",
                encoding="ascii",
            )
            executable = self.compile(
                root,
                "ISel.c",
                "main.c",
                defines=(
                    "ARCHON_ISEL_NO_MAIN",
                    'ARCHON_MIR_FILE="MIR.i"',
                ),
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
