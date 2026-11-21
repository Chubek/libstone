#!/usr/bin/env python3
"""Native imports; optional real Sandstone integration via LLSYNTH_SANDSTONE_BUILD.

All artifacts stay under the checkout (including temporary test directories).
See ../../docs/interop.md for the SDK test build commands.
"""
import importlib.util
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[3]
DRIVER = HERE.parents[1] / "scripts/llsynth.pl"
CC = shlex.split(os.environ.get("CC", "cc"))
SAND_BUILD = os.environ.get("LLSYNTH_SANDSTONE_BUILD")


def run(args, cwd, data=None, success=True):
    p = subprocess.run(list(map(str, args)), cwd=cwd, input=data,
                       capture_output=True, timeout=120)
    if success and p.returncode:
        raise AssertionError(f"{shlex.join(list(map(str, args)))}\n"
                             f"{p.stdout.decode(errors='replace')}{p.stderr.decode(errors='replace')}")
    return p


def specification(declarations, action, syntax="EOF"):
    return ('.language "Interop"\n' + declarations + '\n'
            '.exception !Bad "bad native result"\n'
            '.rule Start\n\'syntax: ' + syntax + "\n'semantics: " + action + "\n.end\n")


class InteropTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix=".interop-test-", dir=ROOT)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.lib = self.root / "native library.so"
        run([*CC, "-std=c11", "-fPIC", "-shared", HERE / "native.c", "-o", self.lib], self.root)
        self.declarations = "\n".join(
            f'.extern c {name}({args}) -> {result} from "{self.lib}" symbol "{symbol}"'
            for name, args, result, symbol in [
                ("add", "a: Int, b: Int", "Int", "interop_add"),
                ("scale", "a: Float64, b: Float64", "Float64", "interop_scale"),
                ("invert", "value: Bool", "Bool", "interop_not"),
                ("echo", "value: String", "String", "interop_echo"),
                ("null", "", "String", "interop_null"),
                ("invalid", "", "String", "interop_invalid"),
                ("record", "value: Int", "Void", "interop_record"),
                ("read", "", "Int", "interop_read")])

    def generate(self, text, target, split="none", extra=()):
        spec = self.root / "input.llsynth"
        spec.write_text(text)
        extension = "py" if target == "python3" else "c"
        output = self.root / f"tool.{extension}"
        run(["perl", DRIVER, "--mode=interpret", "-T", target, "--split", split,
             "--out-dir", self.root, "-o", output, *extra, spec], self.root)
        return output

    def tools(self, text, split="none", sdk=False):
        py = self.generate(text, "python3")
        yield [sys.executable, py]
        self.generate(text, "c11", split=split)
        cfiles = sorted(self.root.glob("*.c"))
        args = [*CC, "-std=c11", "-pedantic-errors", *cfiles, HERE / "native.c", "-lm"]
        if sdk:
            run([*args, "-I", ROOT / "sandstone/assembler", "-c"], self.root)
            # Link the C11 object code against the actual C++ SDK, not C++-
            # compiled generated sources.
            objects = sorted(self.root.glob("*.o"))
            build = Path(SAND_BUILD)
            args = [os.environ.get("CXX", "c++"), *objects,
                    build / "libsandstone_assembler.a", build / "libsandstone.a",
                    build / "liblua.a", "-lz", "-ldl", "-lm"]
        run([*args, "-o", self.root / "tool"], self.root)
        yield [self.root / "tool"]

    def test_native_scalars_and_function_values(self):
        cases = [
            ("return add(9007199254740993, 2);", b"9007199254740995\n"),
            ("return scale(3, 2.5);", b"7.5\n"),
            ("return invert(true);", b"false\n"),
            ('return echo("h\\u00e9llo");', "héllo\n".encode()),
            ("call record(42); return read();", b"42\n"),
            ("let fn: Fn(Int, Int)->Int = &add; return fn(19, 23);", b"42\n"),
        ]
        for action, expected in cases:
            with self.subTest(action=action):
                text = specification(self.declarations, action)
                for tool in self.tools(text):
                    self.assertEqual(run(tool, self.root, b"").stdout, expected)

    def test_split_native(self):
        text = specification(self.declarations, "return add(19, 23);")
        for tool in self.tools(text, split="all"):
            self.assertEqual(run(tool, self.root, b"").stdout, b"42\n")

    def test_alias_signatures(self):
        declarations = (self.declarations + '\n.type %Count = Int64\n'
                        f'.extern c alias(a: %Count, b: Int64) -> %Count from "{self.lib}" symbol "interop_add"')
        for tool in self.tools(specification(declarations, "return alias(19, 23);")):
            self.assertEqual(run(tool, self.root, b"").stdout, b"42\n")

    def test_native_runtime_errors(self):
        for action in ("return null();", "return invalid();"):
            for tool in self.tools(specification(self.declarations, action)):
                p = run(tool, self.root, b"", success=False)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn(b"INTEROP", p.stderr)
        text = '.token N: Int = /[0-9]+/\n' + specification(
            self.declarations, "return add($n, 2);", "n:N? EOF")
        for tool in self.tools(text):
            p = run(tool, self.root, b"", success=False)
            self.assertNotEqual(p.returncode, 0)
            self.assertIn(b"INTEROP", p.stderr)

    def test_generation_errors(self):
        cases = [
            (self.declarations, "return add(1);", "expects 2 arguments"),
            (self.declarations, 'return add("bad", 1);', "type mismatch"),
            ('.extern c bad(x: Vec<Int>) -> Int from "x"', "return 0;", "unsupported native parameter"),
            ('.extern c bad() -> Any from "x"', "return 0;", "unsupported native result"),
            (self.declarations + '\n.function add() -> Int\nreturn 1;\n.end', "return 0;", "duplicate functions"),
            ('.extern c abs() -> Int from "x"', "return 0;", "reserved interop"),
            ('.extern c a() -> Int from "x" symbol "same"\n.extern c b() -> Float64 from "x" symbol "same"',
             "return 0;", "conflicting native"),
            ('.extern c a() -> Int from "x" symbol "same"\n.extern c b() -> Int from "y" symbol "same"',
             "return 0;", "conflicting native"),
            ('.extern c bad(x: Void) -> Int from "x"', "return 0;", "unsupported native parameter"),
            ('.extern c bad() -> Vec<Int> from "x"', "return 0;", "unsupported native result"),
        ]
        for declarations, action, diagnostic in cases:
            with self.subTest(diagnostic=diagnostic):
                spec = self.root / "bad.llsynth"
                spec.write_text(specification(declarations, action))
                p = run(["perl", DRIVER, "--check", "--mode=interpret", spec], self.root, success=False)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn(diagnostic.encode(), p.stderr)
        spec.write_text(specification(self.declarations, "return 0;"))
        p = run(["perl", DRIVER, "--check", "--mode=interpret", "-T", "cpp17", spec],
                self.root, success=False)
        self.assertIn(b"not C++", p.stderr)

    def test_python_lazy_load_and_boundary_checks(self):
        text = specification('.extern c missing(x: Int) -> Int from "/no/such/library.so"', "return 42;")
        path = self.generate(text, "python3", extra=("--driver-mode=library",))
        spec = importlib.util.spec_from_file_location("interop_generated", path)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        self.addCleanup(sys.modules.pop, spec.name, None)
        spec.loader.exec_module(module)
        self.assertEqual(module.Context().interpret(""), 42)
        declaration = module.SPEC["functions"][0]
        for args in ([], [True], [2**63], [1.0], ["bad"], [1]):
            with self.subTest(args=args), self.assertRaises(module.LanguageError):
                module.interop_call(declaration, args)
        declaration["library"] = str(self.lib)
        with self.assertRaises(module.LanguageError):
            module.interop_call(declaration, [1])  # absent native symbol

    def test_missing_sandstone_cli(self):
        text = specification('.sandstone asm bank "missing.symas" cli "/no/such/sandstone-as"',
                             'return len(asm("nop\\n"));')
        py = self.generate(text, "python3")
        p = run([sys.executable, py], self.root, b"", success=False)
        self.assertNotEqual(p.returncode, 0)
        self.assertIn(b"SANDSTONE", p.stderr)

    @unittest.skipUnless(SAND_BUILD, "set LLSYNTH_SANDSTONE_BUILD for real SDK/CLI tests")
    def test_real_sandstone(self):
        build = Path(SAND_BUILD).resolve()
        bank = self.root / "test bank.symas"
        run([build / "mkbank", bank], self.root)
        declarations = f'.sandstone asm bank "{bank}" syntax "intel" cli "{build / "sandstone-as"}"'
        text = specification(declarations,
                             'let bytes = asm("nop\\nret\\n.byte 42\\n"); '
                             'require len(bytes) == 3 && bytes[0] == 144 && '
                             'bytes[1] == 195 && bytes[2] == 42 else Throw(!Bad); '
                             'return len(bytes);')
        for tool in self.tools(text, sdk=True):
            self.assertEqual(run(tool, self.root, b"").stdout, b"3\n")
        for tool in self.tools(specification(declarations, 'return len(asm(""));'), sdk=True):
            self.assertEqual(run(tool, self.root, b"").stdout, b"0\n")
        for source in ("unknown_instruction", ".quad external_symbol", ".error broken"):
            text = specification(declarations, f'return len(asm("{source}\\n"));')
            for tool in self.tools(text, sdk=True):
                p = run(tool, self.root, b"", success=False)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn(b"SANDSTONE", p.stderr)

    @unittest.skipUnless(SAND_BUILD, "set LLSYNTH_SANDSTONE_BUILD for real SDK/CLI tests")
    def test_split_sandstone(self):
        build = Path(SAND_BUILD).resolve()
        bank = self.root / "test.symas"
        run([build / "mkbank", bank], self.root)
        declarations = f'.sandstone asm bank "{bank}" cli "{build / "sandstone-as"}"'
        for tool in self.tools(specification(declarations, 'return asm("ret\\n")[0];'),
                               split="all", sdk=True):
            self.assertEqual(run(tool, self.root, b"").stdout, b"195\n")

    @unittest.skipUnless(SAND_BUILD, "set LLSYNTH_SANDSTONE_BUILD for real SDK/CLI tests")
    def test_sandstone_initialization_errors(self):
        build = Path(SAND_BUILD).resolve()
        bank = self.root / "test.symas"
        run([build / "mkbank", bank], self.root)
        declarations = [
            f'.sandstone asm bank "{bank}.missing" cli "{build / "sandstone-as"}"',
            f'.sandstone asm bank "{bank}" syntax "missing" cli "{build / "sandstone-as"}"',
        ]
        for declaration in declarations:
            for tool in self.tools(specification(declaration, 'return len(asm("nop\\n"));'), sdk=True):
                p = run(tool, self.root, b"", success=False)
                self.assertNotEqual(p.returncode, 0)
                self.assertIn(b"SANDSTONE", p.stderr)


if __name__ == "__main__":
    unittest.main()
