"""Register effects and encoding boundaries for the AArch64 integer subset."""

from pathlib import Path
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent
MASK = (1 << 64) - 1


class AArch64SubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.target = Target(compile_file(HERE / "root.arch"))

    def test_register_effects(self):
        registers = [0, MASK, 2, 3, 0xF0, 0xCC] + [0] * 26
        for assembly, dest, result in (
            ("bic x5, x4, x5", 5, 0x30),
            ("orn x6, x4, x5", 6, (MASK ^ 0xCC) | 0xF0),
            ("eon x7, x4, x5", 7, (MASK ^ 0xCC) ^ 0xF0),
            ("eon x7, x4, xzr", 7, MASK ^ 0xF0),
            ("bic xzr, x1, xzr", 31, 0),
            ("madd x3, x1, x2, x3", 3, 1),
            ("msub x3, x1, x2, x3", 3, 5),
            ("msub x8, x2, x3, xzr", 8, MASK - 5),
            ("madd xzr, x2, x3, x1", 31, 0),
        ):
            with self.subTest(assembly=assembly):
                inst = self.target.assemble(assembly)
                self.assertEqual(self.target.decode(self.target.encode(*inst)), inst)
                expected = registers.copy()
                expected[dest] = result
                self.assertEqual(self.target.execute(*inst, registers), expected)

    def test_rejects_other_encoding_forms(self):
        for word in (
            0x8A320630,  # bic x16, x17, x18, lsl #1
            0x8A720230,  # bic x16, x17, x18, lsr #0
            0xEA320230,  # bics x16, x17, x18
            0x0A320230,  # bic w16, w17, w18
            0x1B1B7359,  # madd w25, w26, w27, w28
        ):
            with self.subTest(word=f"{word:08x}"), self.assertRaises(ArchonError):
                self.target.decode(word.to_bytes(4, "little"))

    def test_sp_is_distinct_from_xzr(self):
        initial = self.target.new_state(registers=[3] * 32, scalars={"SP": 0x1000})
        result = self.target.execute_state(*self.target.assemble("add x1, xzr, x2"), initial)
        self.assertEqual(result.registers[1], 3)
        self.assertEqual(result.registers[31], 0)
        self.assertEqual(result.scalars, {"SP": 0x1000})
        result = self.target.execute_state(*self.target.assemble("add xzr, x1, x2"), result)
        self.assertEqual(result.registers[31], 0)
        self.assertEqual(result.scalars, {"SP": 0x1000})
        for spelling in ("sp", "SP"):
            with self.subTest(spelling=spelling), self.assertRaises(ArchonError):
                self.target.assemble(f"add {spelling}, x1, x2")


if __name__ == "__main__":
    unittest.main()
