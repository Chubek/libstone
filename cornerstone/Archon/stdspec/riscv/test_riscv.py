"""Encoding goldens and concrete register effects for the shared ALU subset."""

from pathlib import Path
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent


def make_target(width):
    return Target(compile_file(HERE / f"rv{width}i.arch"))


class RiscVSubsetTests(unittest.TestCase):
    def test_encodings(self):
        lines = (HERE / "encoding-golden.tsv").read_text(encoding="ascii").splitlines()
        vectors = [line.split("\t") for line in lines if not line.startswith("#")]
        for width in (32, 64):
            target = make_target(width)
            for golden, assembly in vectors:
                with self.subTest(width=width, assembly=assembly):
                    expect = bytes.fromhex(golden)
                    inst = target.assemble(assembly)
                    self.assertEqual(target.encode(*inst), expect)
                    self.assertEqual(target.decode(expect), inst)
                    self.assertEqual(target.assemble(target.disassemble(expect)), inst)

    def test_rejects_outside_subset(self):
        for width in (32, 64):
            target = make_target(width)
            for imm in (-2049, 2048):
                with self.subTest(width=width, imm=imm), self.assertRaises(ArchonError):
                    target.assemble(f"addi x1, x2, {imm}")
            for imm in (-1, 1 << 20):
                with self.subTest(width=width, imm=imm), self.assertRaises(ArchonError):
                    target.assemble(f"lui x1, {imm}")
            for word in (0x00311093, 0x02000033, 0x0000001B):  # slli, mul, addiw
                with self.subTest(width=width, word=word), self.assertRaises(ArchonError):
                    target.decode(word.to_bytes(4, "little"))

    def test_register_effects(self):
        for width in (32, 64):
            target = make_target(width)
            all_ones = (1 << width) - 1
            # Assembly, initial x1, initial x2, destination register, result.
            cases = (
                ("add x1, x1, x2", 0xFFFFFFFF, 1, 1, 0 if width == 32 else 1 << 32),
                ("sub x3, x0, x2", 0, 1, 3, all_ones),
                ("xor x3, x1, x2", 0x55, 0x3C, 3, 0x69),
                ("or x3, x1, x2", 0x55, 0x3C, 3, 0x7D),
                ("and x3, x1, x2", 0x55, 0x3C, 3, 0x14),
                ("addi x3, x0, -2048", 0, 0, 3, all_ones - 2047),
                ("addi x0, x1, 2047", 42, 0, 0, 0),
                ("xori x3, x1, -1", 0x55, 0, 3, all_ones - 0x55),
                ("ori x3, x1, -2048", 0x55, 0, 3, all_ones - 2047 + 0x55),
                ("andi x3, x1, -16", all_ones, 0, 3, all_ones - 15),
                ("lui x3, 0", 0, 0, 3, 0),
                ("lui x3, 0x7ffff", 0, 0, 3, 0x7FFFF000),
                ("lui x3, 0x80000", 0, 0, 3, (1 << width) - (1 << 31)),
                ("lui x3, 0xfffff", 0, 0, 3, all_ones - 4095),
                ("lui x0, 0xfffff", 0, 0, 0, 0),
            )
            for assembly, x1, x2, dest, result in cases:
                with self.subTest(width=width, assembly=assembly):
                    regs = [0, x1, x2] + [0] * 29
                    expected = regs.copy()
                    expected[dest] = result
                    inst, operands = target.assemble(assembly)
                    self.assertEqual(list(target.execute(inst, operands, regs)), expected)

    def test_masked_register_shifts(self):
        for width in (32, 64):
            target = make_target(width)
            mask = (1 << width) - 1
            for mnemonic in ("sll", "srl", "sra"):
                inst, operands = target.assemble(f"{mnemonic} x2, x1, x2")
                for value in (0, 1, 1 << (width - 1), mask, (1 << (width - 1)) - 1):
                    signed = value if value < 1 << (width - 1) else value - (1 << width)
                    for shift in (0, 1, 31, 32, width - 1, width, width + 1, mask):
                        count = shift % width
                        expected = {"sll": value << count, "srl": value >> count,
                                    "sra": signed >> count}[mnemonic] & mask
                        registers = [0, value, shift] + [0] * 29
                        with self.subTest(width=width, mnemonic=mnemonic, value=value, shift=shift):
                            self.assertEqual(target.execute(inst, operands, registers)[2], expected)


if __name__ == "__main__":
    unittest.main()
