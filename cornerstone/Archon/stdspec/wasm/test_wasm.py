"""Golden encodings and scope checks for the WebAssembly 1.0 opcode subset."""

import hashlib
from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent


class WasmSubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = database.loads(database.dumps(compile_file(HERE / "root.arch")))
        cls.target = Target(cls.db)
        lines = (HERE / "encoding-golden.tsv").read_text(encoding="ascii").splitlines()
        cls.vectors = [line.split("\t") for line in lines if line and not line.startswith("#")]

    def test_golden_round_trips(self):
        self.assertEqual(
            hashlib.sha256((HERE / "encoding-golden.tsv").read_bytes()).hexdigest(),
            "918beec1e380153e6b02737df3b3a434e8a0d4db9882532cbaea06b6c34a30e0",
        )
        self.assertEqual(len(self.vectors), 128)
        self.assertEqual(len({assembly for _, assembly in self.vectors}), len(self.vectors))
        self.assertEqual(len({bytes.fromhex(golden) for golden, _ in self.vectors}),
                         len(self.vectors))
        for golden, assembly in self.vectors:
            with self.subTest(assembly=assembly):
                encoded = bytes.fromhex(golden)
                self.assertEqual(len(encoded), 1)
                name, operands = self.target.assemble(assembly)
                self.assertEqual(operands, {})
                self.assertEqual(self.target.encode(name, operands), encoded)
                self.assertEqual(self.target.decode(encoded), (name, operands))
                self.assertEqual(self.target.disassemble(encoded), assembly)

    def test_rejects_every_other_opcode(self):
        covered = {int(golden, 16) for golden, _ in self.vectors}
        for opcode in sorted(set(range(256)) - covered):
            with self.subTest(opcode=f"{opcode:02x}"):
                with self.assertRaisesRegex(ArchonError, "unknown instruction encoding"):
                    self.target.decode(bytes([opcode]))

    def test_requires_one_instruction(self):
        # Even two supported bytes are outside the single-instruction API.
        for encoded in (b"", b"\x01\x1a", b"\x41\x00", b"\x00asm\x01\x00\x00\x00"):
            with self.subTest(encoded=encoded):
                with self.assertRaisesRegex(ArchonError, "expected 1 instruction bytes"):
                    self.target.decode(encoded)
        for assembly in ("i32.const 0", "local.get 0", "i32.add 1", "drop unused0",
                         "i32.add nop", "(i32.add)", "(module)", "I32.ADD",
                         "return 0", "select (result i32)", "memory.size", "end",
                         "i32.extend8_s", "i32.trunc_sat_f32_s"):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)

    def test_basic_stack_execution_and_rollback(self):
        empty = self.target.new_state()
        self.assertEqual(empty.registers, [])
        name, operands = self.target.assemble("nop")
        self.assertEqual(self.target.execute_state(name, operands, empty), empty)

        state = self.target.new_state(stacks={
            "OperandStack": [0xFEDCBA9876543210],
            "OperandTypes": [2],
        })
        name, operands = self.target.assemble("drop")
        result = self.target.execute_state(name, operands, state)
        self.assertEqual(result.stacks, {"OperandStack": [], "OperandTypes": []})
        self.assertEqual(state.stacks["OperandStack"], [0xFEDCBA9876543210])
        self.assertEqual(state.stacks["OperandTypes"], [2])

        malformed = self.target.new_state(stacks={
            "OperandStack": [],
            "OperandTypes": [1],
        })
        snapshot = malformed.copy()
        with self.assertRaisesRegex(ArchonError, "OperandStack.*underflow"):
            self.target.execute_state(name, operands, malformed)
        self.assertEqual(malformed, snapshot)

    def test_encoding_only_entries_reject_execution(self):
        executable = {
            row["assembly"] for row in self.db["instructions"]
            if row["semantics"] is not None
        }
        self.assertEqual(executable, {"drop", "nop"})
        for _, assembly in self.vectors:
            if assembly in executable:
                continue
            with self.subTest(assembly=assembly):
                name, operands = self.target.assemble(assembly)
                with self.assertRaisesRegex(ArchonError, "no executable semantics"):
                    self.target.execute_state(name, operands, self.target.new_state())


if __name__ == "__main__":
    unittest.main()
