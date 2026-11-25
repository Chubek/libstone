#!/usr/bin/env python3
"""Type checking and opt-in tests against the native Sandstone SDK."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
LLSYNTH = HERE.parents[1]
DRIVER = LLSYNTH / "scripts/llsynth.pl"
COMPANION = LLSYNTH / "stdlib/assembler"
CC = shlex.split(os.environ.get("CC", "cc"))
BRIDGE = os.environ.get("LLSYNTH_ASSEMBLER_LIBRARY")
MKBANK = os.environ.get("LLSYNTH_ASSEMBLER_MKBANK")


def run(args, cwd, *, ok=True, env=None, data=b""):
    args = list(map(str, args))
    process = subprocess.run(args, cwd=cwd, env=env, input=data, capture_output=True, timeout=30)
    if ok and process.returncode:
        raise AssertionError(f"{shlex.join(args)}\n{process.stderr.decode(errors='replace')}")
    return process


def spec(action, imports=".import std.assembler as A", *, declarations="", syntax="EOF"):
    return (f'.language "Assembly"\n{imports}\n{declarations}\n.rule Start\n'
            f"'syntax: {syntax}\n'semantics:\n{action}\n.end\n")


class AssemblyTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="llsynth-assembler-")
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.source = self.root / "input.llsynth"

    def generate(self, text, target="python3", split="none"):
        self.source.write_text(text, encoding="utf-8")
        output = self.root / ("assembly.py" if target == "python3" else "assembly.c")
        run(["perl", DRIVER, "--mode=interpret", "-T", target, "--split", split,
             "--out-dir", self.root, "-o", output, self.source], self.root)
        return output

    def test_typed_exports(self):
        declarations = [
            ("return A:assemble(\"nop\", \"bank\");", b"expects 3 arguments"),
            ("return A:assembleTarget(\"nop\", 42, \"\");", b"type mismatch"),
        ]
        for action, diagnostic in declarations:
            for target in ("python3", "c11"):
                with self.subTest(action=action, target=target):
                    self.source.write_text(spec(action), encoding="utf-8")
                    process = run(["perl", DRIVER, "--mode=interpret", "--check", "-T", target,
                                   self.source], self.root, ok=False)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn(diagnostic, process.stderr)

    def test_python_lazy_and_opt_in(self):
        env = {**os.environ, "LLSYNTH_ASSEMBLER_LIBRARY": str(self.root / "missing-bridge.so")}
        plain = self.generate(spec("return 42;", imports=""))
        self.assertNotIn("def assembler_sdk_call", plain.read_text(encoding="utf-8"))
        native = self.generate(spec("return 42;", imports=""), "c11")
        self.assertNotIn("llsynth_assembler.h", native.read_text(encoding="utf-8"))
        unused = self.generate(spec("return 42;"))
        self.assertEqual(run([sys.executable, unused], self.root, env=env).stdout, b"42\n")
        called = self.generate(spec('return A:assemble("nop", "bank", "");'))
        process = run([sys.executable, called], self.root, env=env, ok=False)
        self.assertNotEqual(process.returncode, 0)
        self.assertIn(b"SANDSTONE", process.stderr)
        self.assertIn(b"missing-bridge", process.stderr)

    @unittest.skipUnless(BRIDGE and MKBANK, "configure tests/assembler with a real Sandstone SDK")
    def test_real_sdk(self):
        bank = self.root / "test bank.symas"
        run([Path(MKBANK).resolve(), bank], self.root)
        text = spec(r'''
let f: Fn(String, String, String)->Vec<Int> = &A->assemble;
let code: Vec<Int> = f("nop\nret\n.byte 0, 255\n", "test bank.symas", "intel");
let empty = A:assemble("", "test bank.symas", "");
require len(empty) == 0 else Throw(!Bad);
let rv = A:assembleTarget("addi x1, x0, 7\n", "rv64im", "default");
require len(rv) == 4 && rv[0] == 147 && rv[1] == 0 &&
    rv[2] == 112 && rv[3] == 0 else Throw(!Bad);
let bss = A:assembleTarget(".byte 42\n.bss\n.space 4", "rv64im", "");
require len(bss) == 12 && bss[0] == 42 && bss[11] == 0 else Throw(!Bad);
let result = A:assemble(".byte 42\nlabel:\n.quad label", "test bank.symas", "");
require len(result) == 9 && result[0] == 42 && result[1] == 1 &&
    result[8] == 0 else Throw(!Bad);
require len(code) == 4 && code[0] == 144 && code[1] == 195 &&
    code[2] == 0 && code[3] == 255 else Throw(!Bad);
return len(code);''', declarations='.exception !Bad "incorrect assembled bytes"')

        bridge = Path(BRIDGE).resolve()
        env = {**os.environ, "PATH": "", "LLSYNTH_ASSEMBLER_LIBRARY": str(bridge)}
        py = self.generate(text)
        self.assertEqual(run([sys.executable, py], self.root, env=env).stdout, b"4\n")
        path_env = {**env, "LLSYNTH_LIBRARY_PATH": str(bridge.parent)}
        del path_env["LLSYNTH_ASSEMBLER_LIBRARY"]
        self.assertEqual(run([sys.executable, py], self.root, env=path_env).stdout, b"4\n")

        self.generate(text, "c11", "all")
        executable = self.root / "assembly"
        run([*CC, "-std=c11", "-pedantic-errors", "-I", COMPANION,
             *sorted(self.root.glob("*.c")), bridge, f"-Wl,-rpath,{bridge.parent}",
             "-lm", "-o", executable], self.root)
        self.assertEqual(run([executable], self.root, env=env).stdout, b"4\n")

    @unittest.skipUnless(BRIDGE and MKBANK, "configure tests/assembler with a real Sandstone SDK")
    def test_sdk_diagnostics(self):
        run([Path(MKBANK).resolve(), self.root / "test.symas"], self.root)
        text = spec('return len(A:assemble($args[0], $args[1], $args[2]));',
                    declarations=".token ARG: String = /[^|]+/\n.skip /[|]+/",
                    syntax="args:ARG+ EOF")
        bridge = Path(BRIDGE).resolve()
        env = {**os.environ, "PATH": "", "LLSYNTH_ASSEMBLER_LIBRARY": str(bridge)}
        py = self.generate(text)
        source = self.generate(text, "c99")
        executable = self.root / "assembly"
        run([*CC, "-std=c99", "-pedantic-errors", "-I", COMPANION, source,
             bridge, f"-Wl,-rpath,{bridge.parent}", "-lm", "-o", executable], self.root)
        for command in ([sys.executable, py], [executable]):
            for data, diagnostic in [
                (b".quad external_symbol|test.symas|intel", b"unresolved relocation"),
                (b"nop|missing.symas|intel", b"cannot open bank"),
                (b"nop|test.symas|unknown", b"unknown syntax"),
                (b".error explicit assembly failure|test.symas|intel", b"explicit assembly failure"),
                (b".bss\n" + b".space 16777216\n" * 4 + b".space 1|test.symas|intel",
                 b"cannot construct flat image"),
            ]:
                with self.subTest(command=command, data=data):
                    process = run(command, self.root, env=env, data=data, ok=False)
                    self.assertNotEqual(process.returncode, 0)
                    self.assertIn(b"SANDSTONE", process.stderr)
                    self.assertIn(diagnostic, process.stderr)


if __name__ == "__main__":
    unittest.main()
