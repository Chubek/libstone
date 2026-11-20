"""ArchDB qualification and independently checked RV64IM provider facts."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
from random import Random
from shutil import copyfile
from tempfile import TemporaryDirectory
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen.native_target import resolve_native_target
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
RISCV = STDSPEC / "riscv"


def instruction(db, stem):
    return next(row for row in db["instructions"] if row["name"] == stem)


class NativeTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = compile_file(RISCV / "rv64im.arch")

    def test_native_contract_and_round_trip(self):
        db = database.loads(database.dumps(self.db))
        facts = resolve_native_target(db)
        self.assertEqual(facts["name"], "RISCV64IM")
        self.assertEqual(facts["abi"], "LP64")
        self.assertEqual(facts["abi_facts"], {
            "schema_version": 1, "register_file": "IntegerRegisters",
            "name": "LP64", "args": list(range(10, 18)), "result": 10, "stack_align": 16,
            "stack_pointer": 2, "return_address": 1, "stack_slot_bytes": 8,
            "stack_arg_offset": 0, "stack_growth": "downward",
            "caller_saved": [1, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 28, 29, 30, 31],
            "callee_saved": [2, 8, 9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27],
            "unallocatable": [0, 2, 3, 4],
        })
        self.assertEqual(facts["abi_facts"], db["abi_facts"])
        self.assertEqual(facts["fingerprint"], hashlib.sha256(
            database.dumps(db).encode("ascii")
        ).hexdigest())
        stems = (
            "ADD SUB MUL DIV REM DIVU REMU AND OR XOR SLL SRL SRA SLT SLTU "
            "ADDI SLLI LD SD JALR EBREAK BEQ BNE JAL AUIPC"
        ).split()
        self.assertEqual(facts["instructions"], {stem: stem.lower() for stem in stems})
        checks = facts["instruction_facts"]
        self.assertEqual({stem for stem, f in checks.items() if not f["in_db"]},
                         {"SD", "BEQ", "BNE", "JAL"})
        for stem in ("ADD", "SRA", "MUL", "SLT", "SLTU", "SLLI"):
            self.assertEqual(checks[stem]["evidence"], "exact-archsem")
        for stem in ("DIV", "LD", "EBREAK", "AUIPC"):
            self.assertEqual(checks[stem]["evidence"], "reviewed-isa-manifest")
            self.assertIsNone(instruction(db, stem)["semantics"])

        facts["instructions"]["ADD"] = "sub"
        facts["abi_facts"]["args"].clear()
        facts["abi_facts"]["caller_saved"].clear()
        checks["ADD"]["fields"].clear()
        self.assertEqual(resolve_native_target(db), resolve_native_target(self.db))

    def test_v1_database_without_abi_remains_readable_but_is_not_native(self):
        old = deepcopy(self.db)
        del old["abi_facts"]
        text = database.dumps(old)
        self.assertEqual(hashlib.sha256(text.encode("ascii")).hexdigest(),
                         "3e5dc650a0a5df6d05bb780535e8fe4e3f01aff85aff30e834a466734e530e1e")
        self.assertEqual(database.dumps(database.loads(text)), text)
        self.assertEqual(Target(database.loads(text)).encode("ADD", {"rd": 1, "rs1": 2, "rs2": 3}),
                         bytes.fromhex("b3003100"))
        rv64i = compile_file(RISCV / "rv64i.arch")
        self.assertEqual(rv64i["abi_facts"]["name"], "LP64")
        with self.assertRaisesRegex(ArchonError, "expected RISCV64IM"):
            resolve_native_target(rv64i)
        with self.assertRaisesRegex(ArchonError, "missing declarative abi_facts"):
            resolve_native_target(database.loads(text))

    def test_abi_facts_validation_at_db_boundaries(self):
        original = self.db["abi_facts"]
        for overrides, message in (
            ({"schema_version": 2}, "unsupported ArchDB abi_facts schema version"),
            ({"schema_version": True}, r"abi_facts\.schema_version.*JSON integer"),
            ({"name": "LP64\n"}, r"abi_facts\.name.*identifier"),
            ({"register_file": "GPR"}, r"abi_facts\.register_file"),
            ({"args": [10, 10]}, r"duplicate register in abi_facts\.args"),
            ({"args": [10, 32]}, r"abi_facts\.args\[1\].*index"),
            ({"args": [1]}, "argument and result registers"),
            ({"result": True}, r"abi_facts\.result.*JSON integer"),
            ({"stack_align": 24}, r"abi_facts\.stack_align.*power of two"),
            ({"stack_align": 4}, r"abi_facts\.stack_align.*smaller than"),
            ({"stack_arg_offset": 4}, r"abi_facts\.stack_arg_offset.*slot-aligned"),
            ({"stack_growth": "downward\n"}, r"abi_facts\.stack_growth"),
            ({"caller_saved": [2, *original["caller_saved"]]}, "must be sorted"),
            ({"caller_saved": [1, 2, *original["caller_saved"][1:]]}, "overlaps"),
            ({"callee_saved": original["callee_saved"][:-1]}, "classify every register"),
            ({"stack_pointer": 3}, "requires a preserved, fixed stack pointer"),
            ({"unallocatable": original["unallocatable"][1:]}, "zero register as fixed only"),
            ({"shadow_bytes": 0}, "unknown abi_facts member"),
        ):
            db = deepcopy(self.db)
            db["abi_facts"].update(overrides)
            with self.subTest(overrides=overrides):
                with self.assertRaisesRegex(ArchonError, message):
                    database.dumps(db)
                with self.assertRaisesRegex(ArchonError, message):
                    database.loads(json.dumps(db))
                with self.assertRaisesRegex(ArchonError, message):
                    resolve_native_target(db)
        for text, message in (
            ('"schema_version": 1, "schema_version": 1,', "duplicate JSON key"),
            ("", r"missing abi_facts\.schema_version"),
        ):
            db_text = database.dumps(self.db).replace('"schema_version": 1,', text)
            with self.assertRaisesRegex(ArchonError, message):
                database.loads(db_text)

    def test_well_formed_abi_mutations_do_not_qualify(self):
        original = self.db["abi_facts"]
        for key, value in (
            ("name", "LP64D"),
            ("register_file", "OtherRegisters"),
            ("args", original["args"][::-1]),
            ("result", 11),
            ("stack_align", 32),
            ("stack_slot_bytes", 16),
            ("stack_arg_offset", 8),
            ("stack_growth", "upward"),
            ("stack_pointer", 8),
            ("return_address", 5),
            ("caller_saved", [r for r in original["caller_saved"] if r != 28]),
            ("callee_saved", [r for r in original["callee_saved"] if r != 8]),
            ("unallocatable", [*original["unallocatable"], 8]),
        ):
            db = deepcopy(self.db)
            abi = db["abi_facts"]
            abi[key] = value
            if key == "register_file":
                db["registers"]["name"] = value
            if key == "stack_pointer":
                abi["unallocatable"] = [0, 3, 4, 8]
            if key == "caller_saved":
                # Reclassify t3 as callee-saved without violating the
                # format's structural rules.
                abi["callee_saved"].append(28)
            if key == "callee_saved":
                abi["unallocatable"].append(8)
            with self.subTest(key=key):
                db = database.loads(database.dumps(db))
                with self.assertRaisesRegex(ArchonError, rf"abi_facts\.{key}.*reviewed LP64"):
                    resolve_native_target(db)

    def test_abi_facts_source_resolution_and_determinism(self):
        source = (RISCV / "rv64im.arch").read_text(encoding="utf-8")
        with TemporaryDirectory() as directory:
            root = Path(directory) / "rv64im.arch"
            for module in ("base.arch", "rv64im_backend.arch"):
                copyfile(RISCV / module, root.with_name(module))
            for before, after, message in (
                ("schema_version = 1;", "schema_version = 2;", "unsupported abi_facts schema"),
                ("register_file = IntegerRegisters;", "register_file = GPR;", "unknown.*register file"),
                ("result = a0;", "result = bogus;", "unknown register alias"),
                ("result = a0;", "result = a0; result = a1;", "duplicate abi_facts member"),
                ("result = a0;", "", "abi_facts requires result"),
                ("stack_align = 16;", "stack_align = 12;", "stack_align.*power of two"),
                ("a6, a7];", "a6, a6];", "duplicate register"),
                ("schema_version = 1;", "schema_version = 1; extra = 0;",
                 "unsupported abi_facts member"),
            ):
                root.write_text(source.replace(before, after), encoding="utf-8")
                with self.subTest(after=after), self.assertRaisesRegex(ArchonError, message):
                    compile_file(root)

            root.write_text(source.replace("[zero, sp, gp, tp]", "[tp, gp, sp, zero]")
                            .replace("callee_saved = [sp, s0,", "callee_saved = [s0, sp,"),
                            encoding="utf-8")
            self.assertEqual(database.dumps(compile_file(root)), database.dumps(self.db))
            root.write_text(source.replace("args = [a0, a1,", "args = [a1, a0,"), encoding="utf-8")
            ordered = compile_file(root)
            self.assertEqual(ordered["abi_facts"]["args"][:2], [11, 10])
            with self.assertRaisesRegex(ArchonError, r"abi_facts\.args.*reviewed LP64"):
                resolve_native_target(ordered)

    def test_rejects_wrong_targets_and_configurations(self):
        for spec in ("riscv/rv32i.arch", "riscv/rv64i.arch", "wasm/root.arch"):
            with self.subTest(spec=spec), self.assertRaisesRegex(ArchonError, "architecture is"):
                resolve_native_target(compile_file(STDSPEC / spec))
        with self.assertRaisesRegex(ArchonError, "expected I and M"):
            resolve_native_target(compile_file(RISCV / "rv64im.arch", features=["I"]))
        with self.assertRaisesRegex(ArchonError, "invalid ArchDB"):
            resolve_native_target({})

    def test_rejects_mnemonic_lookalikes_and_unreviewed_db(self):
        def replace(stem, key, value):
            def apply(db):
                instruction(db, stem)[key] = value
            return apply

        for mutate, message in (
            (replace("ADD", "semantics", instruction(self.db, "SUB")["semantics"]),
             "ADD does not match the reviewed ArchSem"),
            (replace("SLT", "semantics", instruction(self.db, "SLTU")["semantics"]),
             "SLT does not match the reviewed ArchSem"),
            (replace("MUL", "semantics", None), "MUL does not match the reviewed ArchSem"),
            (replace("DIV", "value", 0x02005033), "DIV does not match.*encoding"),
            (replace("LD", "assembly", "ld {rd}, {imm}({rs1}) # unreviewed"),
             "LD does not match.*assembly"),
            (replace("ADDI", "fields", instruction(self.db, "ADDI")["fields"][::-1]),
             "ADDI does not match.*encoding"),
            (lambda db: db["registers"]["aliases"].update(sp=3), "GPR aliases"),
            (lambda db: db["registers"]["aliases"].update(a0=11), "GPR aliases"),
            (lambda db: db["registers"]["aliases"].update(s11=26), "GPR aliases"),
            (lambda db: db["registers"]["aliases"].update(x0=1), "database fingerprint"),
            (lambda db: db["instructions"].remove(instruction(db, "EBREAK")), "missing.*EBREAK"),
            (lambda db: db.update(unreviewed=True), "database fingerprint"),
        ):
            db = deepcopy(self.db)
            mutate(db)
            with self.subTest(message=message), self.assertRaisesRegex(ArchonError, message):
                resolve_native_target(db)

    def test_native_operand_bit_mappings(self):
        target = Target(self.db)
        facts = resolve_native_target(self.db)["instruction_facts"]
        # Checked against clang's RV64IM assembler and llvm-objdump.
        for stem, operands, golden in (
            ("MUL", {"rd": 1, "rs1": 2, "rs2": 3}, 0x023100B3),
            ("DIV", {"rd": 1, "rs1": 2, "rs2": 3}, 0x023140B3),
            ("DIVU", {"rd": 1, "rs1": 2, "rs2": 3}, 0x023150B3),
            ("REM", {"rd": 1, "rs1": 2, "rs2": 3}, 0x023160B3),
            ("REMU", {"rd": 1, "rs1": 2, "rs2": 3}, 0x023170B3),
            ("SLT", {"rd": 1, "rs1": 2, "rs2": 3}, 0x003120B3),
            ("SLTU", {"rd": 1, "rs1": 2, "rs2": 3}, 0x003130B3),
            ("SLLI", {"rd": 1, "rs1": 2, "shamt": 63}, 0x03F11093),
            ("LD", {"rd": 1, "rs1": 2, "imm": -16}, 0xFF013083),
            ("SD", {"rs2": 1, "rs1": 2, "imm": -16}, 0xFE113823),
            ("SD", {"rs2": 3, "rs1": 2, "imm": 2047}, 0x7E313FA3),
            ("JALR", {"rd": 1, "rs1": 2, "imm": -16}, 0xFF0100E7),
            ("AUIPC", {"rd": 1, "imm": 0x12345}, 0x12345097),
            ("EBREAK", {}, 0x00100073),
            ("BEQ", {"rs1": 2, "rs2": 3, "imm": -16}, 0xFE3108E3),
            ("BNE", {"rs1": 2, "rs2": 3, "imm": 2046}, 0x7E311F63),
            ("JAL", {"rd": 1, "imm": -1024}, 0xC01FF0EF),
            ("JAL", {"rd": 1, "imm": 4096}, 0x000010EF),
        ):
            with self.subTest(stem=stem, operands=operands):
                row = facts[stem]
                encoded = row["value"]
                for field in row["fields"]:
                    field_mask = (1 << field["width"]) - 1
                    payload = (operands[field["operand"]] >> field["operand_lsb"]) & field_mask
                    encoded |= payload << field["lsb"]
                self.assertEqual(encoded, golden)
                self.assertEqual(encoded & row["mask"], row["value"])
                if row["in_db"]:
                    self.assertEqual(target.encode(stem, operands), golden.to_bytes(4, "little"))
                else:
                    with self.assertRaises(ArchonError):
                        target.decode(golden.to_bytes(4, "little"))

    def test_comparison_and_modular_multiply_witnesses(self):
        target = Target(self.db)
        rng = Random(64)
        mask = (1 << 64) - 1
        sign = 1 << 63
        boundary = (0, 1, sign - 1, sign, sign + 1, mask)
        samples = [(a, b) for a in boundary for b in boundary]
        samples.extend((rng.getrandbits(64), rng.getrandbits(64)) for _ in range(128))
        operands = {"rd": 1, "rs1": 2, "rs2": 3}
        for a, b in samples:
            regs = [0, 0, a, b] + [0] * 28
            for stem, expected in (
                ("SLT", (a ^ sign) < (b ^ sign)), ("SLTU", a < b), ("MUL", a * b & mask),
            ):
                with self.subTest(stem=stem, a=a, b=b):
                    self.assertEqual(target.execute(stem, operands, regs)[1], int(expected))


if __name__ == "__main__":
    unittest.main()
