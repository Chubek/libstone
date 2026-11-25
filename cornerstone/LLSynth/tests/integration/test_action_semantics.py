"""LLSynth facet delegation against the actual NovoStone acsem generator."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"
ACSEM = os.environ.get("LLSYNTH_TEST_ACSEM") or shutil.which("acsem")


class ActionSemanticsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="llsynth-acsem-")
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        self.spec = self.work / "test.llsynth"

    def run_cmd(self, *args, source=None):
        return subprocess.run(args, input=source, cwd=self.work, text=True,
                              capture_output=True, timeout=40)

    def generator(self, *args, tool=None):
        tool = tool or ACSEM
        params = ["--acsem", tool] if tool else []
        return self.run_cmd(DRIVER, *params, *args, self.spec)

    @unittest.skipUnless(ACSEM, "build acsem or set LLSYNTH_TEST_ACSEM")
    def test_delegated_rule_mixes_with_ordinary_facets_and_rules(self):
        self.spec.write_text(
            """\
.language "Mixed"
.token N: Int = /-?[0-9]+/
.skip /[ \\t\\r\\n]+/
.start Start
.ir Code
.op PUSH_F64(value: Float64) : () -> (Float64)
.end
.rule Start
'syntax: "double" datum:Datum EOF
'semantics: return $datum * 2;
'lower: return [Code.PUSH_F64($datum)];
'analyze: print($datum);
.end
.rule Datum
'syntax: value:N
'semantics actionsemantics(value: Int) -> Int:
  (allocate cell then (store value in given(1) and then give contents(given(1))))
'lower: return $value + 2;
'analyze: return $value + 4;
.end
""", encoding="utf-8")
        result = self.generator("--mode", "both", "--program-target", "bytecode",
                                "-o", "tool.py", "--manifest", "build.json",
                                "--depfile", "build.d")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cmd(sys.executable, "tool.py", "--interpret", source="double 21")
        self.assertEqual((result.returncode, result.stdout), (0, "42\n"), result.stderr)
        result = self.run_cmd(sys.executable, "tool.py", "--analyze", source="double 21")
        self.assertEqual((result.returncode, result.stdout), (0, "25\nanalysis OK\n"), result.stderr)
        result = self.run_cmd(sys.executable, "tool.py", "--compile", "-o", "result.bc", source="double 21")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cmd(sys.executable, "tool.py", "--run-bytecode", "result.bc")
        self.assertEqual((result.returncode, result.stdout), (0, "23\n"), result.stderr)
        manifest = json.loads((self.work / "build.json").read_text())
        self.assertIn(str(Path(ACSEM).resolve()), [entry["path"] for entry in manifest["dependencies"]])
        self.assertIn(str(Path(ACSEM).resolve()), (self.work / "build.d").read_text())

    @unittest.skipUnless(ACSEM, "build acsem or set LLSYNTH_TEST_ACSEM")
    def test_native_and_python_runtime_parity(self):
        self.spec.write_text(
            """\
.language "Parity"
.token N: Int = /-?[0-9]+/
.token Name: String = /[A-Za-z_]+/
.skip /[ \\t\\r\\n]+/
.start Start
.rule Start
'syntax: datum:Datum EOF
'semantics actionsemantics(datum: Int) -> Int: give datum
.end
.rule Datum
'syntax: value:N name:Name
'semantics actionsemantics(value: Int, name: String) -> Int:
  (if value == -2 then ((unfolding (unfold and then complete)) otherwise give 77) else
    (if value < 0 then escape value else
      (check (equal(name, "ok") + equal(name, "LLS_ACTION_SEMANTICS_ENTRY")) then give value + 1)))
.end
""", encoding="utf-8")
        cases = [
            ("41 ok", "42"),
            ("41 LLS_ACTION_SEMANTICS_ENTRY", "42"),
            ("-1 ok", "ACSEM: ActionSemantics action escaped"),
            ("-2 ok", "ACSEM: ActionSemantics input or resource error"),
            ("41 bad", "ACSEM: ActionSemantics action failed"),
            ("2147483647 ok", "ACSEM: ActionSemantics action failed"),
            ("2147483648 ok", "must fit a signed 32-bit integer"),
            ("41 " + "x" * 65, "must be an ASCII identifier"),
        ]
        for target in ("python3", "c99", "c11", "cpp17", "cpp20"):
            with self.subTest(target=target):
                cc = shutil.which("cc")
                cxx = shutil.which("c++")
                if target != "python3" and (not cc or target.startswith("cpp") and not cxx):
                    continue
                output = "tool.py" if target == "python3" else "tool.cpp" if target.startswith("cpp") else "tool.c"
                args = ["--split", "all"] if target == "cpp17" else []
                result = self.generator("--mode", "interpret", "-T", target, "-o", output, *args)
                self.assertEqual(result.returncode, 0, result.stderr)
                if target == "python3":
                    run = [sys.executable, output]
                else:
                    warnings = ["-Wall", "-Wextra", "-Werror", "-pedantic"]
                    if target.startswith("cpp"):
                        result = self.run_cmd(cc, "-std=c11", *warnings,
                                              "-c", "tool.actionsemantics.c", "-o", "action.o")
                        self.assertEqual(result.returncode, 0, result.stderr)
                        files = (sorted(self.work.glob("Parity_*.cpp")) if args else []) + [output, "action.o"]
                        compiler = [cxx, "-std=" + target.replace("cpp", "c++")]
                    else:
                        files = [output, "tool.actionsemantics.c"]
                        compiler = [cc, "-std=" + target]
                    result = self.run_cmd(*compiler, *warnings, *files, "-lm", "-o", "tool")
                    self.assertEqual(result.returncode, 0, result.stderr)
                    run = [str(self.work / "tool")]
                for program, expected in cases:
                    with self.subTest(program=program):
                        result = self.run_cmd(*run, "--interpret", source=program)
                        if expected == "42":
                            self.assertEqual((result.returncode, result.stdout), (0, "42\n"), result.stderr)
                        else:
                            self.assertNotEqual(result.returncode, 0)
                            self.assertIn(expected, result.stderr)
                            self.assertIn("Datum", result.stderr)

    @unittest.skipUnless(ACSEM, "build acsem or set LLSYNTH_TEST_ACSEM")
    def test_imported_token_action_and_result_boundary(self):
        part = self.work / "part.llsynth"
        part.write_text(
            """\
.module Part
.export Atom
.token Word: String = /[a-z]+/
.rule Atom
'syntax: word:Word
'semantics actionsemantics(word: String) -> String: give word
.end
""", encoding="utf-8")
        self.spec.write_text(
            """\
.language "Import"
.import "part.llsynth" as P
.rule Start
'syntax: word:P->Atom EOF
'semantics actionsemantics(word: String) -> String: give word
.end
""", encoding="utf-8")
        result = self.generator("--mode", "interpret", "-o", "tool.py")
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_cmd(sys.executable, "tool.py", "--interpret", source="name")
        self.assertEqual((result.returncode, result.stdout), (0, "name\n"), result.stderr)
        self.spec.write_text(self.spec.read_text().replace("-> String: give word", "-> Void: give word"))
        self.assertEqual(self.generator("--mode", "interpret", "-o", "tool.py").returncode, 0)
        result = self.run_cmd(sys.executable, "tool.py", "--interpret", source="name")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("incompatible with Void", result.stderr)
        self.spec.write_text(self.spec.read_text().replace("-> Void: give word", "-> Void: complete"))
        self.assertEqual(self.generator("--mode", "interpret", "-o", "tool.py").returncode, 0)
        result = self.run_cmd(sys.executable, "tool.py", "--interpret", source="name")
        self.assertEqual((result.returncode, result.stdout), (0, "None\n"), result.stderr)

    def test_binding_and_missing_tool_diagnostics(self):
        prefix = ".token N: Int = /[0-9]+/\n.rule Start\n"
        cases = [
            ("'syntax: N EOF\n'semantics actionsemantics(value: Int) -> Int: give value",
             "must occur in every syntax alternative"),
            ("'syntax: value:N EOF\n'semantics actionsemantics(value: String) -> Int: give 0",
             "expects String, got Int"),
            ("'syntax: value:N? EOF\n'semantics actionsemantics(value: Int) -> Int: give value",
             "grouped, repeated or optional"),
            ("'syntax: value:N EOF\n'semantics actionsemantics(value: Int) -> Int: give value\n"
             "'semantics: return $value;", "cannot combine"),
            ("'syntax: value:N EOF\n'semantics: return $value;\n"
             "'semantics actionsemantics(value: Int) -> Int: give value", "cannot combine"),
            ("'syntax: N EOF\n'semantics actionsemantics() -> Int: give 7;",
             "without a terminating semicolon"),
            ("'syntax: N EOF\n'semantics actionsemantics() -> Int: give 7",
             "not found or not executable"),
        ]
        for rule, expected in cases:
            with self.subTest(expected=expected):
                self.spec.write_text(prefix + rule + "\n.end\n")
                result = self.generator("--mode", "interpret", "--check",
                                        "--diagnostics", "json", tool=str(self.work / "not-acsem"))
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, json.loads(result.stderr)["message"])

    @unittest.skipUnless(ACSEM, "build acsem or set LLSYNTH_TEST_ACSEM")
    def test_novostone_validation_maps_back_to_facet_source(self):
        part = self.work / "body.inc"
        part.write_text("  give undeclared\n", encoding="utf-8")
        self.spec.write_text(
            ".rule Start\n'syntax: EOF\n'semantics actionsemantics() -> Int:\n"
            '.include "body.inc"\n.end\n', encoding="utf-8")
        result = self.generator("--mode", "interpret", "--check", "--diagnostics", "json")
        self.assertNotEqual(result.returncode, 0)
        diag = json.loads(result.stderr)
        self.assertIn("ActionSemantics", diag["message"])
        self.assertIn("undeclared", diag["message"])
        self.assertEqual(Path(diag["location"]["file"]), part)
        self.assertEqual(diag["location"]["line"], 1)
        self.assertFalse((self.work / "language.py").exists())


if __name__ == "__main__":
    unittest.main()
