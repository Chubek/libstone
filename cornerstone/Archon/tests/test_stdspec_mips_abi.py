"""MIPS32 O32 facts at the source and strict ArchDB boundaries."""

from pathlib import Path
import json
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen.native_target import resolve_native_target


ROOT = Path(__file__).resolve().parents[1] / "stdspec" / "mips32" / "root.arch"


class MipsO32FactsTests(unittest.TestCase):
    def test_profile_and_deterministic_archdb_round_trip(self):
        db = compile_file(ROOT)
        text = database.dumps(db)
        loaded = database.loads(text)
        self.assertEqual(database.dumps(loaded), text)
        self.assertEqual(database.dumps(compile_file(ROOT)), text)
        self.assertEqual((loaded["endian"], loaded["registers"]["width"]), ("little", 32))
        self.assertEqual(loaded["abi_facts"], {
            "schema_version": 1,
            "name": "O32NonPIC",
            "register_file": "IntegerRegisters",
            "args": [4, 5, 6, 7],
            "result": 2,
            "stack_pointer": 29,
            "return_address": 31,
            "stack_align": 8,
            "stack_slot_bytes": 4,
            "stack_arg_offset": 16,
            "stack_growth": "downward",
            "caller_saved": [*range(2, 16), 24, 25, 31],
            "callee_saved": [*range(16, 24), 28, 29, 30],
            "unallocatable": [0, 1, 26, 27, 28, 29],
        })
        with self.assertRaisesRegex(ArchonError, "architecture is 'MIPS32'"):
            resolve_native_target(loaded)

    def test_fixed_and_call_clobbered_gp_cannot_be_serialized(self):
        db = compile_file(ROOT)
        abi = db["abi_facts"]
        # PIC has a fixed-purpose but clobbered gp, not the published
        # non-PIC gp that is fixed-purpose and preserved.
        abi["callee_saved"].remove(28)
        abi["caller_saved"].insert(-1, 28)
        with self.assertRaisesRegex(ArchonError, "caller_saved overlaps"):
            database.dumps(db)
        with self.assertRaisesRegex(ArchonError, "caller_saved overlaps"):
            database.loads(json.dumps(db))


if __name__ == "__main__":
    unittest.main()
