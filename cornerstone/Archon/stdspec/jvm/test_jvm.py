"""Golden encodings and scope checks for the JVM opcode subset."""

import hashlib
from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent


class JVMSubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = database.loads(database.dumps(compile_file(HERE / "root.arch")))
        cls.target = Target(cls.db)
        lines = (HERE / "encoding-golden.tsv").read_text(encoding="ascii").splitlines()
        cls.vectors = [line.split("\t") for line in lines if line and not line.startswith("#")]

    def test_golden_round_trips(self):
        self.assertEqual(
            hashlib.sha256((HERE / "encoding-golden.tsv").read_bytes()).hexdigest(),
            "a367b66fd5f73e1dca3dba2dfbdeb272ac9db3c9f8f55a00cc47c970df5dd65f",
        )
        self.assertEqual(len(self.vectors), 81)
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
        for assembly in ("iconst -1", "iconst_m1 -1", "iadd unused0", "bipush 0",
                         "iload_0", "iinc 1 2", "wide iinc 1 2", "return", "IADD"):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)
        for data in (b"", b"\x03\x57", b"\x10\x00", b"\xca\xfe\xba\xbe"):
            with self.subTest(data=data):
                with self.assertRaisesRegex(ArchonError, "expected 1 instruction bytes"):
                    self.target.decode(data)

    def test_raw_program(self):
        assembly = "iconst_2\niconst_3\niadd\npop\n"
        data = bytes.fromhex("05 06 60 57")
        self.assertEqual(self.target.assemble_program(assembly), data)
        self.assertEqual(self.target.disassemble_program(data), assembly)
        with self.assertRaisesRegex(ArchonError, "at byte address 0x1: unknown"):
            self.target.disassemble_program(b"\x00\x10\x00")  # nop, then bipush 0

    def test_integer_constants_categories_and_rollback(self):
        empty = self.target.new_state()
        self.assertEqual(empty.registers, [])
        name, operands = self.target.assemble("nop")
        self.assertEqual(self.target.execute_state(name, operands, empty), empty)

        cases = (
            ("iconst_m1", 0xFFFFFFFF, 1),
            ("iconst_0", 0, 1),
            ("iconst_1", 1, 1),
            ("iconst_2", 2, 1),
            ("iconst_3", 3, 1),
            ("iconst_4", 4, 1),
            ("iconst_5", 5, 1),
            ("lconst_0", 0, 2),
            ("lconst_1", 1, 2),
        )
        for assembly, value, kind in cases:
            state = self.target.new_state()
            name, operands = self.target.assemble(assembly)
            result = self.target.execute_state(name, operands, state)
            self.assertEqual(result.stacks["OperandStack"], [value])
            self.assertEqual(result.stacks["OperandKinds"], [kind])
            self.assertEqual(state, empty)

        partial = self.target.new_state(stacks={
            "OperandStack": [0] * 1023,
            "OperandKinds": [1] * 1024,
        })
        snapshot = partial.copy()
        name, operands = self.target.assemble("iconst_0")
        with self.assertRaisesRegex(ArchonError, "OperandKinds.*overflow"):
            self.target.execute_state(name, operands, partial)
        self.assertEqual(partial, snapshot)

    def test_encoding_only_entries_reject_execution(self):
        executable = {
            row["assembly"] for row in self.db["instructions"]
            if row["semantics"] is not None
        }
        self.assertEqual(executable, {
            "nop", "iconst_m1", "iconst_0", "iconst_1", "iconst_2",
            "iconst_3", "iconst_4", "iconst_5", "lconst_0", "lconst_1",
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
