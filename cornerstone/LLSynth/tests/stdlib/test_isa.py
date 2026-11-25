"""Inspect and decode real Sandstone ArchML profiles through generated tools."""

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
LIBDIR = os.getenv("LLSYNTH_ISA_LIBDIR") or os.getenv("LLSYNTH_NATIVE_LIBDIR")
SOURCE = r'''.language "ISA"
.import std.isa as ISA
.skip /[\r\n]+/
.token LINE: String = /[^\r\n]+/
.rule Start
'syntax: query:LINE profile:LINE args:LINE* EOF
'semantics:
if $query == "name" { return ISA:profileName($profile); }
if $query == "metadata" { return ISA:metadata($profile); }
if $query == "record" { return ISA:record($profile, $args[0], $args[1]); }
let hex = "";
if len($args) > 0 { hex = $args[0]; }
return ISA:analyze($profile, hex);
.end
'''


class ISAImports(unittest.TestCase):
    def test_module_without_native_library_at_generation(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            spec = work / "isa.llsynth"
            spec.write_text(SOURCE, encoding="utf-8")
            subprocess.run(
                ["perl", DRIVER, "--mode=interpret", "--manifest", "deps.json",
                 "-o", "tool.py", spec], cwd=work, check=True, capture_output=True,
                text=True, timeout=30,
            )
            dependencies = json.loads((work / "deps.json").read_text())["dependencies"]
            self.assertIn(str(LLS / "stdlib" / "isa.llsynth"),
                          [entry["path"] for entry in dependencies])


@unittest.skipUnless(LIBDIR, "set LLSYNTH_ISA_LIBDIR to the built ArchML companion")
class ISAAnalysis(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.work = Path(temporary.name)
        cls.libdir = Path(LIBDIR).resolve()
        cls.env = os.environ.copy()
        for key in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
            existing = cls.env.get(key)
            cls.env[key] = str(cls.libdir) + (os.pathsep + existing if existing else "")
        (cls.work / "main.llsynth").write_text(SOURCE, encoding="utf-8")
        subprocess.run(["perl", DRIVER, "--mode=interpret", "-T", "python3",
                        "-o", "tool.py", "main.llsynth"],
                       cwd=cls.work, check=True, timeout=30)
        cls.tools = [(sys.executable, "tool.py")]
        cc = shlex.split(os.getenv("CC", "cc"))
        if cc and shutil.which(cc[0]):
            subprocess.run(["perl", DRIVER, "--mode=interpret", "-T", "c11",
                            "-o", "tool.c", "main.llsynth"],
                           cwd=cls.work, check=True, timeout=30)
            subprocess.run([*cc, "-std=c11", "tool.c", "-L", cls.libdir,
                            "-lllsynth_isa", "-lm", "-o", "tool"],
                           cwd=cls.work, check=True, timeout=30)
            cls.tools.append((str(cls.work / "tool"),))

    def query(self, *args, error=None):
        results = []
        for tool in self.tools:
            result = subprocess.run(
                tool, input="\n".join(map(str, args)), cwd=self.work, env=self.env,
                capture_output=True, text=True, timeout=30,
            )
            if error is None:
                self.assertEqual(result.returncode, 0, (tool, result.stderr))
            else:
                self.assertNotEqual(result.returncode, 0, (tool, result.stdout))
                self.assertEqual(result.stdout, "")
                self.assertIn("ArchML:", result.stderr)
                self.assertIn(error, result.stderr)
            results.append(result.stdout.strip())
        self.assertTrue(all(reply == results[0] for reply in results))
        return results[0]

    def test_profile_metadata_and_records(self):
        path = PROFILES / "riscv64.aml"
        self.assertEqual(self.query("name", path), "riscv64")
        metadata = json.loads(self.query("metadata", path))
        sym = lambda text: {"symbol": text}
        self.assertEqual(metadata[:3], [sym("archml"), sym("1"), sym("riscv64")])
        target = json.loads(self.query("record", path, "target", "riscv64"))
        self.assertEqual(target, metadata[3])
        self.assertIn([sym("endian"), sym("little")], target)
        instruction = json.loads(self.query("record", path, "instruction", "ld"))
        self.assertEqual(instruction[:2], [sym("instruction"), sym("ld")])
        self.assertIn([sym("effects"), sym("memory")], instruction)
        self.assertTrue(any(item[0] == sym("pcode") for item in instruction[2:]))
        self.assertIsNone(json.loads(self.query("record", path, "instruction", "not-a-form")))

    def test_real_riscv_instructions(self):
        report = json.loads(self.query("analyze", PROFILES / "riscv64.aml",
                                       "B3003100 \t83 30 01 01"))
        self.assertEqual((report["profile"], report["byteLength"]), ("riscv64", 8))
        add, load = report["instructions"]
        self.assertEqual((add["offset"], add["size"], add["name"]), (0, 4, "add"))
        self.assertEqual((load["offset"], load["size"], load["name"]), (4, 4, "ld"))
        self.assertEqual([v["value"] for v in add["operands"]], ["1", "2", "3"])
        self.assertEqual(load["effects"], [{"symbol": "memory"}])
        self.assertEqual(load["implicitUses"], [])
        self.assertEqual(load["features"], ["base"])
        self.assertEqual(load["assembly"], "ld {rd}, {imm}({rs1})")
        self.assertEqual(load["operands"], [
            {"name": "rd", "kind": "register", "access": "write", "value": "1"},
            {"name": "rs1", "kind": "register", "access": "read", "value": "2"},
            {"name": "imm", "kind": "immediate", "access": "read", "value": "16"},
        ])

    def test_variable_lengths_and_64_bit_values(self):
        report = json.loads(self.query("analyze", PROFILES / "amd64.aml",
                                       "90 eb fe 48 b8 ff ff ff ff ff ff ff ff c3"))
        self.assertEqual(report["byteLength"], 14)
        instructions = report["instructions"]
        self.assertEqual([(i["offset"], i["size"], i["name"]) for i in instructions],
                         [(0, 1, "nop"), (1, 2, "jmp-rel8"), (3, 10, "mov64-ri"), (13, 1, "ret")])
        self.assertEqual(instructions[1]["operands"][0]["value"], "-2")
        self.assertEqual(instructions[1]["modes"], ["long64"])
        self.assertEqual(instructions[2]["operands"][1]["value"], "18446744073709551615")

    def test_diagnostics_and_text_only_operations(self):
        path = PROFILES / "riscv64.aml"
        self.assertEqual(json.loads(self.query("analyze", path)),
                         {"profile": "riscv64", "byteLength": 0, "instructions": []})
        self.query("analyze", path, "b3 00 31 00 ff", error="code byte offset 4")
        self.query("analyze", path, "b3 0", error="incomplete hex byte")
        self.query("analyze", path, "b 3", error="hex digit at input byte 1")
        self.query("analyze", path, "0x90", error="hex digit at input byte 1")
        oversize = "00 " * (64 * 1024 + 1)
        self.query("analyze", path, oversize, error="machine code exceeds 64 KiB")
        self.query("name", "absent.aml", error="cannot open absent.aml")
        (self.work / "invalid.aml").write_text("(archml 2 invalid)", encoding="utf-8")
        self.query("metadata", "invalid.aml", error="invalid.aml:1:")
        ptx = PROFILES / "nvptx.am"
        self.assertEqual(json.loads(self.query("record", ptx, "operation", "add.s16"))[1],
                         {"symbol": "add.s16"})
        self.query("analyze", ptx, "90", error="code byte offset 0")

    def test_custom_profile_with_missing_and_empty_properties(self):
        (self.work / "local.am").write_text(
            '''(archml 1 local
  (target local (bits 16) (endian big))
  (pcode-op nop (arity 0 0))
  (instruction first (operands) (encoding (base "12"))
    (pcode (nop)) (assembly "café\\x00"))
  (instruction second (operands) (encoding (base "12"))
    (pcode (nop)) (assembly "second") (effects pure))
  (instruction third (operands) (encoding (base "34"))
    (pcode (nop)) (assembly "third") (effects)))
''', encoding="utf-8",
        )
        report = json.loads(self.query("analyze", "local.am", "12 34"))
        first, third = report["instructions"]
        self.assertEqual(first["name"], "first")
        self.assertEqual(first["assembly"], "café\0")
        self.assertIsNone(first["effects"])
        self.assertEqual(third["effects"], [])


if __name__ == "__main__":
    unittest.main()
