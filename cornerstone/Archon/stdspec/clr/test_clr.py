"""Golden encodings and scope checks for the ECMA-335 CIL opcode subset."""

import hashlib
from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent


class CILSubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = database.loads(database.dumps(compile_file(HERE / "root.arch")))
        cls.target = Target(cls.db)
        lines = (HERE / "encoding-golden.tsv").read_text(encoding="ascii").splitlines()
        cls.vectors = [line.split("\t") for line in lines if line and not line.startswith("#")]

    def test_golden_round_trips(self):
        self.assertEqual(
            hashlib.sha256((HERE / "encoding-golden.tsv").read_bytes()).hexdigest(),
            "aaf813b37f4fc329b4f30b94b4194c8a35d3d8a978f764ea8a12e5657b97e539",
        )
        self.assertEqual(len(self.vectors), 68)
        self.assertEqual(len({assembly for _, assembly in self.vectors}), len(self.vectors))
        self.assertEqual(len({bytes.fromhex(golden) for golden, _ in self.vectors}),
                         len(self.vectors))
        for golden, assembly in self.vectors:
            with self.subTest(assembly=assembly):
                data = bytes.fromhex(golden)
                self.assertEqual(len(data), 1)
                name, operands = self.target.assemble(assembly)
                self.assertEqual(operands, {})
                self.assertEqual(self.target.encode(name, operands), data)
                self.assertEqual(self.target.decode(data), (name, operands))
                self.assertEqual(self.target.disassemble(data), assembly)

    def test_rejects_every_other_opcode(self):
        covered = {int(golden, 16) for golden, _ in self.vectors}
        for opcode in sorted(set(range(256)) - covered):
            with self.subTest(opcode=f"{opcode:02x}"):
                with self.assertRaisesRegex(ArchonError, "unknown instruction encoding"):
                    self.target.decode(bytes([opcode]))

    def test_rejects_unsupported_assembly_and_extra_bytes(self):
        for assembly in ("ldc.i4 -1", "ldc.i4.s 0", "ldc.i4.0 0", "add unused0",
                         "ldarg.0", "ceq", "volatile.", "ret", "ADD"):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)
        for data in (b"", b"\x16\x26", b"\x1f\x00", b"\xfe\x01", b"MZ"):
            with self.subTest(data=data):
                with self.assertRaisesRegex(ArchonError, "expected 1 instruction bytes"):
                    self.target.decode(data)

    def test_raw_program(self):
        assembly = "ldc.i4.2\nldc.i4.3\nadd.ovf\npop\n"
        data = bytes.fromhex("18 19 d6 26")
        self.assertEqual(self.target.assemble_program(assembly), data)
        self.assertEqual(self.target.disassemble_program(data), assembly)
        with self.assertRaisesRegex(ArchonError, "at byte address 0x1: unknown"):
            self.target.disassemble_program(b"\x00\xfe\x01")  # nop, then ceq

    def test_constants_generic_stack_operations_and_rollback(self):
        empty = self.target.new_state()
        self.assertEqual(empty.registers, [])
        name, operands = self.target.assemble("nop")
        self.assertEqual(self.target.execute_state(name, operands, empty), empty)

        cases = [("ldnull", 0, 5), ("ldc.i4.m1", 0xFFFFFFFF, 1)]
        cases.extend((f"ldc.i4.{value}", value, 1) for value in range(9))
        for assembly, value, kind in cases:
            state = self.target.new_state()
            name, operands = self.target.assemble(assembly)
            result = self.target.execute_state(name, operands, state)
            self.assertEqual(result.stacks["EvaluationStack"], [value])
            self.assertEqual(result.stacks["ValueKinds"], [kind])
            self.assertEqual(state, empty)

        state = self.target.new_state(stacks={
            "EvaluationStack": [0xFEDCBA9876543210],
            "ValueKinds": [3],
        })
        name, operands = self.target.assemble("dup")
        result = self.target.execute_state(name, operands, state)
        self.assertEqual(
            result.stacks["EvaluationStack"],
            [0xFEDCBA9876543210, 0xFEDCBA9876543210],
        )
        self.assertEqual(result.stacks["ValueKinds"], [3, 3])
        self.assertEqual(state.stacks["EvaluationStack"], [0xFEDCBA9876543210])
        self.assertEqual(state.stacks["ValueKinds"], [3])

        name, operands = self.target.assemble("pop")
        result = self.target.execute_state(name, operands, result)
        self.assertEqual(result, state)

        malformed = self.target.new_state(stacks={
            "EvaluationStack": [],
            "ValueKinds": [1],
        })
        snapshot = malformed.copy()
        name, operands = self.target.assemble("pop")
        with self.assertRaisesRegex(ArchonError, "EvaluationStack.*underflow"):
            self.target.execute_state(name, operands, malformed)
        self.assertEqual(malformed, snapshot)

        partial = self.target.new_state(stacks={
            "EvaluationStack": [0] * 1023,
            "ValueKinds": [1] * 1024,
        })
        snapshot = partial.copy()
        name, operands = self.target.assemble("dup")
        with self.assertRaisesRegex(ArchonError, "ValueKinds.*overflow"):
            self.target.execute_state(name, operands, partial)
        self.assertEqual(partial, snapshot)

    def test_encoding_only_entries_reject_execution(self):
        executable = {
            row["assembly"] for row in self.db["instructions"]
            if row["semantics"] is not None
        }
        self.assertEqual(executable, {
            "nop", "ldnull", "ldc.i4.m1", "ldc.i4.0", "ldc.i4.1",
            "ldc.i4.2", "ldc.i4.3", "ldc.i4.4", "ldc.i4.5",
            "ldc.i4.6", "ldc.i4.7", "ldc.i4.8", "dup", "pop",
        })
        for _, assembly in self.vectors:
            if assembly in executable:
                continue
            with self.subTest(assembly=assembly):
                name, operands = self.target.assemble(assembly)
                with self.assertRaisesRegex(ArchonError, "no executable semantics"):
                    self.target.execute_state(name, operands, self.target.new_state())


if __name__ == "__main__":
    unittest.main()
