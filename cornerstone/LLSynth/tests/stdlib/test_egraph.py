"""Run std.egraph through LLSynth analyzers and the actual EquinoxNG bridge."""

import ctypes
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
SOURCE = LLS / "stdlib" / "egraph"
CC = shlex.split(os.environ.get("CC", "cc"))
SPEC = """\
.language "EGraph example"
.import std.egraph as E
.rule Start
'syntax: EOF
'analyze: print(E:analyze("(add (add value 0) 0)", "(=> (add ?x 0) ?x)"));
.end
"""


def command(*args, cwd=None, env=None, source=""):
    return subprocess.run(args, cwd=cwd, env=env, input=source, capture_output=True,
                          text=True, timeout=60, check=False)


class EGraphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not shutil.which("cmake") or not shutil.which("c++"):
            raise unittest.SkipTest("CMake and a C++20 compiler are required")
        temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(temporary.cleanup)
        cls.build = Path(temporary.name)
        vendor = os.environ.get("LLSYNTH_EQUINOX_DIR")
        options = [f"-DLLSYNTH_EQUINOX_DIR={vendor}"] if vendor else []
        for args in (("cmake", "-S", SOURCE, "-B", cls.build, *options),
                     ("cmake", "--build", cls.build)):
            result = command(*args)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        native = ctypes.CDLL(str(cls.build / "libllsynth_egraph.so"))
        cls.run_graph = native.llsynth_egraph_run
        cls.run_graph.argtypes = [ctypes.c_char_p] * 3 + [ctypes.c_int] + [ctypes.c_int64] * 2
        cls.run_graph.restype = ctypes.c_char_p

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)
        (self.work / "example.llsynth").write_text(SPEC, encoding="utf-8")

    def query(self, term, rules="", *, other=None, iterations=12, nodes=1024):
        return json.loads(self.run_graph(term.encode(), (other or "").encode(), rules.encode(),
                                         other is not None, iterations, nodes))

    def check(self, *args, **kwargs):
        result = command(*args, cwd=self.work, **kwargs)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def test_python_and_native_analysis_facets(self):
        env = {**os.environ,
               "LD_LIBRARY_PATH": str(self.build) + os.pathsep + os.environ.get("LD_LIBRARY_PATH", "")}
        for target, output in [("python3", "example.py"), ("c11", "example.c")]:
            with self.subTest(target=target):
                self.check("perl", DRIVER, "--mode=analyze", "-T", target, "-o", output,
                           "--depfile", "example.d", "example.llsynth")
                self.assertIn(str(SOURCE.with_suffix(".llsynth")),
                              (self.work / "example.d").read_text(encoding="utf-8"))
                if target == "python3":
                    result = self.check(sys.executable, output, "--analyze", env=env)
                elif CC and shutil.which(CC[0]):
                    self.check(*CC, "-std=c11", output, "-L", self.build,
                               "-l", "llsynth_egraph", "-lm", "-o", "example")
                    result = self.check("./example", "--analyze", env=env)
                else:
                    self.skipTest("C compiler unavailable")
                report, done = result.stdout.strip().splitlines()
                expected = self.query("(add (add value 0) 0)", "(=> (add ?x 0) ?x)")
                self.assertEqual(json.loads(report), expected)
                self.assertEqual((expected["status"], expected["term"], expected["cost"]),
                                 ("saturated", "value", 1))
                self.assertEqual(done, "analysis OK")

    def test_congruence_and_repeated_variable(self):
        report = self.query("(pair (wrap (add x 0)) (wrap x))",
                            "(=> (add ?x 0) ?x) (=> (pair ?same ?same) (equal))",
                            other="(equal)")
        self.assertEqual((report["status"], report["term"]), ("equivalent", "(equal)"))
        self.assertGreaterEqual(report["unions"], 3)
        report = self.query("(pair (wrap a) (wrap b))",
                            "(=> (pair ?same ?same) (equal))", other="(equal)")
        self.assertEqual((report["status"], report["stop"]), ("unknown", "saturated"))

    def test_query_respects_direction_and_has_no_implicit_axioms(self):
        self.assertEqual(self.query("(add 2 3)", other="5")["status"], "unknown")
        self.assertEqual(self.query("leaf", other="(leaf)")["status"], "unknown")
        report = self.query("(f x)", "(=> (f ?x) (g ?x))", other="(g x)")
        self.assertEqual(report["status"], "equivalent")
        self.assertEqual(self.query("same", other="same")["iterations"], 0)

    def test_budget_stops_have_distinct_status(self):
        growing = "(=> (f ?x) (g (f (h ?x))))"
        report = self.query("(f x)", growing, nodes=14)
        self.assertEqual((report["status"], report["term"]), ("node-limit", "(f x)"))
        self.assertLessEqual(report["enodes"], 14)
        report = self.query("(f x)", growing, other="unrelated", nodes=14)
        self.assertEqual((report["status"], report["stop"]), ("unknown", "node-limit"))

        cascade = "(=> (g ?x) ?x) (=> (f ?x) (g ?x))"
        self.assertEqual(self.query("(f x)", cascade, iterations=1)["status"], "iteration-limit")
        self.assertEqual(self.query("(f x)", cascade)["term"], "x")

        fanout = " ".join(f"(=> (wrap a) (wrap {name}))" for name in "bcdefghi")
        pattern = " ".join(f"(wrap ?v{i})" for i in range(8))
        expression = "(pair " + " ".join(["(wrap a)"] * 8) + ")"
        report = self.query(expression, fanout + f" (=> (pair {pattern}) done)")
        self.assertEqual(report["status"], "work-limit")

    def test_diagnostics(self):
        for term, rules, limits, message in [
            ("", "", {}, "expected an unquoted ASCII atom"),
            ("(f x", "", {}, "expected ')'"),
            ("x y", "", {}, "expected one term"),
            ("?notGround", "", {}, "variables are only allowed in rewrite rules"),
            ("x", "(=> ?a atom)", {}, "left side cannot be a bare variable"),
            ("x", "(=> (f ?a) ?missing)", {}, "unbound right-side variable ?missing"),
            ("x", "(=> (f a?b) a)", {}, "invalid character in atom"),
            ("x", "(=> (f ?a) a b)", {}, "expected ')' after the two rule terms"),
            ("x", "(=> (f ?a) a)" * 65, {}, "too many rewrite rules"),
            ("(f " * 33 + "x" + ")" * 33, "", {}, "nesting limit"),
            ("x", "", {"iterations": 0}, "iterations must be"),
            ("x", "", {"nodes": 4097}, "nodes must be"),
            ("(f x)", "", {"nodes": 1}, "input terms exceed the e-node limit"),
        ]:
            with self.subTest(message=message):
                result = self.query(term, rules, **limits)
                self.assertEqual(result["status"], "error")
                self.assertIn(message, result["message"])


if __name__ == "__main__":
    unittest.main()
