"""Specification-module linking and generated-runtime regression tests."""

import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class ModuleTests(unittest.TestCase):
    driver = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"

    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="llsynth-modules-")
        self.addCleanup(temp.cleanup)
        self.work = Path(temp.name)
        self.root = self.work / "main.llsynth"

    def write(self, name, text):
        path = self.work / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def run_command(self, *args, code=0, stdin=None):
        result = subprocess.run(
            args, cwd=self.work, input=stdin, text=True, capture_output=True, timeout=30
        )
        self.assertEqual(result.returncode, code, (args, result.stdout, result.stderr))
        return result

    def generate(self, *args, code=0):
        return self.run_command(
            "perl", self.driver, "--mode=interpret", *args, "--", self.root, code=code
        )

    def basic(self):
        self.write("words.llsynth", r""".module Words
.export Number, value, @count, #symbols, !Bad
.include "tokens.inc"
.state @count: Int = 0
.symtbl #symbols: String -> Int
.exception !Bad(reason: String) "{reason}"
.function helper(x: Int) -> Int
return x + 1;
.end
.function value(x: Int) -> Int
return helper(x);
.end
.init
@count = @count + 1;
.end
.rule Number
'syntax: Number:NUMBER
'semantics: return &value($Number);
.end
""")
        self.write("tokens.inc", ".token NUMBER: Int = /[0-9]+/\n")
        self.write("main.llsynth", r""".language "Modules"
.import "words.llsynth" as W
.rule Start
'syntax: n:W__Number EOF
'semantics: return &W__value($n) + @W__count;
.end
""")

    def test_python_and_c_are_self_contained(self):
        self.basic()
        self.generate("-o", "tool.py")
        self.generate("-T", "c99", "-o", "tool.c")
        # Remove every specification input before running either generated tool.
        for path in self.work.glob("*.llsynth"):
            path.unlink()
        (self.work / "tokens.inc").unlink()
        run = self.run_command(sys.executable, self.work / "tool.py", stdin="40")
        self.assertEqual(run.stdout, "43\n")
        if shutil.which("cc"):
            self.run_command("cc", "-std=c99", "tool.c", "-lm", "-o", "tool")
            run = self.run_command(self.work / "tool", stdin="40")
            self.assertEqual(run.stdout, "43\n")

    def test_arrow_accessors_and_colon_calls(self):
        self.write("Lib/Words.llsynth", """.library "Lib.Words"
.export Number, increment, @count, #symbols, !Bad, %Value
.type %Value = Int
.state @count: %Value = 1
.symtbl #symbols: String -> %Value
.exception !Bad(reason: String) "{reason}"
.init
#symbols["x"] = 2;
.end
.token DIGITS: Int = /[0-9]+/
.rule Number
'syntax: n:DIGITS
'semantics: return $n;
.end
.function increment(x: %Value) -> %Value
return x + 1;
.end
.function private() -> Int
return 0;
.end
""")
        source = """.import qualified Lib.Words
.import Lib.Words as W
.alias &W->increment &inc
.function twice (x: %W->Value) -> %Lib.Words->Value
return W:increment(&Lib.Words->increment(x));
.end
.rule Start
'syntax: n:Lib.Words->Number EOF
'semantics:
require @W->count == 1 else Throw(!W->Bad, "count");
return Lib.Words:increment(twice($n)) + &inc(#W->symbols["x"]) + W__increment(0);
.end
"""
        self.write("main.llsynth", source)
        for target, filename in (("python3", "tool.py"), ("c99", "tool.c")):
            self.generate("-T", target, "-o", filename)
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="40").stdout, "47\n")
        if shutil.which("cc"):
            self.run_command("cc", "-std=c99", "tool.c", "-lm", "-o", "tool")
            self.assertEqual(self.run_command(self.work / "tool", stdin="40").stdout, "47\n")
        for old, new, diagnostic in (
            ("W:increment(", "W:private(", "does not export private"),
            ("&Lib.Words->increment(", "&W->private(", "does not export private"),
            ("@W->count", "@Missing->count", "unknown module alias 'Missing'"),
            ("Lib.Words->Number", "W->DIGITS", "does not export DIGITS"),
        ):
            with self.subTest(reference=new):
                self.write("main.llsynth", source.replace(old, new))
                self.assertIn(diagnostic, self.generate("--check", code=1).stderr)

    def test_colons_and_arrows_preserve_table_methods_and_function_types(self):
        self.write("lib.llsynth", """.library "Lib"
.type %Value = Int
.function increment(x: Int) -> Int
return x + 1;
.end
.function table_result() -> Int
let t = [["increment":fn(x:Int)->Int { return x + 2; }, "value":3]];
t.value = t:increment(t.value);
return t.value;
.end
""")
        self.write("main.llsynth", """.import "lib.llsynth" as W
.import "lib.llsynth" as Point
.type %Number = Int
.prodtype %Record
increment: Int
.end
.function apply(W:Fn(Int)->Int) -> Int
return W(4);
.end
.rule Start
'syntax: "ok" EOF
'semantics:
let W = [["increment":fn(x:Int)->Int { return x + 10; }]];
let cb:%Callable[%Number->Int] = &W->increment;
let typed: %Callable[%Int -> %W->Value] = cb;
let Point = %Record(7);
return W:increment(apply(typed)) + (W):increment(1) + W:table_result() + Point.increment;
.end
""")
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="ok").stdout, "29\n")

    def test_colon_receiver_follows_the_reexporting_library(self):
        self.write("origin.llsynth", """.library "Origin"
.export read, step, @count
.state @count: Int = 3
.function step(x: Int) -> Int
return x + 1;
.end
.function read(x: Int) -> Int
return *This.step(*Argv[0]) + *This.count;
.end
""")
        self.write("wrapper.llsynth", """.library "Wrapper"
.export read, step, @count
.import "origin.llsynth" as O
.alias &O->read &read
.state @count: Int = 10
.function step(x: Int) -> Int
return x + 2;
.end
.function private() -> Int
return 100;
.end
""")
        source = """.import "origin.llsynth" as O
.import "wrapper.llsynth" as W
.rule Start
'syntax: EOF
'semantics:
@W->count = 20;
return O:read(5) + W:read(5);
.end
"""
        self.write("main.llsynth", source)
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="").stdout, "36\n")
        result = self.generate("-T", "c99", "--check", code=1)
        self.assertIn("method context require the python3 tool target", result.stderr)
        self.write("main.llsynth", source.replace("O:read(5) + W:read(5)", "&W->read(5)"))
        self.generate("-o", "tool.py")
        result = self.run_command(sys.executable, "tool.py", stdin="", code=1)
        self.assertIn("metavariable *This is unavailable outside a colon call", result.stderr)
        origin = (self.work / "origin.llsynth").read_text()
        self.write("origin.llsynth", origin.replace("*This.step(*Argv[0]) + *This.count", "*This.private()"))
        self.write("main.llsynth", source.replace("O:read(5) + W:read(5)", "W:read(5)"))
        self.generate("-o", "tool.py")
        result = self.run_command(sys.executable, "tool.py", stdin="", code=1)
        self.assertIn("module 'Wrapper' does not export runtime member 'private'", result.stderr)

    def test_import_once_diamond_aliases_and_include_fingerprints(self):
        self.basic()
        self.write("left.llsynth", """.module Left
.export inc
.import "words.llsynth" as Base
.function inc(x: Int) -> Int
return &Base__value(x);
.end
""")
        self.write("right.llsynth", """.module Right
.export inc
.import "words.llsynth" as Base
.function inc(x: Int) -> Int
return &Base__value(x);
.end
""")
        with self.root.open("a", encoding="utf-8") as out:
            out.write('.import "left.llsynth" as L\n.import "right.llsynth" as R\n')
            out.write('.import "./words.llsynth" as Again\n')
        self.generate("-o", "tool.py", "--manifest", "manifest.json", "--depfile", "tool.d")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="40").stdout, "43\n")
        manifest = json.loads((self.work / "manifest.json").read_text())
        deps = manifest["dependencies"]
        self.assertEqual(len(deps), 5)
        self.assertEqual([d["path"] for d in deps], sorted(d["path"] for d in deps))
        for dep in deps:
            path = Path(dep["path"])
            self.assertEqual(dep["sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
            self.assertIn(str(path), (self.work / "tool.d").read_text())
        before = (self.work / "tool.py").read_bytes()
        self.generate("-o", "tool.py")
        self.assertEqual((self.work / "tool.py").read_bytes(), before)

    def test_private_names_and_capture_names_are_isolated(self):
        self.basic()
        self.write("other.llsynth", """.module Other
.export helper
.function helper(x: Int) -> Int
return x + 100;
.end
""")
        text = self.root.read_text().replace(
            '.import "words.llsynth" as W',
            '.import "words.llsynth" as W\n.import "other.llsynth" as O',
        ).replace("return &W__value($n) + @W__count;", "return &O__helper($n);")
        self.root.write_text(text)
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="40").stdout, "141\n")
        self.root.write_text(text.replace("O__helper", "W__helper"))
        result = self.generate("--check", "--diagnostics=json", code=1)
        diag = json.loads(result.stderr)
        self.assertIn("does not export helper", diag["message"])
        self.assertEqual(diag["location"]["file"], str(self.root))
        self.assertEqual(diag["location"]["line"], 6)

    def test_named_loading_local_precedence_and_ordered_paths(self):
        self.write("first/Lib/Words.llsynth", ".module Lib.Words\n.export Word\n.rule Word\n'syntax: \"ok\"\n.end\n")
        self.write("second/Lib/Words.llsynth", ".wrong\n")
        self.write("main.llsynth", """.import Lib.Words
.rule Start
'syntax: Words__Word EOF
'semantics: return 1;
.end
""")
        self.generate("--check", "--module-path", self.work / "missing",
                      "--module-path", self.work / "first",
                      "--module-path", self.work / "second")
        self.write("Lib/Words.llsynth", ".wrong\n")
        result = self.generate("--check", "--module-path", self.work / "first", code=1)
        self.assertIn("requires .module Name", result.stderr)

    def test_missing_and_cycle_diagnostics_point_to_import_site(self):
        self.write("main.llsynth", '# comment\n.import "missing.llsynth"\n')
        result = self.generate("--check", "--diagnostics=json", code=1)
        diag = json.loads(result.stderr)
        self.assertEqual(diag["location"]["line"], 2)
        self.assertIn("module 'missing.llsynth' not found", diag["message"])
        self.write("main.llsynth", '.import "a.llsynth"\n')
        self.write("a.llsynth", '.module A\n.import "b.llsynth"\n')
        self.write("b.llsynth", '.module B\n.import "./a.llsynth"\n')
        result = self.generate("--check", "--diagnostics=json", code=1)
        diag = json.loads(result.stderr)
        self.assertEqual(diag["location"]["file"], str(self.work / "b.llsynth"))
        self.assertEqual(diag["location"]["line"], 2)
        self.assertIn("module import cycle:", diag["message"])
        self.assertIn("a.llsynth", diag["message"])
        self.assertIn("b.llsynth", diag["message"])

    def test_invalid_module_contracts(self):
        self.basic()
        original = (self.work / "words.llsynth").read_text()
        cases = (
            (original.replace(".module Words\n", ""), "requires .module Name"),
            (".module Words\n.export absent\n", "does not name a local declaration"),
            (".module Words\n.export A, A\n", "duplicate export"),
            (".module Words\n.language \"Oops\"\n", "entry-specification configuration"),
            (".module Words\n.start Start\n", "entry-specification configuration"),
            (".module Words\n.option parser = lalr1\n", "entry-specification configuration"),
            (".module Words\n.pipeline P\n.end\n", "entry-specification configuration"),
            (".module Words\n.module Again\n", "must be the first directive"),
        )
        for text, message in cases:
            with self.subTest(message=message):
                self.write("words.llsynth", text)
                self.assertIn(message, self.generate("--check", code=1).stderr)

    def test_alias_conflicts_name_mismatch_and_duplicate_module_names(self):
        self.basic()
        source = self.root.read_text()
        self.write("other.llsynth", ".module Other\n")
        self.root.write_text(source + '.import "other.llsynth" as W\n')
        self.assertIn("already bound", self.generate("--check", code=1).stderr)
        self.write("Words.llsynth", ".module NotWords\n")
        self.root.write_text(source + ".import Words\n")
        self.assertIn("module name mismatch", self.generate("--check", code=1).stderr)
        self.write("copy.llsynth", ".module Words\n")
        self.root.write_text(source + '.import "copy.llsynth"\n')
        self.assertIn("declared by both", self.generate("--check", code=1).stderr)

    def test_include_remains_textual_not_import_once(self):
        self.write("fragment.inc", ".token T: Int = /[0-9]+/\n")
        self.write("main.llsynth", """.include "fragment.inc"
.include "fragment.inc"
.rule Start
'syntax: T EOF
'semantics: return 1;
.end
""")
        self.assertIn("duplicate tokens declaration", self.generate("--check", code=1).stderr)

    def test_unexported_sigil_and_ir_symbols(self):
        self.basic()
        source = self.root.read_text()
        for symbol, message in (("W__NUMBER", "does not export NUMBER"),
                                ("@W__missing", "does not export @missing"),
                                ("#W__missing", "does not export #missing"),
                                ("!W__missing", "does not export !missing")):
            with self.subTest(symbol=symbol):
                self.root.write_text(source.replace("return &W__value($n) + @W__count;",
                                                   f"return {symbol};"))
                self.assertIn(message, self.generate("--check", code=1).stderr)
        self.write("stack.llsynth", """.module Stack
.export Ops
.ir Ops
.op PUSH_F64(value: Float64): () -> (Float64)
.end
""")
        self.write("main.llsynth", """.import "stack.llsynth" as S
.token NUMBER: Float64 = /[0-9]+/
.rule Start
'syntax: n:NUMBER EOF
'semantics: return $n;
'lower: return [S__Ops.PUSH_F64($n)];
.end
""")
        self.run_command("perl", self.driver, "--mode=both", "-o", "tool.py", self.root)
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="42").stdout, "42\n")

    def test_strings_comments_and_regexes_are_not_rewritten(self):
        self.write("words.llsynth", r""".module Words
.export Word
.token WORD = /helper/
.function helper(x: String) -> String
return "helper @count W__missing"; // helper
.end
.rule Word
'syntax: w:WORD
'semantics: return &helper($w);
.end
""")
        self.write("main.llsynth", """.import "words.llsynth" as W
.rule Start
'syntax: w:W__Word EOF
'semantics: return $w;
.end
""")
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="helper").stdout,
                         "helper @count W__missing\n")
        source = (self.work / "words.llsynth").read_text()
        self.write("words.llsynth", source.replace(
            '"helper @count W__missing"',
            '"helper\\nlet helper = 3;\\n@count W__missing"'.replace("\\n", "\n"),
        ))
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="helper").stdout,
                         "helper\nlet helper = 3;\n@count W__missing\n")

    def test_module_cannot_accidentally_bind_entry_symbols(self):
        self.write("words.llsynth", """.module Words
.export Word
.rule Word
'syntax: RootToken
'semantics: return &entry();
.end
""")
        self.write("main.llsynth", """.import "words.llsynth" as W
.token RootToken = /x/
.function entry() -> Int
return 1;
.end
.rule Start
'syntax: w:W__Word EOF
'semantics: return $w;
.end
""")
        self.assertNotEqual(self.generate("--check", code=1).returncode, 0)

    def test_functions_cannot_implicitly_bind_to_entry_or_other_modules(self):
        self.write("words.llsynth", """.module Words
.export value
.function value() -> Int
return entry();
.end
""")
        self.write("main.llsynth", """.import "words.llsynth" as W
.function entry() -> Int
return 99;
.end
.rule Start
'syntax: "x" EOF
'semantics: return &W__value();
.end
""")
        self.assertIn("undeclared variable 'entry'", self.generate("--check", code=1).stderr)
        # Root functions still resolve correctly when specifications import modules.
        self.write("words.llsynth", """.module Words
.export value
.function value() -> Int
return 1;
.end
""")
        source = self.root.read_text().replace("return &W__value();", "return entry();")
        self.root.write_text(source)
        self.generate("-o", "tool.py")
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="x").stdout, "99\n")

    def test_types_tables_exceptions_and_initializers(self):
        self.write("lib.llsynth", """.module Lib
.export %Value, @count, #symbols, !Bad, get
.type %Value = Int
.state @count: %Value = 0
.symtbl #symbols: String -> %Value
.exception !Bad(reason: String) "bad {reason}"
.init
@count = @count + 1;
#symbols["answer"] = 41;
.end
.function get() -> %Value
grab "answer" from #symbols into %Value(result);
return result;
.end
""")
        self.write("main.llsynth", """.import "lib.llsynth" as L
.function value() -> %L__Value
return &L__get();
.end
.rule Start
'syntax: "ok" EOF
'semantics:
require @L__count == 1 else Throw(!L__Bad, "count");
return value() + #L__symbols["answer"];
.end
""")
        for target, filename in (("python3", "tool.py"), ("c99", "tool.c")):
            self.generate("-T", target, "-o", filename)
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="ok").stdout, "82\n")
        if shutil.which("cc"):
            self.run_command("cc", "-std=c99", "tool.c", "-lm", "-o", "tool")
            self.assertEqual(self.run_command(self.work / "tool", stdin="ok").stdout, "82\n")

    def test_module_ir_types_backend_and_rewrites(self):
        self.write("stack.llsynth", """.module Stack
.export Ops, make
.ir Ops
.op VALUE(value: Float64): () -> (Float64)
.end
.function make(x: Float64) -> Ops.Op
return Ops.VALUE(x);
.end
.rewrite Identity: Ops
.match VALUE(x)
.replace VALUE(x)
.end
.backend python3 for Ops
.emit VALUE(x) => "${x}"
.end
""")
        self.write("main.llsynth", """.import "stack.llsynth" as S
.token NUMBER: Float64 = /[0-9]+/
.rule Start
'syntax: n:NUMBER EOF
'semantics: return $n;
'lower: return [&S__make($n)];
.end
""")
        self.run_command("perl", self.driver, "--mode=both", "--program-target=python3",
                         "-o", "tool.py", self.root)
        self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="42").stdout, "42\n")

    def test_symlink_imports_are_canonicalized(self):
        self.basic()
        (self.work / "alias.llsynth").symlink_to(self.work / "words.llsynth")
        with self.root.open("a") as out:
            out.write('.import "alias.llsynth" as Alias\n')
        self.generate("--check", "--dump-grammar")

    def test_exported_rule_can_be_explicit_entry(self):
        self.write("entry.llsynth", """.module Entry
.export Start
.rule Start
'syntax: "ok" EOF
'semantics: return 7;
.end
""")
        for start in ('"Entry__Start"', 'Entry->Start'):
            with self.subTest(start=start):
                self.write("main.llsynth", f'.import "entry.llsynth"\n.start {start}\n')
                self.generate("-o", "tool.py")
                self.assertEqual(self.run_command(sys.executable, "tool.py", stdin="ok").stdout, "7\n")

    def test_reserved_names_and_function_shadowing_have_diagnostics(self):
        self.basic()
        source = self.root.read_text()
        for replacement, message in (
            ("return LLSMod_private();", "reserved module linker name"),
            ("let W__value = 1; return W__value;", "conflicts with module alias"),
        ):
            self.root.write_text(source.replace("return &W__value($n) + @W__count;", replacement))
            self.assertIn(message, self.generate("--check", code=1).stderr)
        self.root.write_text(source)
        text = (self.work / "words.llsynth").read_text().replace(
            "return helper(x);", "let helper = 1; return helper;"
        )
        self.write("words.llsynth", text)
        self.assertIn("shadows a function declaration", self.generate("--check", code=1).stderr)
        self.write("words.llsynth", """.module Words
.export Number
.type %Secret = Int
.rule Number
'syntax: "x"
.end
""")
        prefix = "LLSMod_" + hashlib.sha256(b"module:Words").hexdigest()[:24] + "_"
        self.root.write_text(
            '.import "words.llsynth" as W\n'
            f".function leak(x: %{prefix}Secret) -> Int\nreturn x;\n.end\n"
            '.rule Start\n\'syntax: W__Number EOF\n\'semantics: return 1;\n.end\n'
        )
        self.assertIn("reserved module linker name", self.generate("--check", code=1).stderr)

    def test_module_precedence_and_compact_separated_repetition(self):
        self.write("expr.llsynth", """.module Expr
.export Value, PLUS
.token NUMBER: Int = /[0-9]+/
.literal PLUS = "+"
.rule Value
'syntax: NUMBER | Value PLUS Value
.end
""")
        self.write("main.llsynth", """.import "expr.llsynth" as E
.precedence left E__PLUS
.rule Start
'syntax: E__Value EOF
'semantics: return 1;
.end
""")
        self.generate("--check")
        self.write("expr.llsynth", """.module Expr
.export Value
.token NUMBER: Int = /[0-9]+/
.literal COMMA = ","
.rule Value
'syntax: NUMBER%COMMA
.end
""")
        self.root.write_text(self.root.read_text().replace(".precedence left E__PLUS\n", ""))
        self.generate("--check")

    def test_imported_inputs_cannot_be_overwritten(self):
        self.basic()
        before = (self.work / "words.llsynth").read_bytes()
        result = self.generate("-o", self.work / "words.llsynth", code=1)
        self.assertIn("overwrite", result.stderr)
        self.assertEqual((self.work / "words.llsynth").read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
