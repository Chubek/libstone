"""End-to-end tests for nominal data, curried callables, Unit and symbol aliases."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


CLI = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"


def rule(action, syntax="EOF"):
    return f".rule Main\n'syntax: {syntax}\n'semantics: {action}\n.end\n"


DATA = """\
// Fields may refer forward to other nominal types.
.sumtype %Choice
  Full: %Pair // payload
  Empty: %Unit
.end
.prodtype %Pair
  X: %Int
  Y: %Int
.end
.alias %Choice %Opt
"""


class TypeTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="llsynth-types-")
        self.addCleanup(tmp.cleanup)
        self.dir = Path(tmp.name)

    def cmd(self, *args, stdin="", success=True):
        p = subprocess.run(args, cwd=self.dir, input=stdin, text=True,
                           capture_output=True, timeout=30)
        self.assertEqual(p.returncode == 0, success, (args, p.stdout, p.stderr))
        return p

    def build(self, src, *args, success=True):
        (self.dir / "input.llsynth").write_text(src, encoding="utf8")
        return self.cmd("perl", CLI, "--mode=interpret", *args, "input.llsynth", success=success)

    def run_targets(self, src, expected, stdin="", runtime_error=False):
        for target, compiler, std, suffix in [
            ("python3", None, None, "py"), ("c99", "cc", "c99", "c"),
            ("cpp17", "c++", "c++17", "cpp")
        ]:
            with self.subTest(target=target):
                self.build(src, "-T", target, "-o", "tool." + suffix)
                if compiler:
                    if not shutil.which(compiler):
                        continue
                    self.cmd(compiler, "-std=" + std, "tool." + suffix, "-lm", "-o", "tool")
                    argv = [self.dir / "tool"]
                else:
                    argv = [sys.executable, "tool.py"]
                result = self.cmd(*argv, stdin=stdin, success=not runtime_error)
                if runtime_error:
                    self.assertIn(expected, result.stderr)
                else:
                    self.assertEqual(result.stdout.strip(), expected)

    def test_module_and_alias_identity(self):
        (self.dir / "data.llsynth").write_text(
            ".module Data\n.export %Opt, %Pair, evaluate, @hits, #mem, !Bad, NUM\n" +
            DATA + """\
.alias original evaluate
.alias @count @hits
.alias #cache #mem
.alias !Fail !Bad
.alias NUMBER NUM
.alias %Int %Number
.token NUMBER: %Number = /[0-9]+/
.state @count: %Number = 0
.symtbl #cache: String -> %Number
.exception !Fail "empty"
.function original(x: %Opt) -> %Number
require %Choice.is_Full(x) else Throw(!Bad);
let p = %Choice.get_Full(x);
return %Pair.X(p) + %Pair.Y(p);
.end
""", encoding="utf8")
        self.run_targets(""".import "data.llsynth" as D
.alias %D__Opt %Result
.alias %D__Pair %Point
.alias D__evaluate value
.alias value value2
.alias Main Entry
.start Entry
""" + rule("""let x = %Result.Full(%Point($n, 2));
@D__hits = 1;
#D__mem["n"] = @D__hits;
require !%Result.is_Empty(x) else Throw(!D__Bad);
return value2(x) + #D__mem["n"];""", "n:D__NUM EOF"), "43", "40")

    def test_curried_unit_and_first_class_constructors(self):
        src = DATA + """\
.sumtype %Foobar
 A: %Int
 B: %Callable[%Int -> %Int -> %Unit]
.end
.state @counter: %Int = 0
.function count(x: %Int) -> %Callable[%Int -> %Unit]
 return fn(y: %Int) -> %Unit { @counter = x + y; return; };
.end
""" + rule("""let constructor = %Foobar.B;
let x = constructor(count);
let f = %Foobar.get_B(x);
let u: %Unit = f(20)(22);
require u == nil else Throw(!Bad);
return @counter;""")
        self.build(".exception !Bad \"not unit\"\n" + src, "-o", "tool.py")
        self.assertEqual(self.cmd(sys.executable, "tool.py").stdout.strip(), "42")

    def test_recursive_data_and_empty_unit(self):
        self.run_targets(""".sumtype %Tree
 Leaf: %Unit
 Branch: Vec<%Tree>
.end
""" + rule("""let leaf = %Tree.Leaf(%Unit());
let children = %Tree.get_Branch(%Tree.Branch([leaf]));
return children[0] == %Tree.Leaf(nil);"""), "true")

    def test_alias_metaprograms_ir_and_named_curried_native(self):
        src = """\
.ir Ops
.op LOAD(x: %Int): () -> (%Int)
.end
.alias Ops Code
.alias Code.LOAD load_one
.alias len size
.template doubled(n) = n + n
.alias doubled twice
.alias later apply
.function later(unused: %Unit) -> %Callable[%Int -> %Int]
    return &inc;
.end
.function inc(n: %Int) -> %Int
    return n + 1;
.end
""" + rule("""let xs: Vec<Code.Op> = [load_one(2)];
let f: %Callable[%Unit -> %Int -> %Int] = apply;
return twice(f(%Unit())(size(xs) + 19));""")
        self.run_targets(src, "42")

    def test_wrong_tag_fails_at_projection(self):
        self.run_targets(DATA + rule("return %Pair.X(%Opt.get_Full(%Opt.Empty(nil)));"),
                         "expected %Choice.Full, got %Choice.Empty", runtime_error=True)

    def test_dot_projects_nominal_fields_and_active_variants(self):
        self.run_targets(DATA + rule("""
let choice: %Opt = %Opt.Full(%Pair(40, 2));
return choice.Full.X + choice.Full.Y;"""), "42")
        self.run_targets(DATA + rule("""
let choice: Any = %Opt.Full(%Pair(40, 2));
let result: Int = choice.Full.X + choice.Full.Y;
return result;"""), "42")
        self.run_targets(DATA + rule("return %Opt.Empty(nil).Full.X;"),
                         "expected %Choice.Full, got %Choice.Empty", runtime_error=True)

    def test_member_alias_in_module(self):
        (self.dir / "lib.llsynth").write_text(".module Lib\n.export %New, %Read, make, wrap, unwrap\n" + DATA +
                                             ".alias %Point.X %Read\n.type %Point = %Pair\n"
                                             ".type %New = %Point\n.alias %New make\n"
                                             ".type %Legacy = %Opt\n.alias %Legacy.Full wrap\n"
                                             ".alias %Legacy.get_Full unwrap\n", encoding="utf8")
        self.run_targets('.import "lib.llsynth" as L\n.alias %L.New.Y y\n' +
                         rule("let f = L.wrap; let p = L.unwrap(f(L.make(40, 2))); "
                              "return %L.Read(p) + y(%L.New(0, 2));"), "42")

    def test_diagnostics(self):
        cases = [
            (".sumtype %Empty\n.end\n", "at least one variant"),
            (".prodtype %D\nX: %Int\nX: %String\n.end\n", "duplicate member"),
            (".sumtype %D\nX: %Unit\nis_X: %Int\n.end\n", "conflicting type operation"),
            (".prodtype %D\nX: %Missing\n.end\n", "unknown type"),
            (".type %F = %Callable[%Int]\n", "invalid callable"),
            (".alias %A %B\n.alias %B %A\n", "cyclic alias"),
            (".alias %Missing %B\n", "unknown or private alias source"),
            (".alias %Int @N\n", "must preserve"),
            (".alias %Pair %Choice\n", "conflicts with a declaration"),
        ]
        for declarations, msg in cases:
            with self.subTest(msg=msg):
                e = self.build(DATA + declarations + rule("return 0;"), "--check", success=False)
                self.assertIn(msg, e.stderr)
        for act, msg in [
            ("%Pair(1)", "expects 2 arguments"), ("%Pair.X(%Opt.Empty(nil))", "type mismatch"),
            ("%Opt.Empty(1)", "type mismatch"), ("%Pair.Z(%Pair(1, 2))", "unknown type operation"),
        ]:
            with self.subTest(action=act):
                self.assertIn(msg, self.build(DATA + rule(f"return {act};"), "--check", success=False).stderr)


if __name__ == "__main__":
    unittest.main()
