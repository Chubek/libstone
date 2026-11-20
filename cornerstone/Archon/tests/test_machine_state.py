"""Core language, ASIR, and Python runtime coverage for non-register state."""

from copy import deepcopy
import json
from pathlib import Path
from tempfile import TemporaryDirectory
from textwrap import dedent
import unittest

from archon.compiler import compile_file
from archon.database import dumps, loads
from archon.diagnostics import ArchonError
from archon.frontend import parse
from archon.runtime import MachineState, Target


SOURCE = """
architecture Stateful {
    endian = little;
    address_width = 16;

    type Word = bits<16>;
    type Flag = bits<1>;
    state PC: Word;
    state Carry: Flag;
    stack Values: Word { capacity = 3; }
    memory RAM { size = 8; }

    format Op: bits<8> {
        field opcode: bits<8> @ [7:0];
    }

    instruction MIX {
        encoding Op { opcode = 1; }
        assembly "mix";
        semantics {
            let rhs = pop(Values);
            let lhs = pop(Values);
            let word = load<bits<16>>(RAM, read_state(PC));
            push(Values, lhs +% rhs +% word);
            store<bits<16>>(RAM, read_state(PC), rhs);
            parallel {
                PC := read_state(PC) +% 2;
                Carry := extract(rhs, 0, 0);
                store<bits<16>>(RAM, 4, lhs);
            }
        }
    }

    instruction DUP_SECOND {
        encoding Op { opcode = 2; }
        assembly "dup.second";
        semantics { push(Values, peek(Values, 1)); }
    }

    instruction FAIL {
        encoding Op { opcode = 3; }
        assembly "fail";
        semantics {
            PC := read_state(PC) +% 2;
            let value = pop(Values);
            store<bits<16>>(RAM, 7, value);
        }
    }

    instruction CONFLICT {
        encoding Op { opcode = 4; }
        assembly "conflict";
        semantics {
            parallel {
                store<bits<16>>(RAM, 0, 0x0102);
                store<bits<16>>(RAM, 1, 0x0304);
            }
        }
    }

    instruction NOP {
        encoding Op { opcode = 5; }
        assembly "nop";
        semantics {}
    }
}
"""


class MachineStateTests(unittest.TestCase):
    def compile(self, source=SOURCE):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        path = Path(temporary.name) / "root.arch"
        path.write_text(dedent(source), encoding="utf-8")
        return compile_file(path)

    def test_frontend_parses_bounded_state_declarations(self):
        tree = parse(dedent(SOURCE), "state.arch")
        declarations = {
            (row["kind"], row["name"]): row for row in tree["declarations"]
        }
        self.assertEqual(
            declarations["state", "PC"]["spec"], {"kind": "named", "arg": "Word"},
        )
        self.assertEqual(declarations["stack", "Values"]["capacity"], 3)
        self.assertEqual(
            declarations["stack", "Values"]["spec"],
            {"kind": "named", "arg": "Word"},
        )
        self.assertEqual(declarations["memory", "RAM"]["size"], 8)

        for declaration, message in (
            ("stack S: bits<8> {}", "requires 'capacity'"),
            ("stack S: bits<8> { capacity=1; capacity=2; }", "duplicate stack member"),
            ("memory M {}", "requires 'size'"),
            ("memory M { capacity=1; }", "unsupported memory member"),
        ):
            with self.subTest(declaration=declaration), self.assertRaisesRegex(
                ArchonError, message,
            ):
                parse(f"module Bad {{ {declaration} }}", "bad.arch")

    def test_compiler_emits_typed_state_and_effects_without_registers(self):
        db = self.compile()
        self.assertIsNone(db["registers"])
        self.assertEqual(db["state"], {
            "scalars": [
                {"name": "Carry", "type": {"kind": "bits", "width": 1}},
                {"name": "PC", "type": {"kind": "bits", "width": 16}},
            ],
            "stacks": [{
                "name": "Values",
                "element_type": {"kind": "bits", "width": 16},
                "capacity": 3,
            }],
            "memories": [{"name": "RAM", "size": 8}],
        })
        mix = next(row for row in db["instructions"] if row["name"] == "MIX")
        self.assertEqual(mix["semantics"]["word_width"], 16)
        self.assertEqual(mix["semantics"]["effects"], {
            "reads": [],
            "writes": [],
            "state_reads": ["PC"],
            "state_writes": ["PC", "Carry"],
            "stack_reads": ["Values"],
            "stack_writes": ["Values"],
            "memory_reads": ["RAM"],
            "memory_writes": ["RAM"],
        })

        for original, replacement, message in (
            ("capacity = 3", "capacity = 0", "capacity must be between"),
            ("size = 8", "size = 0", "size must be between"),
            ("push(Values, lhs", "push(Missing, lhs", "unknown stack"),
            (
                "load<bits<16>>(RAM, read_state(PC))",
                "load<bits<7>>(RAM, read_state(PC))",
                "multiple of eight",
            ),
            ("parallel {\n                PC :=", "parallel {\n                push(Values, 0);\n                PC :=",
             "push is not permitted"),
        ):
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                self.compile(SOURCE.replace(original, replacement))

    def test_archdb_rejects_malformed_state_semir(self):
        db = self.compile()
        self.assertEqual(loads(dumps(db)), db)

        invalid = []
        duplicate = deepcopy(db)
        duplicate["state"]["stacks"][0]["name"] = "PC"
        invalid.append((duplicate, "duplicate architectural state name"))

        bad_capacity = deepcopy(db)
        bad_capacity["state"]["stacks"][0]["capacity"] = 0
        invalid.append((bad_capacity, "capacity must be between"))

        missing_effect = deepcopy(db)
        mix = next(row for row in missing_effect["instructions"] if row["name"] == "MIX")
        del mix["semantics"]["effects"]["memory_reads"]
        invalid.append((missing_effect, r"missing .*memory_reads"))

        parallel_pop = deepcopy(db)
        mix = next(row for row in parallel_pop["instructions"] if row["name"] == "MIX")
        parallel = mix["semantics"]["statements"][-1]
        parallel["statements"][0]["value"] = {
            "op": "pop", "name": "Values",
            "type": {"kind": "bits", "width": 16},
        }
        invalid.append((parallel_pop, "pop is not permitted"))

        for malformed, message in invalid:
            with self.subTest(message=message):
                for operation in (dumps, lambda value: loads(json.dumps(value))):
                    with self.assertRaisesRegex(ArchonError, message):
                        operation(malformed)

    def test_runtime_machine_state_endianness_and_transactionality(self):
        for endian, initial, result_value, stored in (
            ("little", b"\x01\x02\x00\x00\x00\x00\x00\x00", 543,
             b"\x14\x00\x00\x00\x0a\x00\x00\x00"),
            ("big", b"\x01\x02\x00\x00\x00\x00\x00\x00", 288,
             b"\x00\x14\x00\x00\x00\x0a\x00\x00"),
        ):
            with self.subTest(endian=endian):
                db = self.compile(SOURCE.replace("endian = little", f"endian = {endian}"))
                target = Target(loads(dumps(db)))
                state = target.new_state(
                    scalars={"PC": 0},
                    stacks={"Values": [10, 20]},
                    memories={"RAM": initial},
                )
                result = target.execute_state("MIX", {}, state)
                self.assertIsInstance(result, MachineState)
                self.assertEqual(result.registers, [])
                self.assertEqual(result.scalars, {"Carry": 0, "PC": 2})
                self.assertEqual(result.stacks, {"Values": [result_value]})
                self.assertEqual(bytes(result.memories["RAM"]), stored)
                self.assertEqual(state.stacks, {"Values": [10, 20]})
                self.assertEqual(bytes(state.memories["RAM"]), initial)

                duplicated = target.execute_state(
                    "DUP_SECOND", {}, target.new_state(stacks={"Values": [3, 5]}),
                )
                self.assertEqual(duplicated.stacks["Values"], [3, 5, 3])

                before = target.new_state(
                    scalars={"PC": 1}, stacks={"Values": [0xBEEF]},
                )
                for instruction, error in (
                    ("FAIL", "out of bounds"),
                    ("CONFLICT", "conflicting parallel stores"),
                ):
                    with self.subTest(instruction=instruction), self.assertRaisesRegex(
                        ArchonError, error,
                    ):
                        target.execute_state(instruction, {}, before)
                    self.assertEqual(before.scalars["PC"], 1)
                    self.assertEqual(before.stacks["Values"], [0xBEEF])
                    self.assertEqual(bytes(before.memories["RAM"]), bytes(8))

                # The register-list entry point remains valid for a
                # registerless, stateful target when no non-register input is
                # needed by the instruction.
                self.assertEqual(target.execute("NOP", {}, []), [])
                program = target.encode("NOP", {}) * 2
                linear = target.execute_program_state(
                    program, target.new_state(scalars={"PC": 7}),
                )
                self.assertEqual(linear.scalars["PC"], 7)  # no implicit PC update
                self.assertEqual(target.execute_program(program, []), [])

    def test_runtime_validates_public_machine_state_shape(self):
        target = Target(self.compile())
        for kwargs, message in (
            ({"scalars": {"Missing": 0}}, "unknown scalar state"),
            ({"scalars": {"Carry": 2}}, "invalid value"),
            ({"stacks": {"Values": [0, 1, 2, 3]}}, "invalid depth"),
            ({"memories": {"RAM": b"\0"}}, "exactly 8 bytes"),
        ):
            with self.subTest(kwargs=kwargs), self.assertRaisesRegex(ArchonError, message):
                target.new_state(**kwargs)

        state = target.new_state()
        state.memories["RAM"] = b"\0"
        with self.assertRaisesRegex(ArchonError, "exactly 8 bytes"):
            target.execute_state("MIX", {}, state)


if __name__ == "__main__":
    unittest.main()
