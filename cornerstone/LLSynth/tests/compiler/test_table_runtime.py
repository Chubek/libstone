"""Exercise hybrid tables in the generated Python interpreter."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLI = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class TableRuntimeTests(unittest.TestCase):
    def run_action(self, action, expected, *, error=False):
        source = ".rule Main\n'syntax: EOF\n'semantics: " + action + "\n.end\n"
        with tempfile.TemporaryDirectory(prefix="llsynth-table-") as tmp:
            root = Path(tmp)
            spec, tool = root / "test.llsynth", root / "generated.py"
            spec.write_text(source, encoding="utf8")
            build = subprocess.run(
                ["perl", CLI, "--mode=interpret", "-T", "python3", "-o", tool, spec],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run(
                [sys.executable, tool], input="", capture_output=True, text=True, timeout=30,
            )
            if error:
                self.assertNotEqual(run.returncode, 0, run.stdout)
                self.assertIn(expected, run.stderr)
            else:
                self.assertEqual(run.returncode, 0, run.stderr)
                self.assertEqual(run.stdout.strip(), expected)

    def test_mixed_numeric_and_named_keys(self):
        self.run_action("""
            let t: %Table = [[8, "label": 3, 7: 40, 2, true: 100]];
            t.label = t[7] + t[2];
            t[1.0] = t.label;
            let n = 0;
            for key in t { n = n + 1; }
            return t[1] == 42 && t[true] == 100 && "label" in t
                && !(3 in t) && n == len(t);
        """, "true")

    def test_prototype_shadowing_and_nil_deletion(self):
        self.run_action("""
            let prototype = [["value": 42]];
            let t = [["*Constructor": prototype]];
            let nested = [["*Constructor": t]];
            t.value = 7;
            let shadowed = nested.value;
            t["value"] = nil;
            return shadowed == 7 && nested.value == 42
                && "value" in nested && len(nested) == 1 && nested.missing == nil;
        """, "true")

    def test_table_identity_and_cycle_safe_formatting(self):
        self.run_action("""
            let t = [[]];
            let alias = t;
            let distinct = [[]];
            t[t] = 42;
            t.self = t;
            if alias == t && distinct != t && !(distinct in t) && t[t] == 42 {
                return t;
            }
            return distinct;
        """, "{{...}: 42, 'self': {...}}")

    def test_prototype_cycle_is_an_error(self):
        self.run_action("""
            let t = [[]];
            t["*Constructor"] = t;
            return t.missing;
        """, "table prototype cycle", error=True)


if __name__ == "__main__":
    unittest.main()
