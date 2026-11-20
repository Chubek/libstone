"""Exercise the checkout CLI and the on-disk ArchDB boundary."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from archon import database
from archon.diagnostics import ArchonError


ROOT = Path(__file__).resolve().parents[1]
RV32 = ROOT / "stdspec" / "riscv" / "rv32i.arch"


class ArchonCLI(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.directory = Path(tmp.name)
        self.db = self.directory / "riscv.archdb"

    def cli(self, *args, stdin=None, ok=True):
        result = subprocess.run(
            [sys.executable, "-m", "archon", *map(str, args)],
            cwd=ROOT, input=stdin, capture_output=True, check=False,
        )
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        else:
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"Traceback", result.stderr)
        return result

    def compile_db(self):
        self.cli("compile", RV32, "-o", self.db)
        return database.load(self.db)

    def test_check_compile_inspect_and_serialization(self):
        self.assertEqual(self.cli("check", RV32).stdout, b"")
        db = self.compile_db()
        stored = self.db.read_bytes()
        self.assertEqual(db["version"], database.SCHEMA_VERSION)
        self.assertEqual(stored, self.cli("compile", RV32).stdout)
        self.assertEqual(stored, self.cli("inspect", self.db, "--json").stdout)
        self.assertEqual(stored.decode(), database.dumps(dict(reversed(list(db.items())))))
        self.assertEqual(database.loads(stored.decode()), db)
        summary = self.cli("inspect", self.db).stdout.decode()
        self.assertIn(db["architecture"], summary)
        self.assertIn("ADD (32 bits)", summary)

    def test_standalone_compiler_wrapper(self):
        result = subprocess.run(
            [str(ROOT / "tools" / "archonc"), str(RV32)],
            cwd=self.directory, capture_output=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, self.cli("compile", RV32).stdout)

    def test_assemble_disassemble_and_execute(self):
        self.compile_db()
        self.assertEqual(self.cli("asm", self.db, "add x1, x2, x3").stdout, b"b3003100\n")

        machine_code = self.directory / "instruction.bin"
        self.cli("asm", self.db, "add x1, x2, x3", "-o", machine_code)
        raw = bytes.fromhex("b3003100")
        self.assertEqual(machine_code.read_bytes(), raw)
        self.assertEqual(self.cli("asm", self.db, "add x1, x2, x3", "-o", "-").stdout, raw)
        assembly = self.cli("dis", self.db, "b3 00 31 00").stdout
        self.assertTrue(assembly.startswith(b"add "))
        self.assertEqual(self.cli("asm", self.db, assembly.decode().strip()).stdout, b"b3003100\n")
        self.assertEqual(self.cli("dis", self.db, "--file", machine_code).stdout, assembly)
        self.assertEqual(self.cli("dis", self.db, "--file", "-", stdin=raw).stdout, assembly)

        result = self.cli("run", self.db, "add x1, x2, x3", "--reg", "sp=0xffffffff", "--reg", "x3=2")
        final_registers = json.loads(result.stdout)
        self.assertEqual(final_registers["x1"], 1)
        self.assertEqual(final_registers["x2"], 0xFFFFFFFF)
        self.assertEqual(final_registers["x0"], 0)

    def test_features_replace_defaults(self):
        defaults = json.loads(self.cli("compile", RV32).stdout)
        self.assertIn("I", defaults["features"])
        enabled = json.loads(self.cli("compile", RV32, "--features=I").stdout)
        self.assertEqual(enabled["features"], ["I"])
        disabled = json.loads(self.cli("compile", RV32, "--features=").stdout)
        self.assertEqual(disabled["features"], [])
        self.assertNotIn("ADD", [inst["name"] for inst in disabled["instructions"]])

    def test_parameter_override(self):
        source = self.directory / "parameter.arch"
        source.write_text(
            """architecture Parameterized {
                endian = big;
                parameter WIDTH: nat in {8, 16} = 8;
                address_width = WIDTH;
                register_file Registers {
                    count = 2;
                    width = WIDTH;
                    register r[0..1];
                }
            }""",
            encoding="utf-8",
        )
        db = json.loads(self.cli("compile", source, "-D", "WIDTH=0x10").stdout)
        self.assertEqual(db["address_width"], 16)
        self.assertEqual(db["registers"]["width"], 16)
        self.assertEqual(db["parameters"], {"WIDTH": 16})

    def test_generate_c99_codec(self):
        self.compile_db()
        path = self.directory / "rv32_codec.c"
        self.cli("gen", self.db, "-o", path, "--prefix", "rv32")
        code = path.read_bytes()
        self.assertIn(b"rv32_encode(", code)
        self.assertIn(b"rv32_decode(", code)
        self.assertEqual(code, self.cli("gen", self.db, "--prefix", "rv32").stdout)
        self.assertIn(b"prefix", self.cli("gen", self.db, "--prefix", "9rv32", ok=False).stderr)

    def test_diagnostics(self):
        source = self.directory / "bad.arch"
        source.write_text("architecture Broken {\n", encoding="utf-8")
        self.assertIn(b"bad.arch:", self.cli("check", source, ok=False).stderr)

        for contents, expected in (
            ("{", b"malformed ArchDB JSON"),
            ('{"version":2}', b"unsupported ArchDB schema version 2"),
            ('{"version":true}', b"version must be a JSON integer"),
            ('{"version":1}', b"missing architecture"),
            ('{"version":1,"version":2}', b"duplicate JSON key"),
        ):
            with self.subTest(contents=contents):
                self.db.write_text(contents, encoding="utf-8")
                result = self.cli("inspect", self.db, ok=False)
                self.assertIn(expected, result.stderr)
                self.assertIn(b"riscv.archdb:", result.stderr)

        db = self.compile_db()
        db["instructions"][0]["mask"] = -1
        with self.assertRaisesRegex(ArchonError, r"instructions\[0\]\.mask"):
            database.loads(json.dumps(db))

        self.assertIn(b"invalid hex", self.cli("dis", self.db, "xyz", ok=False).stderr)
        self.assertIn(
            b"unknown register",
            self.cli("run", self.db, "add x1, x2, x3", "--reg", "x32=0", ok=False).stderr,
        )
        self.assertIn(b"invalid integer", self.cli("check", RV32, "-D", "XLEN=abc", ok=False).stderr)

    def test_malformed_semir_reports_field(self):
        db = self.compile_db()
        add = next(inst for inst in db["instructions"] if inst["name"] == "ADD")
        del add["semantics"]["statements"][0]["value"]["type"]
        self.db.write_text(json.dumps(db), encoding="utf-8")
        result = self.cli("run", self.db, "add x1, x2, x3", ok=False)
        self.assertIn(b"semantics.statements[0].value.type", result.stderr)


if __name__ == "__main__":
    unittest.main()
