"""Encoding and register-state tests for the three-byte AMD64 subset."""

from pathlib import Path
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent
ALIASES = ("rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi")
MASK = (1 << 64) - 1


class AMD64SubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.target = Target(compile_file(HERE / "root.arch"))

    def test_golden_encodings(self):
        goldens = (HERE / "encoding-golden.tsv").read_text(encoding="ascii")
        for line in goldens.splitlines():
            if not line or line.startswith("#"):
                continue
            hex_bytes, assembly = line.split("\t")
            with self.subTest(assembly=assembly):
                data = bytes.fromhex(hex_bytes)
                inst = self.target.assemble(assembly)
                self.assertEqual(self.target.encode(*inst), data)
                self.assertEqual(self.target.decode(data), inst)
                self.assertEqual(self.target.assemble(self.target.disassemble(data)), inst)
        self.assertEqual(self.target.disassemble(bytes.fromhex("4889d8")), "mov gpr0, gpr3")

    def test_two_register_forms(self):
        original = [0x8000000000000000 + index for index in range(8)]
        for mnemonic, opcode in (("mov", 0x89), ("xchg", 0x87)):
            for lhs, lhs_alias in enumerate(ALIASES):
                for rhs, rhs_alias in enumerate(ALIASES):
                    assembly = f"{mnemonic} {lhs_alias}, {rhs_alias}"
                    with self.subTest(assembly=assembly):
                        name, operands = self.target.assemble(assembly)
                        data = bytes((0x48, opcode, 0xC0 | (rhs << 3) | lhs))
                        self.assertEqual(self.target.encode(name, operands), data)
                        self.assertEqual(self.target.decode(data), (name, operands))

                        registers = original.copy()
                        expected = original.copy()
                        expected[lhs] = original[rhs]
                        if mnemonic == "xchg":
                            expected[rhs] = original[lhs]
                        self.assertEqual(self.target.execute(name, operands, registers), expected)
                        self.assertEqual(registers, original)

    def test_one_register_forms(self):
        for mnemonic, encoding in (("not", (0xF7, 0xD0)), ("bswap", (0x0F, 0xC8))):
            opcode, reg_base = encoding
            for index, alias in enumerate(ALIASES):
                for value in (0, MASK, 0x0102030405060708, 0x8000FF00A589AB00):
                    with self.subTest(assembly=f"{mnemonic} {alias}", value=value):
                        name, operands = self.target.assemble(f"{mnemonic} {alias}")
                        data = bytes((0x48, opcode, reg_base | index))
                        self.assertEqual(self.target.encode(name, operands), data)
                        self.assertEqual(self.target.decode(data), (name, operands))

                        original = list(range(8))
                        original[index] = value
                        expected = original.copy()
                        if mnemonic == "not":
                            expected[index] = MASK ^ value
                        else:
                            expected[index] = int.from_bytes(value.to_bytes(8, "little"), "big")
                        self.assertEqual(self.target.execute(name, operands, original), expected)

    def test_rejects_encodings_outside_subset(self):
        for data in (
            "4989d8",   # REX.B: mov r8, rbx
            "4c89d8",   # REX.R: mov rax, r11
            "4089d8",   # No REX.W: mov eax, ebx
            "488bc3",   # Alternate MOV opcode for mov rax, rbx
            "488918",   # Memory destination
            "4863c3",   # MOVSXD: 32-bit source, 64-bit destination
            "48f7da",   # F7 /3: neg rdx
            "4801d8",   # ADD: writes flags
            "48ffd0",   # CALL rax: stack and RIP effects
            "f08710",   # LOCK, no REX.W, memory XCHG
            "480fc7",   # Not the BSWAP opcode
            "0fc8",     # 32-bit bswap eax, too short
            "4893",     # Accumulator short form for XCHG, too short
            "664889d8",  # Additional legacy prefix, too long
        ):
            with self.subTest(data=data), self.assertRaises(ArchonError):
                self.target.decode(bytes.fromhex(data))

    def test_rejects_assembly_outside_subset(self):
        for assembly in ("mov r8, rbx", "mov eax, ebx", "mov [rax], rbx", "mov rax, 1",
                         "movsxd rax, ebx", "lock xchg rax, rbx", "add rax, rbx",
                         "call rax"):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)


if __name__ == "__main__":
    unittest.main()
