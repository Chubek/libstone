"""Canonical X-macro ABI, validation, escaping, and strict-C99 smoke tests."""

from __future__ import annotations

import os
from pathlib import Path
import random
import shlex
import shutil
import string
import subprocess
import tempfile
import unittest

from archon.irgen import macros


class MacroABITestCase(unittest.TestCase):
    def compiler(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")
        return command

    def strict_compilers(self):
        commands = []
        for executable in ("gcc", "clang"):
            if shutil.which(executable):
                commands.append([executable])
        if not commands:
            commands.append(self.compiler())
        return commands

    def compile(self, files, source, *, succeeds=True, command=None):
        command = command or self.compiler()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, text in files.items():
                (root / name).write_text(text, encoding="ascii")
            (root / "test.c").write_text(source, encoding="ascii")
            result = subprocess.run(
                [
                    *command,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-pedantic",
                    "test.c",
                    "-o",
                    "test",
                ],
                cwd=root,
                text=True,
                capture_output=True,
                check=False,
            )
            if succeeds:
                self.assertEqual(result.returncode, 0, result.stderr)
                run = subprocess.run(
                    [str(root / "test")],
                    cwd=root,
                    text=True,
                    capture_output=True,
                    check=False,
                )
                self.assertEqual(run.returncode, 0, run.stderr)
            else:
                self.assertNotEqual(result.returncode, 0)
            return result


class EscapingAndIDs(MacroABITestCase):
    def test_identifier_encoding_is_injective_reversible_and_safe(self):
        values = (
            "",
            "ADD",
            "a-b",
            "a_b",
            "9lives",
            'x)\n#error "owned"\n(',
            "λ/雪/🙂",
            "__reserved",
        )
        encoded = [macros.c_identifier(value) for value in values]
        self.assertEqual(len(encoded), len(set(encoded)))
        for value, token in zip(values, encoded):
            with self.subTest(value=value):
                self.assertRegex(token, r"^[A-Za-z][A-Za-z0-9_]*$")
                self.assertEqual(macros.decode_identifier(token), value)
                self.assertNotIn("#", token)
                self.assertNotIn("\n", token)
        with self.assertRaisesRegex(macros.MacroABIError, "prefix"):
            macros.c_identifier("x", prefix="_reserved")

    def test_string_round_trip_property_and_trigraph_hardening(self):
        randomizer = random.Random(0xA7C0)
        alphabet = (
            string.ascii_letters
            + string.digits
            + string.punctuation
            + " \t\r\n"
            + "λ雪🙂"
        )
        values = [
            "",
            "ordinary",
            '"\\\n#error MALICIOUS',
            "??/ trigraph ??= still data",
            "nul utility only:\0after",
        ]
        for _ in range(250):
            values.append(
                "".join(
                    randomizer.choice(alphabet)
                    for _ in range(randomizer.randrange(0, 60))
                )
            )
        for value in values:
            with self.subTest(value=value):
                literal = macros.c_string(value)
                self.assertTrue(literal.isascii())
                self.assertNotIn("??", literal)
                self.assertEqual(macros.unescape_c_string(literal), value)

    def test_stable_ids_are_order_independent_framed_and_nonzero(self):
        left = macros.stable_id(
            "test",
            {"b": [2, 3], "a": 1},
            "payload",
        )
        right = macros.stable_id(
            "test",
            {"a": 1, "b": [2, 3]},
            "payload",
        )
        self.assertEqual(left, right)
        self.assertNotEqual(
            macros.stable_id("test", "ab", "c"),
            macros.stable_id("test", "a", "bc"),
        )
        self.assertNotEqual(left, 0)
        self.assertLessEqual(left, macros.UINT64_MAX)
        with self.assertRaisesRegex(macros.MacroABIError, "canonical JSON"):
            macros.stable_id("test", float("nan"))


class StreamValidation(MacroABITestCase):
    def program_rows(self):
        block0 = macros.stable_id("program.block", "entry")
        block1 = macros.stable_id("program.block", "later")
        operation0 = macros.stable_id("program.op", "use-later")
        operation1 = macros.stable_id("program.op", "definition")
        value0 = macros.stable_id("program.value", "result")
        value1 = macros.stable_id("program.value", "forward")
        return [
            # Deliberately shuffled.  The emitter has one canonical order.
            macros.Record(
                "SUCCESSOR",
                operation0,
                0,
                block1,
                "ARCHON_EDGE_NORMAL",
            ),
            macros.Record("OPERAND", operation0, 0, value1, 0),
            macros.Record(
                "OP",
                operation1,
                block1,
                0,
                "constant",
                value1,
                "ARCHON_TYPE_BITS",
                32,
                0,
                0,
                0,
                0,
            ),
            macros.Record("BLOCK", block1, 1, "later", 0),
            macros.Record(
                "OP",
                operation0,
                block0,
                0,
                "consume",
                value0,
                "ARCHON_TYPE_BITS",
                32,
                1,
                0,
                1,
                0,
            ),
            macros.Record("BLOCK", block0, 0, "entry", 0),
        ]

    def test_program_determinism_forward_references_and_round_trip(self):
        rows = self.program_rows()
        first = macros.emit_program(rows, stream_id="sample")
        second = macros.emit_program(list(reversed(rows)), stream_id="sample")
        self.assertEqual(first, second)
        parsed = macros.parse_stream(first)
        self.assertEqual(parsed.artifact, macros.ArtifactKind.PROGRAM)
        self.assertEqual(parsed.declared_counts["block_count"], 2)
        self.assertEqual(parsed.declared_counts["operation_count"], 2)
        self.assertEqual(macros.emit_stream(parsed), first)

    def test_program_literal_symbol_text_and_typed_attribute_children(self):
        block = macros.stable_id("program.block", "entry")
        operation = macros.stable_id("program.op", "call")
        rows = [
            macros.Record("BLOCK", block, 0, "entry", 0),
            macros.Record(
                "OP",
                operation,
                block,
                0,
                "call_with_metadata",
                0,
                "ARCHON_TYPE_VOID",
                0,
                3,
                2,
                0,
                0,
            ),
            macros.Record(
                "LITERAL",
                operation,
                0,
                "ARCHON_TYPE_UINT",
                129,
                "0x100000000000000000000000000000001",
                0,
            ),
            macros.Record(
                "SYMBOL",
                operation,
                1,
                "ARCHON_SYMBOL_FUNCTION",
                'callee")\n#error NOT_A_DIRECTIVE',
                -8,
                0,
            ),
            macros.Record(
                "TEXT",
                operation,
                2,
                "ARCHON_TEXT_ASSEMBLY",
                "call {callee}; ??/ remains text",
                0,
            ),
            macros.Record(
                "ATTRIBUTE",
                operation,
                0,
                "alignment",
                "ARCHON_TYPE_UINT",
                32,
                "ARCHON_ATTRIBUTE_UNSIGNED_INTEGER",
                "16",
                0,
            ),
            macros.Record(
                "ATTRIBUTE",
                operation,
                1,
                "known_bits",
                "ARCHON_TYPE_BITS",
                8,
                "ARCHON_ATTRIBUTE_BITS",
                "A5",
                0,
            ),
        ]
        text = macros.emit_program(list(reversed(rows)), stream_id="rich-program")
        parsed = macros.parse_stream(text)
        self.assertEqual(macros.emit_stream(parsed), text)
        by_macro = {row.macro: row for row in parsed.records if row.macro != "ARCHON_PROGRAM_ATTRIBUTE"}
        self.assertEqual(by_macro["ARCHON_PROGRAM_LITERAL"].args[4], "1" + "0" * 31 + "1")
        self.assertEqual(
            by_macro["ARCHON_PROGRAM_SYMBOL"].args[3],
            'callee")\n#error NOT_A_DIRECTIVE',
        )
        self.assertNotIn("\n#error NOT_A_DIRECTIVE", text)

        duplicate_index = rows + [
            macros.Record(
                "TEXT",
                operation,
                0,
                "ARCHON_TEXT_PLAIN",
                "duplicate operand zero",
                0,
            )
        ]
        with self.assertRaisesRegex(macros.MacroABIError, "operand child indices"):
            macros.emit_program(duplicate_index)

    def test_named_records_and_string_ids(self):
        text = macros.emit_program(
            [
                {
                    "macro": "BLOCK",
                    "id": "entry",
                    "ordinal": 0,
                    "name": "entry",
                    "flags": 0,
                },
                {
                    "macro": "OP",
                    "id": "return",
                    "block_id": "entry",
                    "ordinal": 0,
                    "opcode": "return",
                    "result_id": 0,
                    "type": "ARCHON_TYPE_VOID",
                    "width": 0,
                    "operand_count": 0,
                    "attribute_count": 0,
                    "successor_count": 0,
                    "flags": 0,
                },
            ]
        )
        parsed = macros.parse_stream(text)
        self.assertEqual(len(parsed.records), 2)
        self.assertIsInstance(parsed.records[0].args[0], int)

    def test_empty_pipeline_is_linked_deterministic_and_round_trips(self):
        self.assertIs(macros.PIPELINE_STAGE, False)
        context_a = {
            "ir": "anf",
            "architecture_names": ["A", "B"],
            "options": {"z": 2, "a": 1},
        }
        context_b = {
            "options": {"a": 1, "z": 2},
            "architecture_names": ["A", "B"],
            "ir": "anf",
        }
        outputs = macros.emit(context_a)
        self.assertEqual(outputs, macros.emit(context_b))
        self.assertEqual(
            set(outputs),
            {
                "MacroABI.h",
                "Program.i",
                "MIR.i",
                "InstrMap.i",
                "Scheduled.i",
                "RegMap.i",
                "Regallocated.i",
            },
        )
        streams = {
            name: macros.parse_stream(text)
            for name, text in outputs.items()
            if name.endswith(".i")
        }
        self.assertEqual(
            streams["MIR.i"].metadata["program_stream_id"],
            streams["Program.i"].stream_id,
        )
        self.assertEqual(
            streams["InstrMap.i"].metadata["mir_stream_id"],
            streams["MIR.i"].stream_id,
        )
        self.assertEqual(
            streams["Scheduled.i"].metadata["instr_map_stream_id"],
            streams["InstrMap.i"].stream_id,
        )
        self.assertEqual(
            streams["RegMap.i"].metadata["scheduled_stream_id"],
            streams["Scheduled.i"].stream_id,
        )
        self.assertEqual(
            streams["RegMap.i"].metadata["mir_stream_id"],
            streams["MIR.i"].stream_id,
        )
        self.assertEqual(
            streams["Regallocated.i"].metadata["reg_map_stream_id"],
            streams["RegMap.i"].stream_id,
        )
        for stream in streams.values():
            self.assertEqual(stream.records, ())

    def test_all_artifact_emitters_and_count_validation(self):
        samples = (
            macros.emit_program(),
            macros.emit_mir(),
            macros.emit_instr_map(),
            macros.emit_scheduled(),
            macros.emit_reg_map(),
            macros.emit_regallocated(),
        )
        for text in samples:
            with self.subTest(text=text[:40]):
                self.assertIsInstance(macros.validate_stream(text), macros.MacroStream)
        tampered = macros.emit_program().replace(
            "UINT32_C(0), UINT32_C(0))",
            "UINT32_C(1), UINT32_C(0))",
            1,
        )
        with self.assertRaisesRegex(
            macros.MacroABIError,
            r"block_count=1.*body contains 0",
        ):
            macros.validate_stream(tampered)

    def test_arity_type_range_reference_and_order_diagnostics(self):
        block = macros.stable_id("program.block", "entry")
        operation = macros.stable_id("program.op", "op")
        cases = (
            (
                [macros.Record("BLOCK", block, 0, "missing flags")],
                r"Program\.i: record 1.*wrong arity.*got 3.*expected 4",
            ),
            (
                [macros.Record("BLOCK", block, -1, "entry", 0)],
                r"argument 'ordinal'.*outside",
            ),
            (
                [
                    macros.Record("BLOCK", block, 0, "entry", 0),
                    macros.Record(
                        "OP",
                        operation,
                        block,
                        0,
                        "bad",
                        0,
                        "ARCHON_TYPE_VOID",
                        0,
                        1,
                        0,
                        0,
                        0,
                    ),
                ],
                r"operand child indices.*must be \[0\]",
            ),
            (
                [
                    macros.Record("BLOCK", block, 0, "entry", 0),
                    macros.Record(
                        "OP",
                        operation,
                        block,
                        0,
                        "bad",
                        0,
                        "NOT_A_TYPE",
                        0,
                        0,
                        0,
                        0,
                        0,
                    ),
                ],
                r"argument 'type'.*expected one of",
            ),
        )
        for rows, message in cases:
            with self.subTest(message=message):
                with self.assertRaisesRegex(macros.MacroABIError, message):
                    macros.emit_program(rows)

        canonical = macros.make_stream(
            "program",
            [
                macros.Record("BLOCK", block, 0, "entry", 0),
                macros.Record(
                    "OP",
                    operation,
                    block,
                    0,
                    "return",
                    0,
                    "ARCHON_TYPE_VOID",
                    0,
                    0,
                    0,
                    0,
                    0,
                ),
            ],
        )
        reversed_stream = macros.MacroStream(
            canonical.artifact,
            canonical.stream_id,
            tuple(reversed(canonical.records)),
            canonical.metadata,
        )
        with self.assertRaisesRegex(macros.MacroABIError, "canonical deterministic order"):
            macros.validate_stream(reversed_stream)

    def test_zero_distance_and_loop_carried_mir_dependencies(self):
        block = macros.stable_id("mir.block", "b")
        vreg = macros.stable_id("mir.vreg", "v")
        first = macros.stable_id("mir.instruction", "first")
        second = macros.stable_id("mir.instruction", "second")
        valid = [
            macros.Record("BLOCK", block, 0, "b", 0),
            macros.Record("VREG", vreg, 0, "GPR", 64, 0),
            macros.Record(
                "INSTRUCTION",
                first,
                block,
                0,
                "def",
                1,
                0,
                0,
                0,
                2,
                0,
            ),
            macros.Record("DEF", first, 0, vreg, ""),
            macros.Record(
                "INSTRUCTION",
                second,
                block,
                1,
                "use",
                0,
                1,
                0,
                0,
                1,
                0,
            ),
            macros.Record("USE", second, 0, vreg, ""),
            # A normal same-iteration edge must point backward.
            macros.Record(
                "DEPENDENCY",
                second,
                0,
                first,
                "ARCHON_DEP_DATA",
                "ARCHON_LATENCY_KNOWN",
                3,
                0,
            ),
            # A loop-carried recurrence may point to a later instruction.
            macros.Record(
                "DEPENDENCY",
                first,
                0,
                second,
                "ARCHON_DEP_DATA",
                "ARCHON_LATENCY_KNOWN",
                5,
                1,
            ),
            # Unknown latency is explicit and cannot silently become one.
            macros.Record(
                "DEPENDENCY",
                first,
                1,
                first,
                "ARCHON_DEP_ORDER",
                "ARCHON_LATENCY_UNKNOWN",
                0,
                2,
            ),
        ]
        text = macros.emit_mir(list(reversed(valid)))
        parsed = macros.parse_stream(text)
        self.assertEqual(macros.emit_stream(parsed), text)
        dependencies = [
            row for row in parsed.records
            if row.macro == "ARCHON_MIR_DEPENDENCY"
        ]
        self.assertEqual({row.args[6] for row in dependencies}, {0, 1, 2})
        self.assertIn("ARCHON_LATENCY_UNKNOWN", text)

        invalid_zero_distance = [
            row for row in valid
            if row.macro != "ARCHON_MIR_DEPENDENCY"
        ]
        invalid_zero_distance[2] = macros.Record(
            "INSTRUCTION", first, block, 0, "def", 1, 0, 0, 0, 1, 0
        )
        invalid_zero_distance[4] = macros.Record(
            "INSTRUCTION", second, block, 1, "use", 0, 1, 0, 0, 0, 0
        )
        invalid_zero_distance.append(
            macros.Record(
                "DEPENDENCY",
                first,
                0,
                second,
                "ARCHON_DEP_DATA",
                "ARCHON_LATENCY_KNOWN",
                1,
                0,
            )
        )
        with self.assertRaisesRegex(macros.MacroABIError, "zero-distance.*earlier"):
            macros.emit_mir(invalid_zero_distance)

        invalid_unknown = list(valid)
        invalid_unknown[-1] = macros.Record(
            "DEPENDENCY",
            first,
            1,
            first,
            "ARCHON_DEP_ORDER",
            "ARCHON_LATENCY_UNKNOWN",
            1,
            2,
        )
        with self.assertRaisesRegex(macros.MacroABIError, "unknown.*must carry zero"):
            macros.emit_mir(invalid_unknown)

    def test_schedule_regions_preserve_ii_and_sat_or_fallback_producer(self):
        block0 = macros.stable_id("mir.block", "loop.sat")
        block1 = macros.stable_id("mir.block", "loop.fallback")
        region0 = macros.stable_id("scheduled.region", "loop.sat")
        region1 = macros.stable_id("scheduled.region", "loop.fallback")
        entry0 = macros.stable_id("instr-map.entry", "sat.op")
        entry1 = macros.stable_id("instr-map.entry", "fallback.op")
        instruction0 = macros.stable_id("scheduled.instruction", "sat.op")
        instruction1 = macros.stable_id("scheduled.instruction", "fallback.op")
        rows = [
            macros.Record(
                "REGION",
                region0,
                0,
                block0,
                2,
                "ARCHON_SCHEDULE_SAT",
                0,
            ),
            macros.Record(
                "REGION",
                region1,
                1,
                block1,
                5,
                "ARCHON_SCHEDULE_FALLBACK",
                0,
            ),
            macros.Record(
                "INSTRUCTION",
                instruction0,
                0,
                entry0,
                region0,
                block0,
                0,
                0,
                0,
            ),
            macros.Record(
                "INSTRUCTION",
                instruction1,
                1,
                entry1,
                region1,
                block1,
                7,
                1,
                0,
            ),
        ]
        text = macros.emit_scheduled(list(reversed(rows)))
        parsed = macros.parse_stream(text)
        self.assertEqual(parsed.declared_counts["region_count"], 2)
        regions = [
            row for row in parsed.records
            if row.macro == "ARCHON_SCHEDULED_REGION"
        ]
        self.assertEqual(
            {(row.args[3], row.args[4]) for row in regions},
            {
                (2, "ARCHON_SCHEDULE_SAT"),
                (5, "ARCHON_SCHEDULE_FALLBACK"),
            },
        )
        self.assertEqual(macros.emit_stream(parsed), text)

        wrong_block = list(rows)
        wrong_block[-1] = macros.Record(
            "INSTRUCTION",
            instruction1,
            1,
            entry1,
            region1,
            block0,
            7,
            1,
            0,
        )
        with self.assertRaisesRegex(macros.MacroABIError, "does not match region"):
            macros.emit_scheduled(wrong_block)

    def test_secd_like_reg_map_liveness_and_event_stream(self):
        physical = {
            name: macros.stable_id("register", name)
            for name in ("S", "E", "C", "D")
        }
        virtual = {
            name: macros.stable_id("mir.vreg", name)
            for name in ("stack", "environment", "control", "dump")
        }
        scheduled = {
            name: macros.stable_id("scheduled.instruction", name)
            for name in ("load", "apply", "return")
        }
        rows = []
        for ordinal, name in enumerate(("S", "E", "C", "D")):
            rows.append(
                macros.Record(
                    "REGISTER",
                    physical[name],
                    ordinal,
                    name,
                    "SECD",
                    64,
                    0,
                )
            )
        rows.extend(
            [
                macros.Record(
                    "ALIAS",
                    macros.stable_id("register.alias", "SP"),
                    0,
                    "SP",
                    physical["S"],
                    0,
                    64,
                    "ARCHON_WRITE_REPLACE",
                ),
                macros.Record(
                    "RESERVED",
                    macros.stable_id("reg-map.reserved", "D"),
                    0,
                    physical["D"],
                    "dump-chain runtime register",
                    0,
                ),
            ]
        )
        for ordinal, (name, vreg) in enumerate(virtual.items()):
            rows.append(macros.Record("VREG", vreg, ordinal, "SECD", 64, 0))
            rows.append(
                macros.Record(
                    "INTERVAL",
                    macros.stable_id("reg-map.interval", name),
                    ordinal,
                    vreg,
                    ordinal,
                    ordinal + 4,
                    0,
                )
            )
            rows.append(
                macros.Record(
                    "EVENT",
                    macros.stable_id("reg-map.event", name),
                    ordinal,
                    tuple(scheduled.values())[ordinal % 3],
                    ordinal,
                    "ARCHON_REG_EVENT_DEF" if ordinal == 0 else "ARCHON_REG_EVENT_USE",
                    vreg,
                    0,
                )
            )
        rows.append(
            macros.Record(
                "FIXED",
                macros.stable_id("reg-map.fixed", "control"),
                0,
                virtual["control"],
                physical["C"],
                0,
            )
        )
        text = macros.emit_reg_map(
            list(reversed(rows)),
            target="SECD",
            scheduled_stream_id="scheduled",
            mir_stream_id="mir",
        )
        parsed = macros.parse_stream(text)
        self.assertEqual(parsed.declared_counts["register_count"], 4)
        self.assertEqual(parsed.declared_counts["vreg_count"], 4)
        self.assertEqual(parsed.declared_counts["interval_count"], 4)
        self.assertEqual(parsed.declared_counts["event_count"], 4)
        self.assertEqual(parsed.declared_counts["reserved_count"], 1)
        self.assertNotEqual(parsed.metadata["scheduled_stream_id"], 0)
        self.assertNotEqual(parsed.metadata["mir_stream_id"], 0)
        self.assertEqual(macros.emit_stream(parsed), text)

        outside = list(rows)
        event_index = next(
            index for index, row in enumerate(outside)
            if row.macro == "EVENT"
        )
        event = outside[event_index]
        outside[event_index] = macros.Record(
            "EVENT",
            event.args[0],
            event.args[1],
            event.args[2],
            macros.UINT64_MAX,
            event.args[4],
            event.args[5],
            event.args[6],
        )
        with self.assertRaisesRegex(macros.MacroABIError, "outside every live interval"):
            macros.emit_reg_map(outside)

        invalid_interval = list(rows)
        interval_index = next(
            index
            for index, row in enumerate(invalid_interval)
            if row.macro == "INTERVAL"
        )
        interval = invalid_interval[interval_index]
        invalid_interval[interval_index] = macros.Record(
            "INTERVAL",
            interval.args[0],
            interval.args[1],
            interval.args[2],
            interval.args[3],
            interval.args[3],
            interval.args[5],
        )
        with self.assertRaisesRegex(macros.MacroABIError, "nonempty and half-open"):
            macros.emit_reg_map(invalid_interval)

        invalid_reserved = list(rows)
        reserved_index = next(
            index
            for index, row in enumerate(invalid_reserved)
            if row.macro == "RESERVED"
        )
        reserved = invalid_reserved[reserved_index]
        invalid_reserved[reserved_index] = macros.Record(
            "RESERVED",
            reserved.args[0],
            reserved.args[1],
            macros.stable_id("register", "missing"),
            reserved.args[3],
            reserved.args[4],
        )
        with self.assertRaisesRegex(macros.MacroABIError, "unknown register ID"):
            macros.emit_reg_map(invalid_reserved)

        invalid_event_vreg = list(rows)
        event_index = next(
            index
            for index, row in enumerate(invalid_event_vreg)
            if row.macro == "EVENT"
        )
        event = invalid_event_vreg[event_index]
        invalid_event_vreg[event_index] = macros.Record(
            "EVENT",
            event.args[0],
            event.args[1],
            event.args[2],
            event.args[3],
            event.args[4],
            macros.stable_id("mir.vreg", "missing"),
            event.args[6],
        )
        with self.assertRaisesRegex(macros.MacroABIError, "unknown virtual register ID"):
            macros.emit_reg_map(invalid_event_vreg)


class C99Contract(MacroABITestCase):
    def test_empty_artifacts_compile_under_strict_c99(self):
        outputs = macros.emit({"architecture_names": ["target"]})
        source = """#include "MacroABI.h"
#include "Program.i"
#include "MIR.i"
#include "InstrMap.i"
#include "Scheduled.i"
#include "RegMap.i"
#include "Regallocated.i"
int main(void) { return 0; }
"""
        for command in self.strict_compilers():
            with self.subTest(compiler=command[0]):
                self.compile(outputs, source, command=command)

    def test_multi_pass_partial_callbacks_and_malicious_tokens(self):
        block = macros.stable_id("program.block", "entry")
        operation = macros.stable_id("program.op", "return")
        hostile_name = 'entry")\\\n#error ARCHON_INJECTION\n/* ??/ λ'
        hostile_opcode = 'ret) \n#error ARCHON_OPCODE'
        program = macros.emit_program(
            [
                macros.Record("BLOCK", block, 0, hostile_name, 0),
                macros.Record(
                    "OP",
                    operation,
                    block,
                    0,
                    hostile_opcode,
                    0,
                    "ARCHON_TYPE_VOID",
                    0,
                    0,
                    0,
                    0,
                    0,
                ),
            ]
        )
        self.assertNotIn("\n#error ARCHON_INJECTION", program)
        self.assertNotIn("#error ARCHON_OPCODE", program)
        files = {
            "MacroABI.h": macros.macro_abi_header(),
            "Program.i": program,
        }
        source = """#include "MacroABI.h"
#include <stddef.h>
#include <string.h>

#define ARCHON_PROGRAM_BLOCK(ID_, ORDINAL_, NAME_, FLAGS_) NAME_,
static const char *const block_names[] = {
#include "Program.i"
};
#undef ARCHON_PROGRAM_BLOCK

#define ARCHON_PROGRAM_OP(...) + 1
static const int operation_count = 0
#include "Program.i"
;
#undef ARCHON_PROGRAM_OP

#define ARCHON_PROGRAM_OP(ID_, BLOCK_, ORDINAL_, OPCODE_, RESULT_, TYPE_, \\
                          WIDTH_, OPERANDS_, ATTRIBUTES_, SUCCESSORS_, FLAGS_) \\
    #OPCODE_,
static const char *const opcode_tokens[] = {
#include "Program.i"
};
#undef ARCHON_PROGRAM_OP

int main(void) {
    return operation_count != 1 ||
           block_names[0] == NULL ||
           strstr(block_names[0], "#error ARCHON_INJECTION") == NULL ||
           opcode_tokens[0] == NULL ||
           strchr(opcode_tokens[0], '#') != NULL;
}
"""
        for command in self.strict_compilers():
            with self.subTest(compiler=command[0]):
                self.compile(files, source, command=command)

    def test_header_and_stream_reject_incompatible_versions(self):
        files = macros.emit({})
        header_result = self.compile(
            files,
            """#define ARCHON_MACRO_ABI_VERSION 0x00020000UL
#include "MacroABI.h"
int main(void) { return 0; }
""",
            succeeds=False,
        )
        self.assertIn("incompatible", header_result.stderr)

        stream_result = self.compile(
            files,
            """#include "MacroABI.h"
#undef ARCHON_PROGRAM_SCHEMA_VERSION
#define ARCHON_PROGRAM_SCHEMA_VERSION 99UL
#include "Program.i"
int main(void) { return 0; }
""",
            succeeds=False,
        )
        self.assertIn("schema version", stream_result.stderr)


if __name__ == "__main__":
    unittest.main()
