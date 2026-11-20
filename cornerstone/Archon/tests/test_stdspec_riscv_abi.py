"""Check the public scalar ABI facts of the legacy RISC-V databases."""

from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file


RISCV = Path(__file__).resolve().parents[1] / "stdspec" / "riscv"


class RiscVStdSpecAbiTests(unittest.TestCase):
    def test_integer_psabi_facts(self):
        for spec, architecture, name, slot_bytes in (
            ("rv32i.arch", "RISCV32", "ILP32", 4),
            ("rv64i.arch", "RISCV64", "LP64", 8),
        ):
            with self.subTest(spec=spec):
                db = compile_file(RISCV / spec)
                db = database.loads(database.dumps(db))
                self.assertEqual(db["architecture"], architecture)
                # psABI integer register and procedure calling conventions,
                # expressed in physical x-register numbers.
                self.assertEqual(db["abi_facts"], {
                    "schema_version": 1,
                    "name": name,
                    "register_file": "IntegerRegisters",
                    "args": [10, 11, 12, 13, 14, 15, 16, 17],
                    "result": 10,
                    "stack_pointer": 2,
                    "return_address": 1,
                    "stack_align": 16,
                    "stack_slot_bytes": slot_bytes,
                    "stack_arg_offset": 0,
                    "stack_growth": "downward",
                    "caller_saved": [1, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31],
                    "callee_saved": [2, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27],
                    "unallocatable": [0, 2, 3, 4],
                })


if __name__ == "__main__":
    unittest.main()
