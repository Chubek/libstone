"""Select real ArchML forms, compile their IR, and optionally assemble the result."""

import hashlib
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


LLS = Path(__file__).resolve().parents[2]
DRIVER = LLS / "scripts" / "llsynth.pl"
PROFILES = LLS.parents[1] / "sandstone" / "archml" / "stdspec"
EXAMPLE = (LLS / "examples" / "isel" / "select.llsynth").read_text()
LOADS = {
    # The source IR stores (destination, offset, base). All three concrete
    # encoders take (destination, base, offset), including scaled ARM offsets.
    "rv":  ("riscv64", ".insn ld, 1, 2, 16\n", "83 30 01 01"),
    "a64": ("arm64", ".insn ldr64, 1, 2, 16\n", "41 08 40 f9"),
    "x64": ("amd64", ".insn load64-base-disp32, 1, 2, 16\n",
            "48 8b 8c 22 10 00 00 00"),
}
SAND_AS = os.getenv("LLSYNTH_ARCHML_AS") or shutil.which("sandstone-as")


class ISelTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def cmd(self, *args, data="", ok=True):
        result = subprocess.run(args, input=data, text=True, cwd=self.work,
                                capture_output=True, timeout=40)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def generate(self, source=EXAMPLE, *args, ok=True):
        (self.work / "main.llsynth").write_text(source)
        return self.cmd("perl", DRIVER, "--mode=compile", "--program-target=rv",
                        *args, "main.llsynth", ok=ok)

    def check_loads(self, *tool):
        for target, (_, expected, _) in LOADS.items():
            with self.subTest(target=target):
                result = self.cmd(*tool, "--compile", "--target", target, data="1 16 2")
                self.assertEqual(result.stdout, expected)

    def test_three_profiles_and_dependencies(self):
        self.generate(EXAMPLE, "-o", "tool.py", "--manifest", "manifest.json",
                      "--depfile", "tool.d")
        deps = {Path(d["path"]): d["sha256"] for d in
                json.loads((self.work / "manifest.json").read_text())["dependencies"]}
        depfile = (self.work / "tool.d").read_text()
        for path in [LLS / "stdlib" / "ISel.llsynth", LLS / "stdlib" / "ArchMLSelection.pm",
                     *(PROFILES / (name + ".aml") for name, _, _ in LOADS.values())]:
            self.assertEqual(deps[path], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertIn(str(path), depfile)
        self.check_loads(sys.executable, "tool.py")

    def test_native_tool(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C compiler unavailable")
        self.generate(EXAMPLE, "-T", "c11", "-o", "tool.c")
        self.cmd(*cc, "-std=c11", "tool.c", "-lm", "-o", "tool")
        self.check_loads(self.work / "tool")

    @unittest.skipUnless(SAND_AS, "set LLSYNTH_ARCHML_AS to an ArchML-enabled sandstone-as")
    def test_real_assembler(self):
        self.generate(EXAMPLE, "-o", "tool.py")
        for target, (profile, _, encoding) in LOADS.items():
            with self.subTest(profile=profile):
                asm = self.cmd(sys.executable, "tool.py", "--compile", "--target", target,
                               data="1 16 2").stdout
                (self.work / "out.s").write_text(asm)
                self.cmd(SAND_AS, "--archml", PROFILES / (profile + ".aml"),
                         "-o", "out.bin", "out.s")
                self.assertEqual((self.work / "out.bin").read_bytes(), bytes.fromhex(encoding))

    def test_block_scope(self):
        imp = '.import "std.ISel"'
        (self.work / "Sibling.llsynth").write_text(f'.module Sibling\n{imp}\n')
        for imports in ["", ".import Sibling"]:
            result = self.generate(EXAMPLE.replace(imp, imports), "--check", ok=False)
            self.assertIn("unknown directive or unexpected text: '.isel", result.stderr)
        self.generate(EXAMPLE.replace(imp, f"{imp} as S\n{imp}"), "--check")

    def test_imported_ir_and_feature_selection(self):
        (self.work / "Legal.llsynth").write_text('''.library Legal
.type %Reg = Int
.ir Machine
.op Divide(out: %Reg, lhs: %Reg, rhs: %Reg): () -> ()
.end
''')
        self.generate('''.language "Divide"
.import Legal as L
.import std.ISel
.isel rv for L.Machine from riscv64 features base,m
.select Divide(rd, rs1, rs2) => udiv i64
.end
.rule Start
'syntax: EOF
'lower: return [L.Machine.Divide(1, 2, 3)];
.end
''', "-o", "tool.py")
        self.assertEqual(self.cmd(sys.executable, "tool.py", "--compile").stdout,
                         ".insn divu, 1, 2, 3\n")

    def test_diagnostics_identify_the_source_line(self):
        # Move the final block into an included file beside its custom profile.
        folder = self.work / "fragment"
        folder.mkdir()
        fragment = folder / "block.llsynth"
        fragment.write_text('.isel x64 for Machine from "mine.aml"\n'
                            '.select Load(reg, disp, base) => load i32\n.end\n')
        (folder / "mine.aml").write_bytes((PROFILES / "amd64.aml").read_bytes())
        begin, rest = EXAMPLE.split(".isel x64", 1)
        source = begin + '.include "fragment/block.llsynth"\n' + rest.split(".end", 1)[1]
        err = json.loads(self.generate(source, "--check", "--diagnostics=json", ok=False).stderr)
        self.assertIn("no selection for 'load i32'", err["message"])
        self.assertEqual(Path(err["location"]["file"]).name, fragment.name)
        self.assertEqual(err["location"]["line"], 2)

    def test_generation_errors(self):
        cases = [
            ("from riscv64 features base", "from not_a_profile", "unknown ArchML stdspec"),
            ("from riscv64 features base", "from evm", "has no selection records"),
            ("from riscv64 features base", "from riscv64 features m", "requires 'base'"),
            ("from riscv64 features base", "from riscv64 features xyz", "unknown feature"),
            ("Load(rd, imm, rs1) => load i64", "Load(rd, rs1, rs2) => mul i64", "requires feature m"),
            ("Load(rd, imm, rs1) => load i64", "Load(rd, imm, rs1) => load i32", "no selection"),
            ("Load(rd, imm, rs1)", "Load(rd, offset, rs1)", "ld needs operands"),
            ("Load(rd, imm, rs1)", "Load(rd, rd, rs1)", "duplicate operand"),
            ("Load(rd, imm, rs1)", "Absent(rd, imm, rs1)", "undeclared opcode"),
            ("offset: Int", "offset: Float64", "requires Int, got Float64"),
            (".select Load(rd, imm, rs1) => load i64", "", "empty .isel"),
            (".isel a64", ".isel rv", "duplicate backend"),
            (".select Load(rd, imm, rs1) => load i64\n.end",
             ".select Load(rd, imm, rs1) => load i64\n.select Load(rd, imm, rs1) => load i64\n.end",
             "duplicate selection"),
        ]
        for old, new, expected in cases:
            with self.subTest(expected=expected):
                self.assertIn(expected, self.generate(EXAMPLE.replace(old, new, 1), "--check",
                                                      ok=False).stderr)

    def test_profile_edits_drive_selection_and_cost(self):
        # Introduce a second rule using the real SUB record. Same operands but
        # different instruction ID, so the output discriminates the winner.
        src = (PROFILES / "riscv64.aml").read_text()
        first = "(selection sub\n    (pattern (ir-op sub i64))"
        self.assertIn(first, src)
        src = src.replace(first, "(selection aaa\n    (pattern (ir-op add i64))")
        path = self.work / "local.aml"
        spec = EXAMPLE[:EXAMPLE.index(".isel a64")] + EXAMPLE[EXAMPLE.index(".rule Start"):]
        spec = spec.replace("from riscv64", 'from "local.aml"')
        spec = spec.replace("Load(rd, imm, rs1) => load i64", "Load(rd, rs1, rs2) => add i64")
        for cost, winner in [(2, "add"), (1, "sub")]:
            path.write_text(src.replace("(instruction (ref instruction sub))\n    (cost 1)",
                                        f"(instruction (ref instruction sub))\n    (cost {cost})"))
            self.generate(spec, "-o", "tool.py")
            path.unlink()
            out = self.cmd(sys.executable, "tool.py", "--compile", data="1 2 3")
            self.assertEqual(out.stdout, f".insn {winner}, 1, 2, 3\n")


if __name__ == "__main__":
    unittest.main()
