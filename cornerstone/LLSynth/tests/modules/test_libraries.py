"""Libraries, open imports, and generator-time declaration providers."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class Libraries(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.write("Widgets/Numbers.llsynth", '''.library "Widgets.Numbers"
.summary "Reusable arithmetic"
.export Num, plus, @seed, %Value
.type %Value = Int
.state @seed: %Value = 1
.token Num: Int = /[0-9]+/
.function plus(x: %Value) -> %Value
return x + @seed;
.end
''')

    def write(self, name, source):
        path = self.work / name
        path.parent.mkdir(exist_ok=True, parents=True)
        path.write_text(source, encoding="utf-8")
        return path

    def invoke(self, *args, stdin=None, code=0):
        run = subprocess.run(args, input=stdin, cwd=self.work, text=True,
                             capture_output=True, timeout=30)
        self.assertEqual(run.returncode, code, (run.stdout, run.stderr))
        return run

    def generate(self, *args, code=0):
        return self.invoke("perl", DRIVER, "--mode=interpret", *args, "main.llsynth", code=code)

    def main(self, imports, result, grammar="EOF"):
        self.write("main.llsynth", f'''.language "Arithmetic"
{imports}
.rule Start
'syntax: {grammar}
'semantics: {result}
.end
''')

    def test_library_bindings_across_syntax_actions_and_types(self):
        for suffix, prefix in [("", ""), (" qualified", "Widgets__Numbers__"),
                               (" qualified", "Widgets.Numbers."), (" as N", "N."),
                               (" as N", "N__")]:
            with self.subTest(suffix=suffix, prefix=prefix):
                self.main(f'.import "Widgets.Numbers"{suffix}',
                          f'let v: %{prefix}Value = &{prefix}plus($n); return v + @{prefix}seed;',
                          f'n:{prefix}Num EOF')
                self.generate("-o", "tool.py")
                self.assertEqual(self.invoke(sys.executable, "tool.py", stdin="40").stdout, "42\n")

    def test_plain_conflict_and_private_export(self):
        self.write("More.llsynth", '''.library More
.function plus(x: Int) -> Int
return x;
.end
''')
        self.main('.import Widgets.Numbers\n.import More', 'return 1;')
        self.assertIn("ambiguous import 'plus'", self.generate("--check", code=1).stderr)
        self.main('.import qualified Widgets.Numbers', 'return plus(2);')
        self.assertIn("undeclared", self.generate("--check", code=1).stderr)
        self.main('.import More as M', 'return M.missing(2);')
        self.assertIn("does not export missing", self.generate("--check", code=1).stderr)

    def test_quoted_identity_and_literal_file_precedence(self):
        self.write("Numbers.llsynth", ".library Numbers\n.template increment(x) = x + 1\n")
        self.main('.import "Numbers" as N', 'return N.increment(40);')
        self.generate("-o", "tool.py")
        self.assertEqual(self.invoke(sys.executable, "tool.py", stdin="").stdout, "41\n")
        self.write("Numbers", ".module Other\n.export increment\n.template increment(x) = x + 2\n")
        self.generate("-o", "tool.py")
        self.assertEqual(self.invoke(sys.executable, "tool.py", stdin="").stdout, "42\n")

    def test_perl_companion_manifest_package_and_import_once(self):
        pm = self.write("Widgets/Exports.pm", """[
    '.function supplied(x: Int) -> Int',
    'return x + 1;',
    '.end',
]
""")
        self.write("Widgets/Provider.llsynth", '''.library "Widgets.Provider"
.extern "Exports.pm"
.extern perl "./Exports.pm"
''')
        self.main('.import Widgets.Provider\n.import Widgets.Provider as Again',
                  'return Again.supplied(supplied(40));')
        self.generate("--package", "demo", "--manifest", "m.json", "--depfile", "demo.d")
        manifest = json.loads((self.work / "m.json").read_text())
        deps = {Path(d["path"]): d["sha256"] for d in manifest["dependencies"]}
        self.assertEqual(deps[pm], hashlib.sha256(pm.read_bytes()).hexdigest())
        self.assertIn(str(pm), (self.work / "demo.d").read_text())
        self.assertIn("overwrite", self.generate("-o", pm, code=1).stderr)
        shutil.rmtree(pm.parent)
        self.assertEqual(self.invoke(sys.executable, "-m", "demo", stdin="").stdout, "42\n")

    def test_companion_in_language_and_error_locations(self):
        self.main('.extern perl "missing.pl"', 'return answer();')
        err = json.loads(self.generate("--check", "--diagnostics=json", code=1).stderr)
        self.assertEqual(err["location"]["line"], 2)
        self.assertIn("companion 'missing.pl' not found", err["message"])
        self.write("missing.pl", """die "bad declaration provider";""")
        self.assertIn("bad declaration provider", self.generate("--check", code=1).stderr)
        self.write("missing.pl", """'.function answer() -> Int
return 42;
.end';""")
        self.generate("-T", "c99", "-o", "tool.c")
        if shutil.which("cc"):
            self.invoke("cc", "-std=c99", "tool.c", "-lm", "-o", "tool")
            self.assertEqual(self.invoke("./tool", stdin="").stdout, "42\n")

    @unittest.skipUnless(shutil.which("cc"), "C compiler required")
    def test_real_c_companion(self):
        self.write("Widgets/export.c", r'''
const char *llsynth_declarations(void) {
    return ".function answer() -> Int\nreturn 42;\n.end\n";
}
''')
        self.invoke("cc", "-shared", "-fPIC", "Widgets/export.c", "-o", "Widgets/Exports.so")
        self.write("Widgets/Provider.llsynth", '.library Widgets.Provider\n.extern c "Exports.so"\n')
        self.main('.import Widgets.Provider as P', 'return P.answer();')
        ffi = subprocess.run(["perl", "-MFFI::Platypus", "-e", "1"], capture_output=True)
        if ffi.returncode:
            self.assertIn("require Perl FFI::Platypus", self.generate("--check", code=1).stderr)
            self.skipTest("FFI::Platypus unavailable; missing dependency diagnostic verified")
        self.generate("-o", "tool.py")
        self.write("Widgets/Provider.llsynth", '.library Widgets.Provider\n.extern "export.c"\n')
        (self.work / "Widgets/Exports.so").unlink()
        before = set((self.work / "Widgets").iterdir())
        self.generate("-o", "source.py", "--manifest", "m.json")
        self.assertEqual(set((self.work / "Widgets").iterdir()), before)
        deps = json.loads((self.work / "m.json").read_text())["dependencies"]
        self.assertIn(str(self.work / "Widgets/export.c"), [d["path"] for d in deps])
        shutil.rmtree(self.work / "Widgets")
        self.assertEqual(self.invoke(sys.executable, "tool.py", stdin="").stdout, "42\n")
        self.assertEqual(self.invoke(sys.executable, "source.py", stdin="").stdout, "42\n")


if __name__ == "__main__":
    unittest.main()
