"""Exercise extension discovery and consume the generated Vim, LaTeX and DOT."""
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


class Extensions(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.work = Path(tmp.name)
        self.spec = self.work / "lang.llsynth"
        self.spec.write_text(r'''.language "Tiny & Co"
.summary "100% café #1: {a_b} \input & $x$"
.skip /\s+/
.token N: %Int = /[0-9]+/
.rule Start
'syntax: "show" "{" Items "}" EOF
.end
.rule Items
'syntax: N+
.end
''', encoding="utf-8")

    def command(self, *args, success=True):
        p = subprocess.run(args, cwd=self.work, text=True, capture_output=True, timeout=45)
        self.assertEqual(p.returncode == 0, success, (args, p.stdout, p.stderr))
        return p

    def test_discovery_and_custom_perl_extension(self):
        plugins = self.work / "plugins"
        plugins.mkdir()
        (plugins / "Broken.pm").write_text('die "not selected";\n')
        (plugins / "Title.pm").write_text('''package LLSynth::Extension::Title;
sub render { my ($s, $stem) = @_; return {"$stem.txt" => $s->{language}} }
1;
''')
        args = (DRIVER, "--extension-path", plugins)
        listing = self.command(*args, "--list-extensions").stdout.splitlines()
        self.assertEqual(listing, sorted(listing))
        self.assertTrue({"Broken", "Title", "VimGen", "PrettyPrint", "GrammarGraph"} <= set(listing))
        self.command(*args, "--extension", "Title", "-o", "title.txt",
                     "--depfile", "title.d", self.spec)
        self.assertEqual((self.work / "title.txt").read_text(), "Tiny & Co")
        deps = (self.work / "title.d").read_text()
        self.assertIn(str(plugins / "Title.pm"), deps)
        self.assertNotIn("Broken.pm", deps)
        err = self.command(*args, "--diagnostics=json", "--extension", "../Title", self.spec, success=False)
        self.assertIn("unknown extension", json.loads(err.stderr)["message"])

    def test_output_preflight(self):
        before = self.spec.read_bytes()
        err = self.command(DRIVER, "--extension", "PrettyPrint", "-o", self.spec, self.spec, success=False)
        self.assertIn("output would overwrite an input", err.stderr)
        self.assertEqual(self.spec.read_bytes(), before)
        err = self.command(DRIVER, "--extension", "VimGen", "-o", "one", self.spec, success=False)
        self.assertIn("requires exactly one extension artifact", err.stderr)
        self.assertFalse((self.work / "one").exists())

    def test_bundle(self):
        out = self.work / "out"
        self.command(DRIVER, "--extension", "PrettyPrint", "--extension", "VimGen",
                     "--extension", "GrammarGraph", "--out-dir", out, "--manifest", "manifest.json", self.spec)
        expected = {"syntax/tiny_co.vim", "indent/tiny_co.vim", "tiny_co.tex", "tiny_co.dot"}
        outputs = json.loads((out / "manifest.json").read_text())["outputs"]
        self.assertEqual(expected, {str(Path(f["path"]).relative_to(out)) for f in outputs})
        tex = (out / "tiny_co.tex").read_text()
        self.assertIn(r"100\% café \#1: \{a\_b\} \textbackslash{}input \& \$x\$", tex)
        self.assertIn(r"\subsection*{Items}", tex)
        if shutil.which("pdflatex"):
            self.command("pdflatex", "-halt-on-error", "-interaction=batchmode", out / "tiny_co.tex")
        graph = (out / "tiny_co.dot").read_text()
        self.assertIn('"Start" -> "Items";', graph)
        if shutil.which("dot"):
            self.command("dot", "-Tsvg", out / "tiny_co.dot", "-o", self.work / "rules.svg")
        if shutil.which("vim"):
            script = self.work / "check.vim"
            script.write_text(r"""set encoding=utf-8 shiftwidth=2 expandtab
source out/syntax/tiny_co.vim
source out/indent/tiny_co.vim
call setline(1, ['show {', '42', '}'])
normal! gg=G
call assert_equal('llsKeyword', synIDattr(synID(1, 1, 1), 'name'))
call assert_equal('llsOperator', synIDattr(synID(1, 6, 1), 'name'))
call assert_equal('Number', synIDattr(synIDtrans(synID(2, 3, 1)), 'name'))
call assert_equal(['show {', '  42', '}'], getline(1, '$'))
if len(v:errors)
  call writefile(v:errors, 'vim-errors.txt')
  cquit
endif
qa!
""")
            p = subprocess.run(["vim", "-Nu", "NONE", "-n", "-es", "-S", script], cwd=self.work, timeout=15)
            errors = self.work / "vim-errors.txt"
            self.assertEqual(p.returncode, 0, errors.read_text() if errors.exists() else "Vim failed")


if __name__ == "__main__":
    unittest.main()
