"""Scalar ABI metadata across source and ArchDB boundaries."""

from copy import deepcopy
from hashlib import sha256
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen.native_target import resolve_native_target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
ABI = "abi_facts_v2"


class ABIFactsV2Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.arm = compile_file(STDSPEC / "aarch64/root.arch")
        cls.amd = compile_file(STDSPEC / "amd64/root.arch")
        cls.pdp = compile_file(STDSPEC / "pdp11/root.arch")

    def test_aapcs64_scalar(self):
        self.assertEqual(self.arm[ABI], {
            "schema_version": 2, "name": "AAPCS64", "register_file": "IntegerRegisters",
            "args": list(range(8)), "result": 0,
            "stack_pointer": {"kind": "state", "name": "SP"},
            "return_address": {"kind": "register", "index": 30},
            "stack_align": 16, "stack_entry_remainder": 0, "stack_arg_offset": 0,
            "stack_slot_bytes": 8, "stack_growth": "downward",
            "caller_saved": [*range(18), 30], "callee_saved": list(range(19, 30)),
            "unallocatable": [18, 31],
        })
        self.assertEqual(self.arm["state"]["scalars"], [
            {"name": "SP", "type": {"kind": "bits", "width": 64}},
        ])
        self.assertEqual(self.arm["registers"]["zero"], 31)
        self.assertEqual(self.arm["registers"]["aliases"]["xzr"], 31)
        self.assertNotIn("sp", self.arm["registers"]["aliases"])

    def test_sysv_amd64_scalar(self):
        self.assertEqual(self.amd[ABI], {
            "schema_version": 2, "name": "SysVAMD64", "register_file": "IntegerRegisters",
            "args": [7, 6, 2, 1, 8, 9], "result": 0,
            "stack_pointer": {"kind": "register", "index": 4},
            "return_address": {"kind": "stack_slot", "offset": 0, "size_bytes": 8},
            "stack_align": 16, "stack_entry_remainder": 8, "stack_arg_offset": 8,
            "stack_slot_bytes": 8, "stack_growth": "downward",
            "caller_saved": [0, 1, 2, 6, 7, 8, 9, 10, 11],
            "callee_saved": [3, 4, 5, 12, 13, 14, 15], "unallocatable": [4],
        })
        self.assertEqual(self.amd["registers"]["count"], 16)
        for index in range(8, 16):
            self.assertEqual(self.amd["registers"]["aliases"][f"r{index}"], index)
        for inst in self.amd["instructions"]:
            for operand in inst["operands"]:
                self.assertEqual(operand["register_range"], [0, 7])

    def test_unix_v7_pdp11_word_scalar(self):
        self.assertEqual(self.pdp[ABI], {
            "schema_version": 2, "name": "UnixV7Scalar", "register_file": "Registers",
            "args": [], "result": 0, "stack_pointer": {"kind": "register", "index": 6},
            "return_address": {"kind": "stack_slot", "offset": 0, "size_bytes": 2},
            "stack_align": 2, "stack_entry_remainder": 0, "stack_arg_offset": 2,
            "stack_slot_bytes": 2, "stack_growth": "downward",
            "caller_saved": [0, 1], "callee_saved": [2, 3, 4, 5, 6],
            "unallocatable": [5, 6, 7],
        })

    def test_no_new_native_qualification_and_unchanged_rv64im(self):
        for db in (self.arm, self.amd, self.pdp):
            with self.subTest(arch=db["architecture"]), self.assertRaisesRegex(
                ArchonError, "architecture is",
            ):
                resolve_native_target(db)
        legacy = compile_file(STDSPEC / "riscv/rv64im.arch")
        text = database.dumps(legacy)
        self.assertEqual(sha256(text.encode("ascii")).hexdigest(),
                         "bb6494cdce96de6df80c3a2bbd5cb134617e14e4827ca318d1f999f958cb91cb")
        self.assertNotIn(ABI, legacy)
        self.assertEqual(resolve_native_target(legacy)["abi_facts"], legacy["abi_facts"])
        del legacy["abi_facts"]
        text = database.dumps(legacy)
        self.assertEqual(database.dumps(database.loads(text)), text)

    def assert_bad_db(self, db, message):
        with self.assertRaisesRegex(ArchonError, message):
            database.dumps(db)
        with self.assertRaisesRegex(ArchonError, message):
            database.loads(json.dumps(db))

    def test_rejects_bad_stack_locations_and_register_roles(self):
        for changes, message in (
            ({"schema_version": 1}, r"unsupported ArchDB abi_facts_v2 schema"),
            ({"schema_version": True}, r"schema_version must be a JSON integer"),
            ({"stack_pointer": 4}, r"stack_pointer must be a JSON object"),
            ({"stack_pointer": {"kind": "register", "index": True}}, r"stack_pointer.index.*integer"),
            ({"stack_pointer": {"kind": "register", "index": 3}}, r"stack_pointer.*preserved, fixed"),
            ({"return_address": {"kind": "state", "name": "SP"}}, r"invalid location kind"),
            ({"return_address": {"kind": "stack_slot", "offset": 0}}, r"missing .*size_bytes"),
            ({"return_address": {"kind": "stack_slot", "offset": True, "size_bytes": 8}},
             r"return_address.offset.*integer"),
            ({"return_address": {"kind": "stack_slot", "offset": 0, "size_bytes": 4}},
             r"size_bytes must store one architectural address"),
            ({"return_address": {"kind": "stack_slot", "offset": 1, "size_bytes": 8}},
             r"stack slot must be outside"),
            ({"return_address": {"kind": "stack_slot", "offset": 16, "size_bytes": 8}},
             r"stack slot must be outside"),
            ({"return_address": {"kind": "stack_slot", "offset": -(1 << 31), "size_bytes": 8}},
             r"outside the supported byte offset range"),
            ({"return_address": {"kind": "stack_slot", "offset": 0, "size_bytes": 8, "index": 0}},
             r"unknown abi_facts_v2.return_address member"),
            ({"stack_align": 12}, r"stack_align.*power of two"),
            ({"stack_entry_remainder": -1}, r"stack_entry_remainder must be in"),
            ({"stack_entry_remainder": 16}, r"stack_entry_remainder must be in"),
            ({"stack_entry_remainder": False}, r"stack_entry_remainder.*JSON integer"),
            ({"stack_entry_remainder": 7}, r"stack_arg_offset.*slot-aligned"),
            ({"stack_arg_offset": 0}, r"stack slot must be outside"),
            ({"stack_arg_offset": 4}, r"stack_arg_offset.*slot-aligned"),
            ({"args": [7, 7]}, r"duplicate register"),
            ({"args": [16]}, r"args\[0\].*index"),
            ({"args": [12]}, r"argument and result registers must be caller-saved"),
            ({"unallocatable": [4, 8]}, r"caller_saved overlaps"),
            ({"callee_saved": [3, 4, 5, 12, 13, 14]}, r"classify every register"),
            ({"red_zone": 128}, r"unknown abi_facts_v2 member"),
        ):
            with self.subTest(changes=changes):
                db = deepcopy(self.amd)
                db[ABI].update(changes)
                self.assert_bad_db(db, message)
        db = deepcopy(self.amd)
        db["abi_facts"] = {}
        self.assert_bad_db(db, "cannot contain both abi_facts and abi_facts_v2")
        db = deepcopy(self.amd)
        db["registers"]["width"] = 32
        self.assert_bad_db(db, r"stack_pointer must use an address-width register")

    def test_rejects_bad_scalar_and_link_register_references(self):
        for description, mutate, message in (
            ("undeclared SP", lambda db: db["state"]["scalars"].clear(),
             r"stack_pointer.name must name a declared scalar"),
            ("wrong SP width", lambda db: db["state"]["scalars"][0]["type"].update(width=32),
             r"stack_pointer must use address-width"),
            ("signed SP", lambda db: db["state"]["scalars"][0]["type"].update(kind="sint"),
             r"stack_pointer must use address-width"),
            ("XZR as SP", lambda db: db[ABI].update(stack_pointer={"kind": "register", "index": 31}),
             r"stack_pointer must be a preserved, fixed register"),
            ("XZR as link", lambda db: db[ABI]["return_address"].update(index=31),
             r"return_address must be a caller-saved register"),
            ("LR also argument", lambda db: db[ABI]["args"].append(30),
             r"argument and result registers.*distinct"),
            ("LR also result", lambda db: db[ABI].update(result=30),
             r"argument and result registers.*distinct"),
            ("missing fixed zero", lambda db: db[ABI]["unallocatable"].remove(31),
             r"zero register as fixed only"),
        ):
            with self.subTest(description=description):
                db = deepcopy(self.arm)
                mutate(db)
                self.assert_bad_db(db, message)

    def test_source_resolution_and_roles_are_deterministic(self):
        spec = STDSPEC / "amd64"
        source = (spec / "root.arch").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "root.arch"
            shutil.copyfile(spec / "integer.arch", path.with_name("integer.arch"))
            for before, after, message in (
                ("stack_pointer = register(rsp);", "stack_pointer = rsp;", r"location kind"),
                ("return_address = stack_slot(0, 8);",
                 "return_address = stack_slot(0);", r"expected ','"),
                ("stack_entry_remainder = 8;", "", r"requires stack_entry_remainder"),
                ("schema_version = 2;", "schema_version = 1;", r"unsupported abi_facts_v2 schema"),
                ("result = rax;", "result = gpr16;", r"unknown register alias or numbered register"),
                ("result = rax;", "result = rax; result = rcx;", r"duplicate abi_facts_v2 member"),
                ("[rdi, rsi, rdx, rcx, r8, r9]", "[rdi, gpr7]", r"duplicate register in .*args"),
            ):
                path.write_text(source.replace(before, after), encoding="utf-8")
                with self.subTest(after=after), self.assertRaisesRegex(ArchonError, message):
                    compile_file(path)
            path.write_text(source.replace("result = rax;", "result = gpr0;")
                            .replace("register(rsp)", "register(gpr4)")
                            .replace("[rbx, rsp, rbp,", "[rbp, rbx, rsp,"), encoding="utf-8")
            self.assertEqual(database.dumps(compile_file(path)), database.dumps(self.amd))
            path.write_text(source.replace("[rdi, rsi,", "[rsi, rdi,"), encoding="utf-8")
            self.assertEqual(compile_file(path)[ABI]["args"][:2], [6, 7])

    def test_v1_numbered_spellings_and_aliases_serialize_identically(self):
        spec = STDSPEC / "riscv"
        source = (spec / "rv64im.arch").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "rv64im.arch"
            for module in ("base.arch", "rv64im_backend.arch"):
                shutil.copyfile(spec / module, path.with_name(module))
            path.write_text(source.replace("[a0, a1,", "[x10, x11,")
                            .replace("result = a0;", "result = x10;")
                            .replace("stack_pointer = sp;", "stack_pointer = x2;")
                            .replace("return_address = ra;", "return_address = x1;"), encoding="utf-8")
            self.assertEqual(database.dumps(compile_file(path)),
                             database.dumps(compile_file(spec / "rv64im.arch")))

    def test_relative_stack_layout_is_generic(self):
        db = deepcopy(self.amd)
        # A toy ABI with a four-byte PC and an eight-byte argument slot does
        # not have a slot-aligned SP; the first argument's actual address is.
        db["address_width"] = db["registers"]["width"] = 32
        db["instructions"] = []
        db[ABI].update(
            stack_entry_remainder=12, stack_arg_offset=4,
            return_address={"kind": "stack_slot", "offset": 0, "size_bytes": 4},
        )
        self.assertEqual(database.loads(database.dumps(db)), db)
        db[ABI].update(
            stack_growth="upward", stack_entry_remainder=4, stack_arg_offset=-12,
            return_address={"kind": "stack_slot", "offset": -4, "size_bytes": 4},
        )
        self.assertEqual(database.loads(database.dumps(db)), db)

    def test_cli_inspection_reports_v2(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "arm.archdb"
            database.save(self.arm, path)
            result = subprocess.run(
                [sys.executable, "-m", "archon", "inspect", str(path)],
                cwd=STDSPEC.parent, check=True, text=True, capture_output=True, timeout=10,
            )
        self.assertIn("Scalar ABI facts: AAPCS64 (schema 2)", result.stdout)


if __name__ == "__main__":
    unittest.main()
