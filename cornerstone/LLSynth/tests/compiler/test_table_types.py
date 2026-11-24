"""Static checks for first-class tables, member access and method context."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CLI = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class TableTypeTests(unittest.TestCase):
    def compile(self, action, declarations="", diagnostic=None, *, target="python3", run=False):
        source = f"{declarations}\n.rule Main\n'syntax: EOF\n'semantics: {action}\n.end\n"
        with tempfile.TemporaryDirectory(prefix="llsynth-table-types-") as tmp:
            spec, tool = Path(tmp) / "test.llsynth", Path(tmp) / "generated.py"
            spec.write_text(source, encoding="utf8")
            option = ["-o", tool] if run else ["--check"]
            build = subprocess.run(
                ["perl", CLI, "--mode=interpret", "-T", target, *option, spec],
                capture_output=True, text=True, timeout=30,
            )
            if diagnostic is not None:
                self.assertNotEqual(build.returncode, 0, build.stdout)
                self.assertIn(diagnostic, build.stderr)
            else:
                self.assertEqual(build.returncode, 0, build.stderr)
            if run:
                result = subprocess.run(
                    [sys.executable, tool], input="", capture_output=True, text=True, timeout=30,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                return result.stdout.strip()

    def test_state_delimiter_and_nested_vectors(self):
        self.assertEqual(self.compile("""
            let rows: Vec<Vec<Int>> = [[40], [2]];
            let row: Vec<Vec<Int>> = [ [42] ];
            return @data.semi == @semi && rows[0][0] + rows[1][0] == row[0][0];
        """, '.state @data: %Table = [["semi": ";"]];\n.state @semi: String = ";";',
            run=True), "true")

    def test_table_static_checks(self):
        for action, message in (
            ("let t: %Table = [1, 2];", "expected %Table, got Vec<Int>"),
            ('let t = [[nil: 1]];', "table key cannot be nil"),
            ('let t = [[]]; t[nil] = 2;', "table key cannot be nil"),
            ('let t = [["*Constructor": 2]];', "table prototype"),
            ('let t = [[]]; t["*Constructor"] = 2;', "table prototype"),
            ('let n = 1; discard n:read();', "method receiver"),
            ('let n = 1; discard n.read;', "member access requires a table or nominal value"),
        ):
            with self.subTest(action=action):
                self.compile(action, diagnostic=message)
        self.compile("let t = [[]];", target="c99", diagnostic="require the python3")

    def test_metavariables_have_checked_types_and_scope(self):
        self.compile("return *Args;", diagnostic="requires a callable body")
        self.compile("return *Typo;", diagnostic="unknown metavariable '*Typo'")
        self.compile("let f = fn() -> Int { return *Type; };",
                     diagnostic="expected Int, got String")
        self.compile('let t = [["f": fn() -> String { return *Type; }]]; return t:f();')

    def test_dynamic_free_names_are_specific_to_sync_table_entry_closures(self):
        self.assertEqual(self.compile("""
            let t = [["f": fn() -> Int { n = n + 1; return n; }]];
            let n = 41;
            discard t.f();
            return n;
        """, run=True), "42")
        self.compile('let f = fn() -> Int { return n; };', diagnostic="undeclared variable 'n'")
        self.compile('let t = [["f": async fn() -> Int { return n; }]];',
                     diagnostic="undeclared variable 'n'")


if __name__ == "__main__":
    unittest.main()
