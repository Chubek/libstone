"""Exercise specification and native lookup through LLSYNTH_LIBRARY_PATH."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


LLSYNTH = Path(__file__).resolve().parents[2]
ROOT = LLSYNTH.parents[1]
DRIVER = LLSYNTH / "scripts/llsynth.pl"
CC = shlex.split(os.environ.get("CC", "cc"))
LIBRARY = "libllsynth_library_path_test.so.1"


class LibraryPathTests(unittest.TestCase):
    def setUp(self):
        scratch = tempfile.TemporaryDirectory(prefix=".library-path-test-", dir=ROOT)
        self.addCleanup(scratch.cleanup)
        self.work = Path(scratch.name)
        self.project = self.work / "project"
        self.project.mkdir()
        self.env = dict(os.environ, TMPDIR=str(self.work))
        self.env.pop("LLSYNTH_LIBRARY_PATH", None)

    def run_cmd(self, *args, search=None, env=None, success=True):
        env = dict(self.env if env is None else env)
        if search is not None:
            env["LLSYNTH_LIBRARY_PATH"] = search
        command = list(map(str, args))
        result = subprocess.run(command, cwd=self.work, env=env, input="",
                                text=True, capture_output=True, timeout=45)
        if success:
            self.assertEqual(result.returncode, 0, f"{shlex.join(command)}\n{result.stderr}")
        else:
            self.assertNotEqual(result.returncode, 0, f"{shlex.join(command)}\n{result.stdout}")
        return result

    def write_module(self, path, name, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f'.library "{name}"\n.function choice() -> Int\n'
                        f'return {value};\n.end\n', encoding="utf-8")

    def generate(self, declaration, action, search=None, extra=(), target="python3",
                 success=True):
        spec = self.project / "tool.llsynth"
        spec.write_text(f'.language "LibraryPath"\n{declaration}\n.rule Start\n'
                        f"'syntax: EOF\n'semantics: {action}\n.end\n", encoding="utf-8")
        output = self.work / ("tool.py" if target == "python3" else "tool.c")
        result = self.run_cmd("perl", DRIVER, "--mode=interpret", "-T", target,
                              "-o", output, *extra, spec, search=search, success=success)
        return output if success else result

    def test_named_import_order_and_fallbacks(self):
        relative = Path("Pkg/Choice.llsynth")
        self.write_module(self.project / relative, "Pkg.Choice", 30)
        self.write_module(self.work / "first" / relative, "Pkg.Choice", 10)
        self.write_module(self.work / "second" / relative, "Pkg.Choice", 20)
        self.write_module(self.work / "flag" / relative, "Pkg.Choice", 40)
        self.write_module(self.work / relative, "Pkg.Choice", 99)
        flag = ("--module-path", self.work / "flag")

        for spelling, search, want in (
            ("Pkg.Choice", f":first::{self.work / 'second'}:", "10"),
            ('"Pkg.Choice"', "missing:second:first", "20"),
            ("Pkg.Choice", "::", "30"),
        ):
            with self.subTest(spelling=spelling, search=search):
                tool = self.generate(f".import {spelling} as P", "return P:choice();",
                                     search=search, extra=flag)
                self.assertEqual(self.run_cmd(sys.executable, tool).stdout.strip(), want)

        (self.project / relative).unlink()
        tool = self.generate(".import Pkg.Choice as P", "return P:choice();",
                             search=":missing:", extra=flag)
        self.assertEqual(self.run_cmd(sys.executable, tool).stdout.strip(), "40")

    def test_named_std_override_and_bundled_fallback(self):
        graph = self.work / "overrides/std/Graph.llsynth"
        graph.parent.mkdir(parents=True)
        graph.write_text(
            '.library "std.Graph"\n.function distances(edges: Vec<Vec<Int>>, source: Int) -> Vec<Int>\n'
            'return [0, 17];\n.end\n', encoding="utf-8")
        for search, want in (("overrides", "17"), ("missing", "1")):
            with self.subTest(search=search):
                tool = self.generate('.import "std.Graph" as G',
                                     "return G:distances([[1], []], 0)[1];", search=search)
                self.assertEqual(self.run_cmd(sys.executable, tool).stdout.strip(), want)

    def test_explicit_spec_paths_bypass_environment(self):
        local = self.project / "Pkg/Choice.llsynth"
        self.write_module(local, "Pkg.Choice", 13)
        self.write_module(self.work / "overrides/Pkg/Choice.llsynth", "Pkg.Choice", 99)
        self.write_module(self.project / "raw", "Raw", 13)
        self.write_module(self.work / "overrides/raw", "Raw", 99)
        self.write_module(self.work / "overrides/raw.llsynth", "raw", 99)
        for spelling in ("./Pkg/Choice.llsynth", "Pkg/Choice.llsynth", str(local), "raw"):
            with self.subTest(spelling=spelling):
                tool = self.generate(f'.import "{spelling}" as P', "return P:choice();",
                                     search="overrides")
                self.assertEqual(self.run_cmd(sys.executable, tool).stdout.strip(), "13")
        result = self.generate('.import "Other.llsynth"', "return 0;",
                               search="overrides", success=False)
        self.assertIn("module 'Other.llsynth' not found", result.stderr)
        self.assertNotIn("overrides", result.stderr)

    def test_selected_named_module_is_not_replaced_after_error(self):
        self.write_module(self.work / "bad/Pkg/Choice.llsynth", "Wrong.Name", 1)
        self.write_module(self.project / "Pkg/Choice.llsynth", "Pkg.Choice", 2)
        result = self.generate(".import Pkg.Choice", "return 0;", search="bad", success=False)
        self.assertIn("module name mismatch: expected 'Pkg.Choice', found 'Wrong.Name'",
                      result.stderr)

    @unittest.skipUnless(sys.platform.startswith("linux") and CC and shutil.which(CC[0]),
                         "native lookup regression requires a Linux C compiler")
    def test_native_lookup_and_c_linkage(self):
        source = self.work / "native.c"
        for dirname, value in (("first", 10), ("with space", 20), ("platform", 30), ("local", 40)):
            directory = self.work / dirname
            directory.mkdir()
            source.write_text(f"#include <stdint.h>\nint64_t library_path_probe(void) {{ return {value}; }}\n",
                              encoding="utf-8")
            self.run_cmd(*CC, "-std=c11", "-fPIC", "-shared", source, "-o", directory / LIBRARY)
        # Empty entries must not implicitly search the current directory.
        shutil.copyfile(self.work / "local" / LIBRARY, self.work / LIBRARY)
        for subpath in ("local", "missing"):
            alternate = self.work / "first" / subpath / LIBRARY
            alternate.parent.mkdir()
            shutil.copyfile(self.work / "first" / LIBRARY, alternate)
        (self.work / "bad").mkdir()
        (self.work / "bad" / LIBRARY).write_text("not a shared library", encoding="utf-8")
        env = dict(self.env, LD_LIBRARY_PATH=str(self.work / "platform"))

        def native(name, target="python3"):
            # This search path is used at runtime, not by generation.
            return self.generate(f'.extern c probe() -> Int from "{name}" symbol "library_path_probe"',
                                 "return probe();", search="missing-during-generation", target=target)

        tool = native(LIBRARY)
        for search, want in ((":first::with space:", "10"), ("missing:with space:first", "20"),
                             (":missing:", "30")):
            with self.subTest(search=search):
                result = self.run_cmd(sys.executable, tool, search=search, env=env)
                self.assertEqual(result.stdout.strip(), want)

        result = self.run_cmd(sys.executable, tool, search="bad:first", env=env, success=False)
        self.assertIn("INTEROP", result.stderr)
        self.assertIn(str(self.work / "bad" / LIBRARY), result.stderr)

        for explicit in (f"./local/{LIBRARY}", str(self.work / "local" / LIBRARY)):
            with self.subTest(explicit=explicit):
                tool = native(explicit)
                self.assertEqual(self.run_cmd(sys.executable, tool, search="bad:first", env=env)
                                 .stdout.strip(), "40")
        tool = native(f"./missing/{LIBRARY}")
        result = self.run_cmd(sys.executable, tool, search="first", env=env, success=False)
        self.assertIn(f"./missing/{LIBRARY}", result.stderr)

        generated_c = native(LIBRARY, "c11")
        binary = self.work / "tool"
        self.run_cmd(*CC, "-std=c11", generated_c, source, "-lm", "-o", binary)
        self.assertEqual(self.run_cmd(binary, search="bad:first", env=env).stdout.strip(), "40")


if __name__ == "__main__":
    unittest.main()
