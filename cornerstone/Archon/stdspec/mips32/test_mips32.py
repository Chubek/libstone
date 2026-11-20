"""Register effects and encoding boundaries for the MIPSel integer subset."""

from pathlib import Path
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent
MASK = (1 << 32) - 1


class Mips32SubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.target = Target(compile_file(HERE / "root.arch"))

    def test_register_effects(self):
        registers = [0, MASK, 0x8000FFFF, 1, 0x7FFFFFFF] + [0] * 27
        for assembly, dest, result in (
            ("nor r1, r2, r3", 1, 0x7FFF0000),
            ("andi r2, r2, 65535", 2, 0xFFFF),
            ("ori r6, zero, 32768", 6, 0x8000),
            ("xori r7, r2, 65535", 7, 0x80000000),
            ("lui r8, 32768", 8, 0x80000000),
            ("lui r8, 65535", 8, 0xFFFF0000),
            ("lui r0, 65535", 0, 0),
            ("sll r3, r3, 31", 3, 0x80000000),
            ("sll r9, r2, 1", 9, 0x0001FFFE),
            ("srl r9, r2, 31", 9, 1),
            ("sra r9, r2, 31", 9, MASK),
            ("sra r9, r4, 31", 9, 0),
            ("sra r9, r2, 0", 9, 0x8000FFFF),
            ("sll r0, r0, 0", 0, 0),
        ):
            with self.subTest(assembly=assembly):
                inst = self.target.assemble(assembly)
                self.assertEqual(self.target.decode(self.target.encode(*inst)), inst)
                expected = registers.copy()
                expected[dest] = result
                self.assertEqual(self.target.execute(*inst, registers), expected)

    def test_rejects_outside_subset(self):
        for assembly in ("andi r1, r2, -1", "lui r1, 65536", "sll r1, r2, 32",
                         "srl r1, r2, -1", "addi r1, r2, 1"):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)
        for word in (
            0x3C2DFFFF,  # Nonzero rs in the LUI form.
            0x00318002,  # ROTR, not SRL.
            0x00430820,  # Trapping ADD, not ADDU.
        ):
            with self.subTest(word=f"{word:08x}"), self.assertRaises(ArchonError):
                self.target.decode(word.to_bytes(4, "little"))


if __name__ == "__main__":
    unittest.main()
