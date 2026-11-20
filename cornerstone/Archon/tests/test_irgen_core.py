"""Exercise the IR-generator registry, atomic writer, and Gated SSA C99."""

import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import (
    EXPECTED_REPRESENTATIONS,
    available_representations,
    build_context,
    generate_ir,
    get_representation,
    pipeline_stages,
    standard_spec_entry_points,
    write_generated_directory,
)


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"
RV64 = ROOT / "stdspec" / "riscv" / "rv64i.arch"


class IRGenerationCore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.rv32 = compile_file(RV32)
        cls.rv64 = compile_file(RV64)

    def compiler(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")
        return command

    def test_registry_context_and_stdspec_discovery(self):
        self.assertEqual(available_representations(), EXPECTED_REPRESENTATIONS)
        self.assertEqual(get_representation("gssa").NAME, "gated")
        self.assertEqual(get_representation("tac").NAME, "typed-tac")
        self.assertEqual(get_representation("utac").NAME, "untyped-tac")
        with self.assertRaisesRegex(ArchonError, "unknown IR"):
            get_representation("not-an-ir")

        source = json.loads(json.dumps(self.rv32))
        context = build_context(
            "gated-ssa", [source], options={"debug": True, "limit": 4}
        )
        self.assertEqual(context["ir"], "gated")
        self.assertEqual(context["architecture_names"], ["RISCV32"])
        self.assertEqual(
            context["options"],
            {"debug": True, "limit": 4, "oracle": True},
        )
        self.assertEqual(context["architectures"][0], source)
        context["architectures"][0]["features"].append("mutation")
        self.assertNotIn("mutation", source["features"])
        json.dumps(context, allow_nan=False)

        entries = standard_spec_entry_points()
        self.assertGreaterEqual(len(entries), 9)
        self.assertIn(RV32.resolve(), {path.resolve() for path in entries})
        self.assertTrue(all(path.suffix == ".arch" for path in entries))
        stage_names = {stage.__name__.rsplit(".", 1)[-1] for stage in pipeline_stages()}
        self.assertIn("graphs", stage_names)
        self.assertIn("oracle", stage_names)
        self.assertIn("sandbox", stage_names)

    def test_generation_is_deterministic_and_runs_stages(self):
        first = generate_ir("gated", [self.rv32], run_stages=False)
        second = generate_ir("gated", [self.rv32], run_stages=False)
        self.assertEqual(first, second)
        self.assertEqual(set(first), {"IR.c", "IR.h"})
        self.assertIn("ARCHON_IR_OP_RISCV32_ADD", first["IR.h"])
        self.assertIn("archon_ir_build_phi", first["IR.h"])
        self.assertIn("ARCHON_IR_PROGRAM_BEGIN", first["IR.c"])

        complete = generate_ir("gated", [self.rv32])
        self.assertEqual(complete, generate_ir("gated", [self.rv32]))
        self.assertTrue({
            "Graphs.c",
            "Oracle.c",
            "Oracle.h",
            "Rewrite.c",
            "Scheduler.c",
            "Pipeline.c",
            "ISel.c",
            "Regalloc.c",
            "Emitter.c",
            "Assembler.sh",
            "Linker.sh",
            "PrettyPrint.py",
            "Proof.py",
            "Proofs.smt",
            "Translate.py",
            "RunSandbox.c",
        } <= complete.keys())
        self.assertNotIn("AsmOPT.lua", complete)
        self.assertNotIn("LTO.lua", complete)
        self.assertTrue(all(callable(stage.emit) for stage in pipeline_stages()))

    def test_oracle_option_is_default_on_strict_and_can_be_disabled(self):
        default_context = build_context("gated", [self.rv32])
        self.assertIs(default_context["options"]["oracle"], True)

        disabled_context = build_context(
            "gated", [self.rv32], options={"oracle": False}
        )
        self.assertIs(disabled_context["options"]["oracle"], False)
        disabled = generate_ir(
            "gated", [self.rv32], options={"oracle": False}
        )
        self.assertEqual(
            disabled,
            generate_ir("gated", [self.rv32], options={"oracle": False}),
        )
        self.assertNotIn("Oracle.c", disabled)
        self.assertNotIn("Oracle.h", disabled)
        self.assertIn("Graphs.c", disabled)

        for value in (None, 0, 1, "false", [], {}):
            with self.subTest(value=value), self.assertRaisesRegex(
                ArchonError, "option 'oracle' must be a boolean"
            ):
                build_context("gated", [self.rv32], options={"oracle": value})

    def test_multiple_targets_have_an_explicit_consistent_pipeline_target(self):
        default = generate_ir("anf", [self.rv64, self.rv32])
        selected = generate_ir(
            "anf", [self.rv32, self.rv64],
            options={"target": "RISCV64"},
        )
        self.assertIn(
            'static const char Emitter_target[] = "RISCV32";',
            default["Emitter.c"],
        )
        self.assertIn("AP_WORD_WIDTH = 32", default["Pipeline.c"])
        self.assertIn(
            'static const char Emitter_target[] = "RISCV64";',
            selected["Emitter.c"],
        )
        self.assertIn("AP_WORD_WIDTH = 64", selected["Pipeline.c"])
        # Shared databases still contain every explicitly selected target.
        self.assertIn('"RISCV32"', selected["TargetFacts.c"])
        self.assertIn('"RISCV64"', selected["TargetFacts.c"])
        with self.assertRaisesRegex(ArchonError, "unknown architecture"):
            generate_ir(
                "anf", [self.rv32, self.rv64],
                options={"target": "missing"},
            )

    def test_atomic_writer_and_path_safety(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / "generated"
            files = {
                "IR.c": "c\n", "nested/IR.h": "h\n",
                "Assembler.sh": "#!/bin/sh\n", "Linker.sh": "#!/bin/sh\n",
            }
            written = write_generated_directory(files, output)
            self.assertEqual(written, output.resolve())
            self.assertEqual((output / "IR.c").read_text(), "c\n")
            self.assertEqual((output / "nested" / "IR.h").read_text(), "h\n")
            if os.name == "posix":
                for name in ("Assembler.sh", "Linker.sh"):
                    self.assertEqual((output / name).stat().st_mode & 0o111, 0o111)

            # An identical rerun is a deterministic no-op.
            before = (output / "IR.c").stat().st_ino
            write_generated_directory(files, output)
            self.assertEqual((output / "IR.c").stat().st_ino, before)
            if os.name == "posix":
                (output / "Assembler.sh").chmod(0o644)
                with self.assertRaisesRegex(ArchonError, "nonempty"):
                    write_generated_directory(files, output)
                write_generated_directory(files, output, force=True)
                self.assertEqual((output / "Assembler.sh").stat().st_mode & 0o111, 0o111)

            with self.assertRaisesRegex(ArchonError, "nonempty"):
                write_generated_directory({"IR.c": "changed\n"}, output)
            self.assertEqual((output / "IR.c").read_text(), "c\n")
            write_generated_directory({"IR.c": "changed\n"}, output, force=True)
            self.assertEqual((output / "IR.c").read_text(), "changed\n")

            protected = output / "input.archdb"
            protected.write_text("input", encoding="utf-8")
            with self.assertRaisesRegex(ArchonError, "overwrite input"):
                write_generated_directory(
                    {"IR.c": "unsafe\n"}, output, input_paths=[protected], force=True
                )
            self.assertEqual(protected.read_text(), "input")

            for unsafe in ("../escape", "/absolute", "a/../../escape", "a\\b"):
                with self.subTest(unsafe=unsafe), self.assertRaises(ArchonError):
                    write_generated_directory({unsafe: "x"}, root / "other")

    def test_gated_strict_c99_builders_and_program_i_roundtrip(self):
        compiler = self.compiler()
        files = generate_ir("gated", [self.rv32], run_stages=False)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in files.items():
                (root / name).write_text(text, encoding="utf-8", newline="\n")
            (root / "producer.c").write_text(_PRODUCER, encoding="ascii")
            subprocess.run(
                [
                    *compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "IR.c",
                    "producer.c",
                    "-o",
                    "producer",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "producer")], cwd=root, check=True)
            program_i = (root / "Program.i").read_text(encoding="ascii")
            self.assertIn("ARCHON_IR_BLOCK", program_i)
            self.assertIn("ARCHON_IR_OP_RISCV32_ADD", program_i)
            self.assertIn("ARCHON_IR_OP_PHI", program_i)
            self.assertIn("ARCHON_IR_OP_GATE", program_i)
            self.assertIn("ARCHON_IR_TYPE_EFFECT", program_i)

            (root / "consumer.c").write_text(_CONSUMER, encoding="ascii")
            subprocess.run(
                [
                    *compiler,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    '-DARCHON_IR_PROGRAM_FILE="Program.i"',
                    "IR.c",
                    "consumer.c",
                    "-o",
                    "consumer",
                ],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "consumer")], cwd=root, check=True)

    def test_cli_irgen_and_input_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archdb = root / "rv32.archdb"
            from archon import database

            database.save(self.rv32, archdb)
            output = root / "generated"
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "gated",
                    archdb,
                    "-o",
                    output,
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertTrue((output / "IR.c").is_file())
            self.assertTrue((output / "IR.h").is_file())
            self.assertTrue((output / "Oracle.c").is_file())
            self.assertTrue((output / "Oracle.h").is_file())
            self.assertEqual(result.stdout, b"")

            nested = output / "input.archdb"
            shutil.copyfile(archdb, nested)
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "gated",
                    nested,
                    "-o",
                    output,
                    "--force",
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"overwrite input", result.stderr)
            self.assertTrue(nested.is_file())

    def test_cli_no_oracle_conflict_and_invalid_value_are_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archdb = root / "rv32.archdb"
            from archon import database

            database.save(self.rv32, archdb)
            without_oracle = root / "without-oracle"
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "gated",
                    archdb,
                    "-o",
                    without_oracle,
                    "--no-oracle",
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertTrue((without_oracle / "IR.c").is_file())
            self.assertFalse((without_oracle / "Oracle.c").exists())
            self.assertFalse((without_oracle / "Oracle.h").exists())

            conflict_output = root / "conflict"
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "gated",
                    archdb,
                    "-o",
                    conflict_output,
                    "--no-oracle",
                    "--option",
                    "oracle=false",
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                b"specified by both --no-oracle and --option",
                result.stderr,
            )
            self.assertFalse(conflict_output.exists())

            invalid_output = root / "invalid"
            invalid_output.mkdir()
            marker = invalid_output / "unrelated.txt"
            marker.write_text("preserve me\n", encoding="ascii")
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "gated",
                    archdb,
                    "-o",
                    invalid_output,
                    "--option",
                    "oracle=0",
                    "--force",
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b"option 'oracle' must be a boolean", result.stderr)
            self.assertEqual(marker.read_text(encoding="ascii"), "preserve me\n")
            self.assertEqual(set(invalid_output.iterdir()), {marker})

    def test_cli_selects_one_pipeline_target_from_many_databases(self):
        from archon import database

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            rv32 = root / "rv32.archdb"
            rv64 = root / "rv64.archdb"
            database.save(self.rv32, rv32)
            database.save(self.rv64, rv64)
            output = root / "generated"
            result = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "anf",
                    rv32,
                    rv64,
                    "--target",
                    "RISCV64",
                    "-o",
                    output,
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertIn(
                'static const char Emitter_target[] = "RISCV64";',
                (output / "Emitter.c").read_text(encoding="ascii"),
            )
            facts = (output / "TargetFacts.c").read_text(encoding="ascii")
            self.assertIn('"RISCV32"', facts)
            self.assertIn('"RISCV64"', facts)

            missing = root / "missing"
            rejected = subprocess.run(
                [
                    sys.executable,
                    "-m",
                    "archon",
                    "irgen",
                    "anf",
                    rv32,
                    rv64,
                    "--target",
                    "missing",
                    "-o",
                    missing,
                ],
                cwd=ROOT,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn(b"unknown architecture", rejected.stderr)
            self.assertFalse(missing.exists())


_PRODUCER = r"""#include "IR.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct archon_ir_program program;
    struct archon_ir_builder builder;
    archon_ir_block_id entry, left, right, merge, incoming[2];
    struct archon_ir_value condition, value, zero, sum, phi, gate;
    struct archon_ir_value incoming_values[2], add_inputs[2];
    struct archon_ir_effect start, next;
    char message[192];
    FILE *output;

    archon_ir_program_init(&program);
    archon_ir_builder_init(&builder, &program);
    entry = archon_ir_create_block(&program, "entry\077\077/trigraph");
    left = archon_ir_create_block(&program, "left");
    right = archon_ir_create_block(&program, "right");
    merge = archon_ir_create_block(&program, "merge");
    assert(entry && left && right && merge);
    assert(archon_ir_position_at_end(&builder, entry) == ARCHON_IR_OK);
    condition = archon_ir_build_parameter(&builder, archon_ir_uint(1), 0);
    value = archon_ir_build_parameter(&builder, archon_ir_uint(32), 1);
    zero = archon_ir_build_constant(&builder, archon_ir_uint(32), 0);
    add_inputs[0] = value;
    add_inputs[1] = zero;
    sum = archon_ir_build_node(
        &builder, ARCHON_IR_OP_RISCV32_ADD, archon_ir_uint(32), add_inputs, 2);

    assert(archon_ir_position_at_end(&builder, merge) == ARCHON_IR_OK);
    incoming[0] = left;
    incoming[1] = right;
    incoming_values[0] = sum;
    incoming_values[1] = zero;
    phi = archon_ir_build_phi(
        &builder, archon_ir_uint(32), incoming, incoming_values, 2);
    gate = archon_ir_build_gate(&builder, condition, phi, zero);
    start = archon_ir_build_effect_start(&builder);
    next = archon_ir_build_effect(
        &builder, ARCHON_IR_OP_EFFECT, start, &gate, 1);
    assert(gate.id && next.id);
    assert(archon_ir_seal_block(&program, merge) == ARCHON_IR_OK);
    assert(archon_ir_verify(&program, message, sizeof message) == ARCHON_IR_OK);
    assert(archon_ir_architecture_count() == 1);
    assert(archon_ir_architecture_name(0) != NULL);

    output = fopen("Program.i", "w");
    assert(output != NULL);
    assert(archon_ir_emit_program_i(output, &program) == ARCHON_IR_OK);
    assert(fclose(output) == 0);
    output = fopen("Program.txt", "w");
    assert(output != NULL);
    assert(archon_ir_print(output, &program) == ARCHON_IR_OK);
    assert(fclose(output) == 0);
    archon_ir_program_dispose(&program);
    return 0;
}
"""


_CONSUMER = r"""#include "IR.h"
#include <assert.h>
#include <string.h>

int main(void) {
    struct archon_ir_program program;
    char message[192];
    archon_ir_program_init(&program);
    assert(archon_ir_load_program_i(&program) == ARCHON_IR_OK);
    assert(program.block_count == 4);
    assert(program.node_count == 8);
    assert(strcmp(archon_ir_block(&program, 1)->name, "entry\077\077/trigraph") == 0);
    assert(archon_ir_verify(&program, message, sizeof message) == ARCHON_IR_OK);
    assert(archon_ir_node(&program, 4)->opcode == ARCHON_IR_OP_RISCV32_ADD);
    assert(archon_ir_node(&program, 5)->opcode == ARCHON_IR_OP_PHI);
    assert(archon_ir_node(&program, 6)->opcode == ARCHON_IR_OP_GATE);
    assert(archon_ir_node(&program, 8)->type.kind == ARCHON_IR_TYPE_EFFECT);
    archon_ir_program_dispose(&program);
    return 0;
}
"""


if __name__ == "__main__":
    unittest.main()
