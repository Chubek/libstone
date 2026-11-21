"""Driver regression tests, runnable with the Python standard library."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class DriverTests(unittest.TestCase):
    driver = Path(__file__).resolve().parents[1] / "scripts" / "llsynth.pl"

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="llsynth-cli-")
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.spec = self.work / "tiny.llsynth"
        self.spec.write_text('.language "Tiny"\n.include "syntax.llsynth"\n', encoding="utf-8")
        includes = self.work / "includes"
        includes.mkdir()
        self.syntax = includes / "syntax.llsynth"
        self.syntax.write_text(
            r""".skip /[ \t\n]+/
.token NUMBER: Float64 = /[0-9]+/
.ir Stack
.op PUSH_F64(value: Float64): () -> (Float64)
.end
.rule Start
'syntax: value:NUMBER EOF
'semantics: return $value;
'lower: return [Stack.PUSH_F64($value)];
.end
""",
            encoding="utf-8",
        )
        self.include_args = ("--include-path", self.work / "missing", "--include-path", includes)

    def run_command(self, *args, code=0, stdin=None):
        result = subprocess.run(
            args, cwd=self.work, input=stdin, text=True, capture_output=True, timeout=30
        )
        self.assertEqual(
            result.returncode, code, (args, result.stdout, result.stderr)
        )
        return result

    def generate(self, *args, code=0):
        return self.run_command(self.driver, *self.include_args, *args, self.spec, code=code)

    def test_help_and_version_from_another_directory(self):
        # The executable resolves its modules beside the real driver, not the
        # current directory or the directory containing a symlink.
        launcher = self.work / "llsynth"
        launcher.symlink_to(self.driver)
        version = self.run_command(launcher, "--version")
        self.assertRegex(version.stdout, r"^llsynth\.pl \d+\.\d+\.\d+\n$")
        self.assertEqual(version.stderr, "")
        help_result = self.run_command(launcher, "--help")
        self.assertIn("Usage: llsynth.pl", help_result.stdout)
        self.assertEqual(help_result.stderr, "")
        self.run_command(launcher, code=2)

    def test_check_and_dump_with_include_paths(self):
        result = self.generate("--check", "--parser", "lr1", "--dump-grammar", "--dump-tables=tables.json")
        self.assertIn("Start -> NUMBER EOF", result.stdout)
        self.assertIn("OK: Tiny", result.stderr)
        tables = json.loads((self.work / "tables.json").read_text(encoding="utf-8"))
        self.assertTrue(tables["actions"])
        self.assertFalse((self.work / "tiny.py").exists())

    def test_generator_diagnostics(self):
        result = self.run_command(self.driver, "--check", "--diagnostics=json", self.spec, code=1)
        self.assertEqual(result.stdout, "")
        diagnostic = json.loads(result.stderr)
        self.assertEqual(diagnostic["code"], "LLS001")
        self.assertIn("include 'syntax.llsynth' not found", diagnostic["message"])
        self.assertEqual(diagnostic["location"], {"file": str(self.spec), "line": 2, "column": 1})

        result = self.generate("--check", "--diagnostics=sarif", "--parser=ll1", code=1)
        sarif = json.loads(result.stderr)
        self.assertEqual(sarif["version"], "2.1.0")
        self.assertIn("unsupported parser 'll1'", sarif["runs"][0]["results"][0]["message"]["text"])

    def test_compiler_reads_cli_options(self):
        with self.syntax.open("a", encoding="utf-8") as source:
            source.write(".rule Unused\n'syntax: NUMBER\n.end\n")
        result = self.generate("--check")
        self.assertIn("warning W001: unreachable rule Unused", result.stderr)
        result = self.generate("--check", "--warnings-as-errors", code=1)
        self.assertIn("error: unreachable rule Unused", result.stderr)
        result = self.generate("--check", "--mode=interpret", "--program-target=custom")
        self.assertIn("OK: Tiny", result.stderr)
        result = self.generate("--check", "--mode=both", "--program-target=custom", code=1)
        self.assertIn("no backend defined for program target 'custom'", result.stderr)

    def test_python_package_manifest_and_depfile(self):
        self.generate("--package", "tiny_tool", "--manifest", "manifest.json", "--depfile", "tiny.d")
        run = self.run_command(sys.executable, "-m", "tiny_tool", stdin="42\n")
        self.assertEqual(run.stdout, "42\n")
        manifest = json.loads((self.work / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["generator"], "llsynth.pl")
        self.assertEqual(manifest["language"], "Tiny")
        self.assertEqual(
            {Path(entry["path"]) for entry in manifest["dependencies"]},
            {self.spec, self.syntax},
        )
        outputs = {Path(entry["path"]): entry["sha256"] for entry in manifest["outputs"]}
        self.assertEqual(
            set(outputs), {Path("tiny_tool/__init__.py"), Path("tiny_tool/__main__.py")}
        )
        for path, digest in outputs.items():
            self.assertEqual(hashlib.sha256((self.work / path).read_bytes()).hexdigest(), digest)
        depfile = (self.work / "tiny.d").read_text(encoding="utf-8")
        self.assertTrue(depfile.startswith("./tiny_tool/__init__.py: "))
        self.assertIn(str(self.spec), depfile)
        self.assertIn(str(self.syntax), depfile)

    def test_output_preflight_preserves_existing_files(self):
        output = self.work / "tiny.py"
        output.write_text("existing output\n", encoding="utf-8")
        result = self.generate("--output", output, "--manifest", output, code=1)
        self.assertIn("output path collision", result.stderr)
        self.assertEqual(output.read_text(encoding="utf-8"), "existing output\n")

        before = self.spec.read_bytes()
        result = self.generate("-T", "c99", "--output", self.spec, code=1)
        self.assertIn("output would overwrite specification", result.stderr)
        self.assertEqual(self.spec.read_bytes(), before)

    def test_generator_modules_are_not_writable_outputs(self):
        local_scripts = self.work / "scripts"
        shutil.copytree(self.driver.parent, local_scripts)
        driver = local_scripts / self.driver.name
        module = local_scripts / "LLSynthCompiler.pm"
        baseline = module.read_bytes()
        output = self.work / "tiny.py"
        result = self.run_command(
            driver, *self.include_args, "--output", output, "--manifest", module, self.spec, code=1
        )
        self.assertIn("output would overwrite an input", result.stderr)
        self.assertEqual(module.read_bytes(), baseline)
        self.assertFalse(output.exists())

    def test_split_c_driver(self):
        self.generate("-T", "c99", "--split", "facets")
        self.assertIn("Tiny_interpret", (self.work / "Tiny.h").read_text(encoding="utf-8"))
        self.assertTrue((self.work / "Tiny_internal.h").is_file())
        sources = sorted(self.work.glob("*.c"))
        self.assertIn(self.work / "Tiny_driver.c", sources)
        self.assertIn(self.work / "Tiny_runtime.c", sources)
        self.assertTrue((self.work / "TinyTables.i").is_file())
        compiler = shutil.which("cc")
        if compiler:
            exe = self.work / "tiny"
            self.run_command(compiler, "-std=c99", *sources, "-lm", "-o", exe)
            self.assertEqual(self.run_command(exe, stdin="12\n").stdout, "12\n")


if __name__ == "__main__":
    unittest.main()
