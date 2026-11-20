"""Check the default ABI coverage of all standard specification entry points."""

from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
TARGETS = (
    ("riscv/rv32i.arch", 1, "ILP32", "add x1, x2, x3", "b3003100"),
    ("riscv/rv64i.arch", 1, "LP64", "add x1, x2, x3", "b3003100"),
    ("riscv/rv64im.arch", 1, "LP64", "mul x1, x2, x3", "b3003102"),
    ("aarch64/root.arch", 2, "AAPCS64", "add x1, x2, x3", "4100038b"),
    ("mips32/root.arch", 1, "O32NonPIC", "addu r1, r2, r3", "21084300"),
    ("wasm/root.arch", None, None, "i32.add", "6a"),
    ("amd64/root.arch", 2, "SysVAMD64", "mov rax, rbx", "4889d8"),
    ("jvm/root.arch", None, None, "iconst_m1", "02"),
    ("clr/root.arch", None, None, "ldc.i4.m1", "15"),
    ("pdp11/root.arch", 2, "UnixV7Scalar", "mov r1, r2", "4210"),
)


class StandardSpecificationABICoverage(unittest.TestCase):
    def test_abi_and_encoding_round_trip(self):
        for spec, version, abi, assembly, hex_bytes in TARGETS:
            with self.subTest(spec=spec):
                db = compile_file(STDSPEC / spec)
                text = database.dumps(db)
                restored = database.loads(text)
                self.assertEqual(restored, db)
                self.assertEqual(database.dumps(restored), text)

                self.assertEqual(restored["version"], 1)
                for expected, key in ((1, "abi_facts"), (2, "abi_facts_v2")):
                    if version == expected:
                        self.assertEqual(restored[key]["name"], abi)
                        self.assertEqual(restored[key]["schema_version"], expected)
                    else:
                        self.assertNotIn(key, restored)

                target = Target(restored)
                data = bytes.fromhex(hex_bytes)
                instruction = target.assemble(assembly)
                self.assertEqual(target.encode(*instruction), data)
                self.assertEqual(target.decode(data), instruction)
                self.assertEqual(target.assemble(target.disassemble(data)), instruction)


if __name__ == "__main__":
    unittest.main()
