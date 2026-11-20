"""Strict-C99 coverage for typed TAC and SAT modulo scheduling."""

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
from archon.irgen import generate_ir, isched, oracle, typed_tac


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"


def _compilers():
    commands = []
    requested = shlex.split(os.environ.get("CC", "cc"))
    candidates = [requested, ["gcc"], ["clang"]]
    seen = set()
    for command in candidates:
        if not command:
            continue
        executable = shutil.which(command[0])
        if executable and executable not in seen:
            commands.append([executable, *command[1:]])
            seen.add(executable)
    return commands


def _write(root, files):
    for name, text in files.items():
        (root / name).write_text(text, encoding="ascii", newline="\n")


class IRGenISched(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rv32 = compile_file(RV32)

    def test_contract_conservative_models_and_deterministic_names(self):
        self.assertEqual(typed_tac.NAME, "typed-tac")
        self.assertIn("ttac", typed_tac.ALIASES)
        typed = typed_tac.emit_ir({"databases": [self.rv32]})
        self.assertEqual(set(typed), {"IR.c", "IR.h"})
        self.assertIn("IR_OP_PHI", typed["IR.h"])
        self.assertIn("ir_build_call", typed["IR.h"])
        self.assertIn("ir_load_program_i", typed["IR.h"])
        self.assertIn("IR_PROGRAM_BEGIN", typed["IR.c"])

        context = {
            "databases": [self.rv32],
            "options": {"oracle": False},
        }
        first = isched.emit(context)
        second = isched.emit(context)
        self.assertEqual(first, second)
        self.assertEqual(
            set(first), {"ISched.c", "ISched.h", "ISched_RISCV32.i"}
        )
        model = first["ISched_RISCV32.i"]
        self.assertIn("CONSERVATIVE MODEL", model)
        self.assertIn("capacity-one resource", model)
        self.assertIn("ISCHED_MODEL_CONSERVATIVE", model)
        self.assertIn('ISCHED_REGISTER_BANK(0, "conservative-unspecified", 0', model)
        self.assertNotIn("pipeline", model.lower())

        odd = dict(self.rv32)
        odd["architecture"] = "9 / odd--cpu"
        self.assertIn(
            "ISched__9_odd_cpu.i", isched.emit({"architectures": [odd]})
        )
        collision = dict(self.rv32)
        collision["architecture"] = "9___odd cpu"
        with self.assertRaises(ArchonError):
            isched.emit({
                "databases": [odd, collision],
                "options": {"oracle": False},
            })

    def test_oracle_option_and_linked_known_fact_influence_api(self):
        for value in (0, 1, "false", None):
            with self.subTest(invalid_oracle_option=value):
                with self.assertRaises(ArchonError):
                    isched.emit({
                        "databases": [self.rv32],
                        "options": {"oracle": value},
                    })
        enabled = isched.emit({"databases": [self.rv32]})
        disabled = isched.emit({
            "databases": [self.rv32],
            "options": {"oracle": False},
        })
        self.assertIn('#include "Oracle.h"', enabled["ISched.c"])
        self.assertIn("isched_schedule_with_annotations", enabled["ISched.h"])
        self.assertNotIn("Oracle.h", disabled["ISched.c"])
        self.assertNotIn("archon_oracle_", disabled["ISched.c"])

        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, disabled)
            (root / "influence.c").write_text(
                _ISCHED_INFLUENCE_DRIVER,
                encoding="ascii",
            )
            for index, compiler in enumerate(compilers):
                with self.subTest(compiler=compiler[0]):
                    executable = f"isched-influence-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic",
                            "ISched.c", "influence.c", "-o", executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run(
                        [str(root / executable)], cwd=root, check=True
                    )

    def test_oracle_annotations_drive_only_marked_known_facts(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, oracle.emit({"options": {"oracle": True}}))
            _write(root, isched.emit({"databases": [self.rv32]}))
            (root / "facts.oracle.i").write_text(
                """\
ARCHON_ORACLE_BEGIN(1, 4)
ARCHON_ORACLE_U64("isched.resource.0.capacity", 2)
ARCHON_ORACLE_I64("isched.operation.1.priority", 10)
ARCHON_ORACLE_END()
""",
                encoding="ascii",
            )
            (root / "annotations.c").write_text(
                _ISCHED_ANNOTATION_DRIVER,
                encoding="ascii",
            )
            compiler = compilers[0]
            subprocess.run(
                [
                    *compiler, "-D_POSIX_C_SOURCE=200809L",
                    "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "ISched.c", "Oracle.c", "annotations.c",
                    "-lrt", "-o", "isched-annotations",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run(
                [str(root / "isched-annotations")], cwd=root, check=True
            )

    def test_oracle_stream_sidecars_respect_knownness_and_bounds(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, oracle.emit({"options": {"oracle": True}}))
            _write(root, isched.emit({"databases": [self.rv32]}))
            compiler = compilers[0]
            subprocess.run(
                [
                    *compiler, "-D_POSIX_C_SOURCE=200809L",
                    "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                    "-DISCHED_STANDALONE", "ISched.c", "Oracle.c",
                    "-lrt", "-o", "isched",
                ],
                cwd=root,
                check=True,
            )

            def run_case(instrmap, mir, map_annotations, mir_annotations):
                (root / "InstrMap.i").write_text(instrmap, encoding="ascii")
                (root / "MIR.i").write_text(mir, encoding="ascii")
                (root / "InstrMap.oracle.i").write_text(
                    map_annotations, encoding="ascii"
                )
                (root / "MIR.oracle.i").write_text(
                    mir_annotations, encoding="ascii"
                )
                for name in ("Scheduled.i", "RegMap.i"):
                    (root / name).unlink(missing_ok=True)
                return subprocess.run(
                    [str(root / "isched")],
                    cwd=root,
                    text=True,
                    stderr=subprocess.PIPE,
                )

            empty = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                "ARCHON_ORACLE_END()\n"
            )
            capacity = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_U64("isched.resource.0.capacity", 2)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(_KNOWN_INSTR_MAP, _KNOWN_MIR, capacity, empty)
            self.assertEqual(result.returncode, 0, result.stderr)
            scheduled = (root / "Scheduled.i").read_text(encoding="ascii")
            self.assertIn("ISCHED_SCHEDULE_BEGIN(1, 2", scheduled)

            priority = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_I64("isched.operation.2.priority", 10)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(_KNOWN_INSTR_MAP, _KNOWN_MIR, empty, priority)
            self.assertEqual(result.returncode, 0, result.stderr)
            scheduled = (root / "Scheduled.i").read_text(encoding="ascii")
            self.assertIn("ISCHED_SCHEDULE_BEGIN(2, 2", scheduled)
            self.assertLess(
                scheduled.index('ISCHED_SCHEDULE_OP(2, 0, 0, 0, "B")'),
                scheduled.index('ISCHED_SCHEDULE_OP(1, 1, 1, 0, "A")'),
            )

            latency = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_U64("isched.resource.0.capacity", 2)\n'
                'ARCHON_ORACLE_U64("isched.operation.1.latency", 3)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(
                _KNOWN_INSTR_MAP + "ARCHON_INSTR_OPERAND(2, 7, 1)\n",
                _KNOWN_MIR,
                latency,
                empty,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            scheduled = (root / "Scheduled.i").read_text(encoding="ascii")
            self.assertIn('ISCHED_SCHEDULE_OP(2, 3, 0, 3, "B")', scheduled)

            ignored = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_U64("isched.resource.0.capacity", 2)\n'
                'ARCHON_ORACLE_U64("isched.operation.0.latency", 7)\n'
                "ARCHON_ORACLE_END()\n"
            )
            unknown_priority = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_I64("isched.operation.1.priority", 10)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(
                _CONSERVATIVE_INSTR_MAP,
                _CONSERVATIVE_MIR,
                ignored,
                unknown_priority,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            scheduled = (root / "Scheduled.i").read_text(encoding="ascii")
            self.assertIn("ISCHED_SCHEDULE_BEGIN(2, 2", scheduled)
            self.assertLess(
                scheduled.index('ISCHED_SCHEDULE_OP(0,'),
                scheduled.index('ISCHED_SCHEDULE_OP(1,'),
            )

            wrong_type = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_BOOL("isched.resource.0.capacity", 1)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(
                _KNOWN_INSTR_MAP, _KNOWN_MIR, wrong_type, empty
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("type mismatch", result.stderr)
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())

            wrong_stage = (
                "ARCHON_ORACLE_BEGIN(1, 3)\n"
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(
                _KNOWN_INSTR_MAP, _KNOWN_MIR, wrong_stage, empty
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("wrong version or stage", result.stderr)
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())

            invalid_priority = (
                "ARCHON_ORACLE_BEGIN(1, 4)\n"
                'ARCHON_ORACLE_I64("isched.operation.1.priority", 32768)\n'
                "ARCHON_ORACLE_END()\n"
            )
            result = run_case(
                _KNOWN_INSTR_MAP, _KNOWN_MIR, empty, invalid_priority
            )
            self.assertEqual(result.returncode, 1)
            self.assertIn("limit exceeded", result.stderr)
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())

            (root / "InstrMap.i").unlink()
            result = subprocess.run(
                [str(root / "isched")], cwd=root, capture_output=True, text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())

    def test_oracle_stream_failure_diagnoses_and_removes_outputs(self):
        if os.name != "posix" or not Path("/dev/shm").is_dir():
            self.skipTest("Oracle runtime checks require POSIX shared memory")
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, oracle.emit({"options": {"oracle": True}}))
            _write(root, isched.emit({"databases": [self.rv32]}))
            (root / "InstrMap.i").write_text(
                _KNOWN_INSTR_MAP, encoding="ascii"
            )
            (root / "MIR.i").write_text(_KNOWN_MIR, encoding="ascii")
            (root / "worker.c").write_text(
                _ISCHED_ORACLE_WORKER, encoding="ascii"
            )
            compiler = compilers[0]
            common = [
                *compiler, "-D_POSIX_C_SOURCE=200809L",
                "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
            ]
            subprocess.run(
                [
                    *common, "-DISCHED_STANDALONE",
                    "ISched.c", "Oracle.c", "-lrt", "-o", "isched",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run(
                [
                    *common, "Oracle.c", "worker.c",
                    "-lrt", "-o", "worker",
                ],
                cwd=root,
                check=True,
            )
            def invoke_worker(name, *arguments):
                socket_path = root / name
                worker = subprocess.Popen(
                    [
                        str(root / "worker"),
                        str(socket_path),
                        *arguments,
                    ],
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
                    environment = os.environ.copy()
                    environment["ARCHON_ORACLE_SOCKET"] = str(socket_path)
                    result = subprocess.run(
                        [str(root / "isched")],
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
                return result

            transformed = invoke_worker("transform.sock", "success")
            self.assertEqual(transformed.returncode, 0, transformed.stderr)
            self.assertIn(
                'ISCHED_RESOURCE(0, "alu", 2, 0)',
                (root / "InstrMap.i").read_text(encoding="ascii"),
            )
            self.assertIn(
                "ISCHED_SCHEDULE_BEGIN(1, 2",
                (root / "Scheduled.i").read_text(encoding="ascii"),
            )

            (root / "InstrMap.i").write_text(
                _KNOWN_INSTR_MAP, encoding="ascii"
            )
            (root / "MIR.i").write_text(_KNOWN_MIR, encoding="ascii")
            result = invoke_worker("failure.sock")
            self.assertEqual(result.returncode, 1)
            self.assertIn("forced RegMap failure", result.stderr)
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())

    def test_typed_tac_build_verify_print_and_program_i_roundtrip(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, typed_tac.emit_ir(None))
            (root / "producer.c").write_text(_TYPED_PRODUCER, encoding="ascii")
            (root / "consumer.c").write_text(_TYPED_CONSUMER, encoding="ascii")
            for index, compiler in enumerate(compilers):
                with self.subTest(compiler=compiler[0]):
                    producer = f"typed-producer-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", "IR.c", "producer.c",
                            "-o", producer,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run([str(root / producer)], cwd=root, check=True)
                    stream = (root / "Program.i").read_text(encoding="ascii")
                    self.assertIn("IR_OP(", stream)
                    self.assertIn("IR_INPUT(", stream)
                    self.assertIn("IR_TARGETS(", stream)
                    consumer = f"typed-consumer-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic",
                            '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                            "IR.c", "consumer.c", "-o", consumer,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run([str(root / consumer)], cwd=root, check=True)

    def test_sat_unsat_next_unknown_recurrence_resources_and_pressure(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(
                root,
                isched.emit({
                    "databases": [self.rv32],
                    "options": {"oracle": False},
                }),
            )
            (root / "test.c").write_text(_ISCHED_DRIVER, encoding="ascii")
            (root / "InstrMap.i").write_text(_INSTR_MAP, encoding="ascii")
            (root / "MIR.i").write_text(_MIR, encoding="ascii")
            standalone_baseline = None
            for index, compiler in enumerate(compilers):
                with self.subTest(compiler=compiler[0]):
                    executable = f"isched-test-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", "ISched.c", "test.c",
                            "-o", executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run([str(root / executable)], cwd=root, check=True)
                    scheduled = (root / "StreamScheduled.i").read_text(
                        encoding="ascii"
                    )
                    regmap = (root / "StreamRegMap.i").read_text(encoding="ascii")
                    self.assertIn("ISCHED_SCHEDULE_BEGIN(3, 3", scheduled)
                    self.assertIn('ISCHED_SCHEDULE_OP(1, 0, 0, 0, "ADD")', scheduled)
                    self.assertIn("ISCHED_REG(7,", regmap)
                    self.assertIn("ISCHED_REG(8,", regmap)
                    standalone = f"isched-standalone-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", "-DISCHED_STANDALONE",
                            "ISched.c", "-o", standalone,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run(
                        [str(root / standalone)], cwd=root, check=True
                    )
                    outputs = (
                        (root / "Scheduled.i").read_text(encoding="ascii"),
                        (root / "RegMap.i").read_text(encoding="ascii"),
                    )
                    if standalone_baseline is None:
                        standalone_baseline = outputs
                    else:
                        self.assertEqual(outputs, standalone_baseline)

    def test_streams_keep_inconclusive_search_distinct_from_unsat(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, isched.emit({"options": {"oracle": False}}))
            _write(root, {
                "test.c": _ISCHED_STREAM_BOUNDS_DRIVER,
                "InstrMap.i": """\
ISCHED_RESOURCE(0, "alu", 2, 0)
ISCHED_INSTRUCTION("A", 0, 1, 1, 1, 0)
ISCHED_INSTRUCTION("B", 0, 1, 1, 1, 0)
""",
                "MIR.i": """\
ISCHED_MIR_OP(0, "A", 1, 0, 0)
ISCHED_MIR_OP(1, "B", -1, 0, 10)
ISCHED_MIR_DEP(0, 1, 0, 0)
ISCHED_MIR_DEP(1, 0, 0, 0)
""",
            })
            for index, compiler in enumerate(compilers):
                with self.subTest(compiler=compiler[0]):
                    executable = f"isched-stream-bounds-{index}"
                    subprocess.run(
                        [
                            *compiler, "-std=c99", "-Wall", "-Wextra",
                            "-Werror", "-pedantic", "ISched.c", "test.c",
                            "-o", executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run(
                        [str(root / executable)], cwd=root, check=True,
                        timeout=10,
                    )
                    self.assertFalse((root / "UnknownScheduled.i").exists())
                    self.assertFalse((root / "UnknownRegMap.i").exists())
                    scheduled = (root / "SatScheduled.i").read_text(
                        encoding="ascii"
                    )
                    self.assertIn("ISCHED_SCHEDULE_BEGIN(1, 2, 0, 1)",
                                  scheduled)
                    self.assertLess(
                        scheduled.index('ISCHED_SCHEDULE_OP(0, 0, 0, 0, "A")'),
                        scheduled.index('ISCHED_SCHEDULE_OP(1, 0, 0, 0, "B")'),
                    )

    def test_linked_pipeline_rejects_an_invalid_schedule_before_publication(self):
        compilers = _compilers()
        if not compilers:
            self.skipTest("GCC/Clang-compatible C99 compiler unavailable")
        generated = generate_ir("anf", [self.rv32], options={"oracle": False})
        # Inject a corrupt witness inside the generated scheduler, before the
        # public scheduling API can return it to the unmodified linked pipeline.
        anchor = "    isched_make_order(problem, cycle, result->order);"
        self.assertEqual(generated["ISched.c"].count(anchor), 1)
        generated["ISched.c"] = generated["ISched.c"].replace(
            anchor, anchor + "\n    if (result->operation_count) ++result->stage[0];"
        )
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _write(root, generated)
            _write(root, {"Program.i": _PIPELINE_PROGRAM})
            subprocess.run(
                [
                    *compilers[0], "-std=c99", "-Wall", "-Wextra", "-Werror",
                    "-pedantic", '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                    "-DARCHON_ISEL_NO_MAIN", "-DARCHON_REGALLOC_NO_MAIN",
                    "IR.c", "Graphs.c", "Rewrite.c", "ISel.c", "Pipeline.c",
                    "ISched.c", "Regalloc.c", "Emitter.c", "Driver.c", "-o",
                    "driver",
                ],
                cwd=root,
                check=True,
            )
            result = subprocess.run(
                [str(root / "driver"), "--stop-after", "isched",
                 "--keep-artifacts"],
                cwd=root, capture_output=True, text=True, timeout=10,
            )
            self.assertEqual(result.returncode, 1, result.stderr)
            self.assertIn("isched stage: instruction scheduler failed",
                          result.stderr)
            self.assertTrue((root / "InstrMap.i").exists())
            self.assertFalse((root / "Scheduled.i").exists())
            self.assertFalse((root / "RegMap.i").exists())


_PIPELINE_PROGRAM = """\
IR_PROGRAM_BEGIN("pipeline-schedule-test");
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

_TYPED_PRODUCER = r"""#include "IR.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    IR_Program program;
    IR_Diagnostic diagnostic;
    IR_Type u32 = ir_type(IR_TYPE_UINT, 32, 0);
    IR_Type boolean = ir_type(IR_TYPE_BOOL, 1, 0);
    IR_Type effect_type = ir_type(IR_TYPE_EFFECT, 0, 9);
    IR_Type void_type = ir_type(IR_TYPE_VOID, 0, 0);
    IR_Block entry, yes, no, join;
    IR_Temp x, condition, effect, one, yes_value, no_value;
    IR_Temp yes_effect, no_effect, value_phi, effect_phi, result, final_effect;
    IR_Temp argument[1];
    IR_PhiInput values[2], effects[2];
    FILE *output;

    ir_program_init(&program);
    entry = ir_add_block(&program, "entry");
    yes = ir_add_block(&program, "yes");
    no = ir_add_block(&program, "no");
    join = ir_add_block(&program, "join");
    assert(entry != IR_BLOCK_NONE && join != IR_BLOCK_NONE);

    x = ir_add_parameter(&program, u32, "x");
    condition = ir_add_parameter(&program, boolean, "condition");
    effect = ir_build_effect_start(&program, entry, 9);
    assert(ir_build_cond_branch(&program, entry, condition, yes, no, effect));

    one = ir_build_constant(&program, yes, u32, 1);
    yes_value = ir_build_binary(
        &program, yes, IR_OP_ADD, u32, x, one);
    argument[0] = yes_value;
    assert(ir_build_call(
        &program, yes, "observe", void_type, argument, 1,
        effect, 9, &yes_effect) == IR_TEMP_NONE);
    assert(ir_build_branch(&program, yes, join, yes_effect));

    no_value = ir_build_binary(
        &program, no, IR_OP_SUB, u32, x, one);
    argument[0] = no_value;
    assert(ir_build_call(
        &program, no, "observe", void_type, argument, 1,
        effect, 9, &no_effect) == IR_TEMP_NONE);
    assert(ir_build_branch(&program, no, join, no_effect));

    values[0].predecessor = yes;
    values[0].value = yes_value;
    values[1].predecessor = no;
    values[1].value = no_value;
    value_phi = ir_build_phi(&program, join, u32, values, 2);
    effects[0].predecessor = yes;
    effects[0].value = yes_effect;
    effects[1].predecessor = no;
    effects[1].value = no_effect;
    effect_phi = ir_build_phi(&program, join, effect_type, effects, 2);
    argument[0] = value_phi;
    result = ir_build_call(
        &program, join, "finish", u32, argument, 1,
        effect_phi, 9, &final_effect);
    assert(result != IR_TEMP_NONE);
    assert(ir_build_return(&program, join, result, final_effect));
    assert(ir_verify(&program, &diagnostic));

    /* The verifier is not a builder-only assertion. */
    program.temporaries[condition].type = u32;
    assert(!ir_verify(&program, &diagnostic));
    program.temporaries[condition].type = boolean;
    assert(ir_verify(&program, &diagnostic));

    output = fopen("Program.i", "w");
    assert(output != NULL);
    assert(ir_print_program_i(&program, output));
    assert(fclose(output) == 0);
    output = tmpfile();
    assert(output != NULL);
    assert(ir_print(&program, output));
    assert(ftell(output) > 0);
    assert(fclose(output) == 0);
    ir_program_destroy(&program);
    return 0;
}
"""


_TYPED_CONSUMER = r"""#include "IR.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    IR_Program program;
    IR_Diagnostic diagnostic;
    FILE *output;
    assert(ir_load_program_i(&program, &diagnostic));
    assert(program.block_count == 4);
    assert(program.temporary_count >= 10);
    assert(ir_verify(&program, &diagnostic));
    output = tmpfile();
    assert(output != NULL);
    assert(ir_print(&program, output));
    assert(fclose(output) == 0);
    ir_program_destroy(&program);
    return 0;
}
"""


_ISCHED_INFLUENCE_DRIVER = r"""#include "ISched.h"
#include <assert.h>
#include <string.h>

static unsigned calls;
static int mode;

static int influence(
        void *context, ISched_InfluenceKind kind, unsigned id,
        int64_t current, int64_t *replacement, int *replace,
        char *message, size_t message_capacity) {
    (void)context;
    (void)current;
    (void)message;
    (void)message_capacity;
    ++calls;
    *replace = 0;
    if (kind == ISCHED_INFLUENCE_RESOURCE_CAPACITY && mode == 1) {
        *replacement = 2;
        *replace = 1;
    } else if (kind == ISCHED_INFLUENCE_RESOURCE_CAPACITY && mode == 5) {
        *replacement = 4097;
        *replace = 1;
    } else if (kind == ISCHED_INFLUENCE_OPERATION_PRIORITY && mode == 2) {
        *replacement = id == 1u ? 10 : 0;
        *replace = 1;
    } else if (kind == ISCHED_INFLUENCE_OPERATION_LATENCY && mode == 3) {
        *replacement = 70000;
        *replace = 1;
    } else if (kind == ISCHED_INFLUENCE_OPERATION_LATENCY && mode == 4) {
        *replacement = 3;
        *replace = 1;
    }
    return 1;
}

int main(void) {
    ISched_Resource resources[] = {{"alu", 1u, 0u}};
    ISched_Operation operations[] = {
        {0u, "a", 0u, 1u, 1u, 1u, 0, 0u},
        {1u, "b", 0u, 1u, 1u, 1u, 0, 0u}
    };
    ISched_Problem problem = {
        operations, 2u, NULL, 0u, resources, 1u
    };
    unsigned char latency_known[] = {0u, 0u};
    unsigned char capacity_known[] = {0u};
    unsigned char priority_known[] = {0u, 0u};
    int priorities[] = {0, 0};
    ISched_KnownFacts known = {
        2u, 1u, latency_known, capacity_known,
        priorities, priority_known
    };
    ISched_Options options = isched_default_options();
    ISched_Result result;
    char message[128];

    mode = 1;
    calls = 0u;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_SAT);
    assert(result.ii == 2u);
    assert(calls == 0u); /* Unknown capacity stays unknown. */
    isched_result_destroy(&result);

    capacity_known[0] = 1u;
    calls = 0u;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_SAT);
    assert(result.ii == 1u);
    assert(calls == 1u);
    isched_result_destroy(&result);

    resources[0].capacity = 1u;
    resources[0].flags = ISCHED_MODEL_CONSERVATIVE;
    calls = 0u;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_SAT);
    assert(result.ii == 2u);
    assert(calls == 0u);
    isched_result_destroy(&result);
    resources[0].flags = 0u;

    mode = 5;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_INVALID);

    capacity_known[0] = 0u;
    priority_known[0] = priority_known[1] = 1u;
    options.search_limit = 0u;
    mode = 2;
    calls = 0u;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_FALLBACK);
    assert(result.order[0] == 1u);
    assert(calls == 2u);
    isched_result_destroy(&result);

    options.search_limit = 1000000u;
    priority_known[0] = priority_known[1] = 0u;
    latency_known[0] = 1u;
    mode = 3;
    assert(isched_schedule_with_influence(
               &problem, &options, &known, influence, NULL, &result,
               message, sizeof message) == ISCHED_STATUS_INVALID);

    {
        ISched_Dependency dependency = {
            0u, 1u, 1u, 0u,
            ISCHED_DEP_DATA | ISCHED_DEP_OPERATION_LATENCY, 0u, 1u
        };
        problem.dependencies = &dependency;
        problem.dependency_count = 1u;
        resources[0].capacity = 2u;
        mode = 4;
        assert(isched_schedule_with_influence(
                   &problem, &options, &known, influence, NULL, &result,
                   message, sizeof message) == ISCHED_STATUS_SAT);
        assert(result.cycle[1] >= result.cycle[0] + 3u);
        isched_result_destroy(&result);
    }
    return 0;
}
"""


_ISCHED_ANNOTATION_DRIVER = r"""#include "ISched.h"
#include "Oracle.h"
#include <assert.h>

int main(void) {
    ArchonOracleAnnotations annotations;
    ArchonOracleDiagnostic diagnostic;
    ISched_Resource resources[] = {{"alu", 1u, 0u}};
    ISched_Operation operations[] = {
        {0u, "a", 0u, 1u, 1u, 1u, 0, 0u},
        {1u, "b", 0u, 1u, 1u, 1u, 0, 0u}
    };
    ISched_Problem problem = {
        operations, 2u, NULL, 0u, resources, 1u
    };
    unsigned char latency_known[] = {0u, 0u};
    unsigned char capacity_known[] = {1u};
    unsigned char priority_known[] = {1u, 1u};
    int priorities[] = {0, 0};
    ISched_KnownFacts known = {
        2u, 1u, latency_known, capacity_known,
        priorities, priority_known
    };
    ISched_Options options = isched_default_options();
    ISched_Result result;
    char message[128];

    archon_oracle_annotations_init(&annotations);
    archon_oracle_diagnostic_init(&diagnostic);
    assert(archon_oracle_annotations_load(
               "facts.oracle.i", &annotations, &diagnostic) ==
           ARCHON_ORACLE_STATUS_OK);
    assert(isched_schedule_with_annotations(
               &problem, &options, &known, &annotations, &result,
               message, sizeof message) == ISCHED_STATUS_SAT);
    assert(result.ii == 1u);
    isched_result_destroy(&result);

    options.search_limit = 0u;
    assert(isched_schedule_with_annotations(
               &problem, &options, &known, &annotations, &result,
               message, sizeof message) == ISCHED_STATUS_FALLBACK);
    assert(result.order[0] == 1u);
    isched_result_destroy(&result);

    /* The same annotations cannot create knowledge the caller withheld. */
    capacity_known[0] = 0u;
    priority_known[0] = priority_known[1] = 0u;
    options.search_limit = 1000000u;
    assert(isched_schedule_with_annotations(
               &problem, &options, &known, &annotations, &result,
               message, sizeof message) == ISCHED_STATUS_SAT);
    assert(result.ii == 2u);
    isched_result_destroy(&result);

    annotations.protocol_version = 0u;
    assert(isched_schedule_with_annotations(
               &problem, &options, &known, &annotations, &result,
               message, sizeof message) == ISCHED_STATUS_ORACLE_ERROR);
    assert(result.status == ISCHED_STATUS_ORACLE_ERROR);
    annotations.protocol_version = ARCHON_ORACLE_PROTOCOL_VERSION;
    capacity_known[0] = 1u;
    annotations.entries[0].value.u64 = UINT64_C(0);
    assert(isched_schedule_with_annotations(
               &problem, &options, &known, &annotations, &result,
               message, sizeof message) == ISCHED_STATUS_ORACLE_ERROR);
    assert(result.status == ISCHED_STATUS_ORACLE_ERROR);
    archon_oracle_annotations_dispose(&annotations);
    return 0;
}
"""


_ISCHED_ORACLE_WORKER = r"""#include "Oracle.h"
#include <stdio.h>
#include <string.h>

static ArchonOracleStatus respond(
        const ArchonOracleWorkerRequest *request,
        ArchonOracleWorkerResponse *response,
        void *context,
        ArchonOracleDiagnostic *diagnostic) {
    if (!request || !response || !request->artifact_data ||
            request->stage != ARCHON_ORACLE_STAGE_ISCHED)
        return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
    if (request->artifact_path &&
            strstr(request->artifact_path, "InstrMap.i") != NULL) {
        static const unsigned char capacity[] = "\"alu\", 1, 0)";
        size_t offset;
        for (offset = 0u;
                offset + sizeof capacity - 1u <= request->artifact_size;
                ++offset)
            if (memcmp(
                    request->artifact_data + offset,
                    capacity, sizeof capacity - 1u) == 0)
                break;
        if (offset + sizeof capacity - 1u > request->artifact_size)
            return ARCHON_ORACLE_STATUS_INVALID_ARGUMENT;
        request->artifact_data[
            offset + (sizeof "\"alu\", " - 1u)] = (unsigned char)'2';
        response->artifact_size = request->artifact_size;
        response->artifact_changed = 1;
    }
    if (request->artifact_path &&
            strstr(request->artifact_path, "RegMap.i") != NULL &&
            context && *(const int *)context) {
        archon_oracle_diagnostic_set(
            diagnostic, ARCHON_ORACLE_STATUS_WORKER_ERROR, 0,
            "forced RegMap failure");
        return ARCHON_ORACLE_STATUS_WORKER_ERROR;
    }
    return ARCHON_ORACLE_STATUS_OK;
}

int main(int argc, char **argv) {
    ArchonOracleServer server;
    ArchonOracleDiagnostic diagnostic;
    ArchonOracleStatus status = ARCHON_ORACLE_STATUS_OK;
    unsigned request;
    int fail_regmap;
    if (argc != 2 && argc != 3)
        return 2;
    fail_regmap = argc == 2;
    archon_oracle_server_init(&server);
    archon_oracle_diagnostic_init(&diagnostic);
    status = archon_oracle_server_open(
        &server, argv[1], 4u, 0u, &diagnostic);
    for (request = 0u;
            request < 4u && status == ARCHON_ORACLE_STATUS_OK;
            ++request) {
        archon_oracle_diagnostic_init(&diagnostic);
        status = archon_oracle_server_serve_once(
            &server, respond, &fail_regmap, &diagnostic);
    }
    archon_oracle_server_dispose(&server);
    if (request == 4u &&
            (status == ARCHON_ORACLE_STATUS_OK ||
             (fail_regmap &&
              status == ARCHON_ORACLE_STATUS_WORKER_ERROR)))
        return 0;
    (void)fprintf(
        stderr, "%s: %s\n",
        archon_oracle_status_string(status), diagnostic.message);
    return 1;
}
"""


_KNOWN_INSTR_MAP = """\
ISCHED_RESOURCE(0, "alu", 1, 0)
ISCHED_INSTRUCTION("A", 0, 1, 1, 1, 0)
ISCHED_INSTRUCTION("B", 0, 1, 1, 1, 0)
ARCHON_INSTR(1, 0, "A", 0, 0, 0, 0, 0)
ARCHON_INSTR(2, 1, "B", 0, 0, 0, 0, 0)
"""


_KNOWN_MIR = """\
MIR_SCHEMA(2)
MIR_PROGRAM_BEGIN("oracle-known")
MIR_OP(0, 0, 0, 0, 4294967295u, MIR_OPCODE_ADD, 0, 0, 0)
MIR_OP(1, 0, 0, 1, 4294967295u, MIR_OPCODE_SUB, 0, 0, 0)
MIR_PROGRAM_END()
"""


_CONSERVATIVE_INSTR_MAP = """\
ISCHED_RESOURCE(0, "unknown", 1, 1)
ISCHED_INSTRUCTION("A", 0, 1, 1, 1, 1)
ISCHED_INSTRUCTION("B", 0, 1, 1, 1, 1)
"""


_CONSERVATIVE_MIR = """\
ISCHED_MIR_OP(0, "A")
ISCHED_MIR_OP(1, "B")
"""


_ISCHED_DRIVER = r"""#include "ISched.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static ISched_Options options(void) {
    ISched_Options value = isched_default_options();
    value.max_ii = 32;
    value.max_stage = 4;
    value.search_limit = 2000000;
    value.solution_limit = 64;
    return value;
}

static void sat_and_resources(void) {
    ISched_Resource resources[] = {{"alu", 1, 0}};
    ISched_Operation operations[] = {
        {0, "long-a", 0, 1, 3, 1, 0, 0},
        {1, "long-b", 0, 1, 3, 1, 0, 0}
    };
    ISched_Problem problem = {
        operations, 2, NULL, 0, resources, 1
    };
    ISched_Result first, second;
    ISched_Options config = options();
    assert(isched_schedule(&problem, &config, &first) == ISCHED_STATUS_SAT);
    assert(first.res_mii == 6 && first.ii == 6);
    assert(first.slot[0] != first.slot[1]);
    assert(isched_verify(&problem, &first));
    assert(isched_schedule(&problem, &config, &second) == ISCHED_STATUS_SAT);
    assert(second.ii == first.ii);
    assert(memcmp(second.cycle, first.cycle,
                  2 * sizeof *first.cycle) == 0);
    isched_result_destroy(&first);
    isched_result_destroy(&second);
}

static void unsat_then_next_ii(void) {
    ISched_Resource resources[] = {{"alu", 1, 0}};
    ISched_Operation operations[] = {
        {0, "a", 0, 1, 1, 1, 0, 0},
        {1, "b", 0, 1, 1, 1, 0, 0}
    };
    ISched_Dependency dependencies[] = {
        {0, 1, 2, 0, ISCHED_DEP_DATA, 0, 1},
        {1, 0, 0, 1, ISCHED_DEP_DATA, 0, 2}
    };
    ISched_Problem problem = {
        operations, 2, dependencies, 2, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_SAT);
    assert(result.first_ii == 2);
    assert(result.ii == 3);
    assert(result.attempts == 2);
    assert(isched_verify(&problem, &result));
    isched_result_destroy(&result);
}

static void recurrence_graph(void) {
    ISched_Resource resources[] = {{"alu", 2, 0}};
    ISched_Operation operations[] = {
        {0, "a", 0, 3, 1, 1, 0, 0},
        {1, "b", 0, 4, 1, 1, 0, 0}
    };
    ISched_Dependency dependencies[] = {
        {0, 1, 3, 0, ISCHED_DEP_DATA, 0, 1},
        {1, 0, 4, 1, ISCHED_DEP_DATA, 0, 2}
    };
    ISched_Problem problem = {
        operations, 2, dependencies, 2, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_SAT);
    assert(result.rec_mii == 7 && result.ii == 7);
    assert(isched_verify(&problem, &result));
    isched_result_destroy(&result);
}

static void unknown_fallback(void) {
    ISched_Resource resources[] = {{"alu", 1, 0}};
    ISched_Operation operations[] = {
        {1, "producer", 0, 1, 1, 1, 1, 0},
        {0, "consumer", 0, 1, 1, 1, -1, 0}
    };
    ISched_Problem problem = {
        operations, 2, NULL, 0, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    config.search_limit = 0;
    assert(isched_schedule(&problem, &config, &result) ==
           ISCHED_STATUS_FALLBACK);
    assert(result.status == ISCHED_STATUS_FALLBACK);
    assert(result.last_solver_status == ISCHED_SOLVER_UNKNOWN);
    assert(result.cycle[1] < result.cycle[0]);
    assert(isched_verify(&problem, &result));
    isched_result_destroy(&result);

    resources[0].capacity = 2;
    config.max_ii = 1;
    assert(isched_schedule(&problem, &config, &result) ==
           ISCHED_STATUS_UNKNOWN);  /* Serializing would exceed the II cap. */
    assert(result.last_solver_status == ISCHED_SOLVER_UNKNOWN);
    assert(!result.cycle && !result.order);
    isched_result_destroy(&result);

    config.search_limit = 1;
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_UNKNOWN);
    assert(result.last_solver_status == ISCHED_SOLVER_UNKNOWN);
    assert(result.explored == 1);
    isched_result_destroy(&result);

    config.search_limit = 2000000;
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_SAT);
    assert(result.ii == 1);
    isched_result_destroy(&result);

    operations[0].units = 3;
    config.max_ii = 32;
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_UNSAT);
    assert(result.last_solver_status == ISCHED_SOLVER_UNSAT);
    assert(!result.cycle && !result.order);
    isched_result_destroy(&result);
}

static void formula_limit_fallback(void) {
    ISched_Resource resources[] = {{"alu", 18, 0}};
    ISched_Operation operations[18];
    ISched_Problem problem = {
        operations, 18, NULL, 0, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    unsigned i;
    for (i = 0; i < 18; ++i) {
        ISched_Operation row = {i, "a", 0, 1, 1, 1, 0, 0};
        operations[i] = row;
    }
    assert(isched_schedule(&problem, &config, &result) ==
           ISCHED_STATUS_FALLBACK);
    assert(result.last_solver_status == ISCHED_SOLVER_UNKNOWN);
    assert(result.first_ii == 1 && result.ii == 18);
    assert(result.attempts == 1 && result.explored == 0);
    assert(isched_verify(&problem, &result));
    isched_result_destroy(&result);
}

static void verify_witness(void) {
    ISched_Resource resources[] = {{"alu", 1, 0}};
    ISched_Operation operations[] = {
        {0, "a", 0, 1, 3, 1, 0, 0},
        {1, "b", 0, 1, 3, 1, 0, 0}
    };
    ISched_Dependency dependency = {0, 1, 2, 0, ISCHED_DEP_ORDER, 0, 0};
    ISched_Problem problem = {
        operations, 2, &dependency, 1, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    FILE *scheduled = tmpfile(), *regmap = tmpfile();
    unsigned original;
    assert(scheduled && regmap);
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_SAT);
    assert(isched_verify(&problem, &result));
    result.last_solver_status = ISCHED_SOLVER_UNKNOWN;
    assert(!isched_verify(&problem, &result));
    assert(isched_write_streams(scheduled, regmap, &problem, &result) ==
           ISCHED_STATUS_INVALID);
    assert(ftell(scheduled) == 0 && ftell(regmap) == 0);
    result.last_solver_status = ISCHED_SOLVER_SAT;

    original = result.order[1];
    result.order[1] = result.order[0];
    assert(!isched_verify(&problem, &result));
    result.order[1] = original;
    ++result.stage[0];
    assert(!isched_verify(&problem, &result));
    --result.stage[0];

    /* A too-late dependency and a wrapping occupancy conflict each fail. */
    dependency.latency = 100;
    assert(!isched_verify(&problem, &result));
    dependency.latency = 2;
    operations[1].occupancy = 4;
    assert(!isched_verify(&problem, &result));
    operations[1].occupancy = 3;
    assert(isched_verify(&problem, &result));

    ++result.pressure_area;
    assert(!isched_verify(&problem, &result));
    isched_result_destroy(&result);
    assert(fclose(scheduled) == 0 && fclose(regmap) == 0);
}

static void pressure_objective(void) {
    ISched_Resource resources[] = {{"alu", 2, 0}};
    ISched_Operation operations[] = {
        {0, "produce-a", 0, 1, 1, 1, 1, 0},
        {1, "produce-b", 0, 1, 1, 1, 1, 0},
        {2, "use-a", 0, 1, 1, 1, -1, 0},
        {3, "use-b", 0, 1, 1, 1, -1, 0}
    };
    ISched_Dependency dependencies[] = {
        {0, 2, 1, 0, ISCHED_DEP_DATA, 0, 10},
        {1, 3, 1, 0, ISCHED_DEP_DATA, 0, 11}
    };
    ISched_Problem problem = {
        operations, 4, dependencies, 2, resources, 1
    };
    ISched_Result result;
    ISched_Options config = options();
    config.max_stage = 3;
    assert(isched_schedule(&problem, &config, &result) == ISCHED_STATUS_SAT);
    assert(result.ii == 2);
    assert(result.peak_pressure == 1);
    assert(isched_verify(&problem, &result));
    isched_result_destroy(&result);
}

int main(void) {
    sat_and_resources();
    unsat_then_next_ii();
    recurrence_graph();
    unknown_fallback();
    formula_limit_fallback();
    verify_witness();
    pressure_objective();
    assert(isched_schedule_streams(
        "MIR.i", "InstrMap.i", "StreamScheduled.i", "StreamRegMap.i",
        NULL) == ISCHED_STATUS_SAT);
    return 0;
}
"""


_ISCHED_STREAM_BOUNDS_DRIVER = r"""#include "ISched.h"
#include <assert.h>
#include <string.h>

int main(void) {
    ISched_Options config = isched_default_options();
    char message[128];
    config.max_ii = 1;
    config.max_stage = 1;
    config.search_limit = 0;
    assert(isched_schedule_streams_with_diagnostic(
               "MIR.i", "InstrMap.i", "UnknownScheduled.i", "UnknownRegMap.i",
               &config, message, sizeof message) == ISCHED_STATUS_UNKNOWN);
    assert(strstr(message, "search inconclusive") != NULL);
    config.search_limit = 100;
    assert(isched_schedule_streams(
               "MIR.i", "InstrMap.i", "SatScheduled.i", "SatRegMap.i",
               &config) == ISCHED_STATUS_SAT);
    return 0;
}
"""


_INSTR_MAP = """\
ARCHON_INSTR_MAP_BEGIN("RISCV32")
ARCHON_INSTR(1u, 0u, "ADD", 0u, 1u, 4u, 0u, 0u)
ARCHON_INSTR(2u, 1u, "SUB", 1u, 1u, 4u, 0u, 0u)
ARCHON_INSTR_OPERAND(2u, 7u, 1u)
ARCHON_INSTR(3u, 2u, "XOR", 1u, 1u, 4u, 0u, 0u)
ARCHON_INSTR_OPERAND(3u, 8u, 2u)
ARCHON_INSTR_ROOT(9u, 3u)
ARCHON_INSTR_MAP_END()
"""


_MIR = """\
MIR_SCHEMA(2)
MIR_PROGRAM_BEGIN("stream-test")
MIR_OP(0, 0, 0, 0, 7, MIR_OPCODE_ADD, 1, MIR_EFFECT_NONE, 0)
MIR_OP(1, 0, 0, 1, 8, MIR_OPCODE_SUB, 1, MIR_EFFECT_NONE, 1)
MIR_DEPENDENCY(1, 0, MIR_DEP_DATA)
MIR_OP(2, 0, 0, 2, 9, MIR_OPCODE_XOR, 1, MIR_EFFECT_NONE, 2)
MIR_DEPENDENCY(2, 1, MIR_DEP_DATA | MIR_DEP_CONTROL)
MIR_PROGRAM_END()
"""


if __name__ == "__main__":
    unittest.main()
