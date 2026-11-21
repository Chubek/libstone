"""Exercise the bundled libraries through generated language tools."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


DRIVER = Path(__file__).resolve().parents[2] / "scripts" / "llsynth.pl"

# This specification imports every portable library. Its input is deliberately
# unsorted, with duplicates: the outputs check ordering, multiplicity and bounds.
PORTABLE = r'''.language "LibraryIntegration"
.import std.Functional as F
.import std.Collections as C
.import std.Sorting as O
.import std.Search as S
.import std.Sets as Set
.import std.Sequences as Seq
.import std.Statistics as Stats
.import std.Integer as I
.import std.Bytes as B
.import std.Graph as G
.import std.LinearAlgebra as LA
.import std.Polynomial as P
.import std.RunLength as R
.skip /[ \t\n]+/
.token INT: Int = /[0-9]+/
.exception !Mismatch(part: String) "unexpected {part}"

.function double(x: Int) -> Int
return x * 2;
.end
.function odd(x: Int) -> Bool
return I__modulo(x, 2) == 1;
.end
.function difference(a: Int, b: Int) -> Int
return a - b;
.end

.rule Start
'syntax: ns:INT+ EOF
'semantics:
let xs = $ns;
let ordered = O__sorted(xs);
require ordered == [1, 2, 3, 3] && xs == [3, 1, 3, 2] else Throw(!Mismatch, "sort");
require S__equalRange(ordered, 3) == [2, 4] && S__lowerBound(ordered, 4) == 4 else Throw(!Mismatch, "bounds");
let mapped = F__map(F__filter(xs, &odd), &double);
require mapped == [6, 2, 6] && F__fold(mapped, 30, &difference) == 16 else Throw(!Mismatch, "higher-order calls");
let part = C__slice(xs, -2, 2);
require C__concat(part, [9]) == [3, 1, 9] && C__slice(xs, 3, 1) == [] else Throw(!Mismatch, "slice");
require O__merge([1, 3], [2, 3]) == ordered else Throw(!Mismatch, "merge");
require Set__intersection(xs, [1, 3]) == part && Set__union(xs, [1, 4]) == [3, 1, 2, 4] else Throw(!Mismatch, "sets");
require Seq__differences(Seq__prefixSums(xs)) == xs && Seq__range(1, 3) == [1, 2] else Throw(!Mismatch, "sequences");
require Stats__mean(xs) == 2.25 && abs(Stats__variance(xs) - 0.6875) < 0.000001 else Throw(!Mismatch, "statistics");
require I__modulo(-7, 3) == 2 && I__ceilDiv(-7, 3) == -2 && I__gcd(54, 24) == 6 else Throw(!Mismatch, "integer arithmetic");
let raw = B__writeU32LE(4278190593);
require raw == [1, 2, 0, 255] && B__readU32LE(C__concat([77], raw), 1) == 4278190593 else Throw(!Mismatch, "field encoding");
let edges: Vec<Vec<Int>> = [[1, 2], [0], [], []];
require G__distances(edges, 0) == [0, 1, 1, -1] && G__reachable(edges, 1, 2) else Throw(!Mismatch, "BFS");
let matrix = [[1, 2, 3], [4, 5, 6]];
require LA__matvec(LA__transpose(matrix), [2, 3]) == [14, 19, 24] else Throw(!Mismatch, "matrix product");
require P__evaluate(P__derivative([7, 2, 3]), 4) == 26 else Throw(!Mismatch, "polynomial");
let runs = R__encode(ordered);
require runs == [1, 1, 2, 1, 3, 2] && R__decode(runs) == ordered else Throw(!Mismatch, "run lengths");
return len(xs);
.end
'''

PARSEC = r'''.language "Recognize"
.import std.Parsec as P
.token WORD = /[a-z]+/
.skip /[ \t\n]+/
.rule Start
'syntax: words:WORD* EOF
'semantics:
let a = P__token("a");
let ab = P__then(a, P__token("b"));
let ac = P__then(a, P__satisfy(fn(word: String) -> Bool { return word == "c"; }));
let pair: %P__Parser = P__choice(ab, ac);
let pairs = P__many(pair);
let more = P__many(pairs);
let parser = P__then(more, P__token("z"));
if P__complete(parser, $words) { return 1; }
return 0;
.end
'''


class LibraryTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.work = Path(temporary.name)

    def run_cmd(self, *command, text="", success=True):
        result = subprocess.run(command, cwd=self.work, input=text, text=True,
                                capture_output=True, timeout=40)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def generate(self, source, target="python3", success=True):
        (self.work / "spec.llsynth").write_text(source, encoding="utf-8")
        name = "tool.py" if target == "python3" else "tool.c"
        return self.run_cmd(DRIVER, "--mode=interpret", "-T", target,
                            "-o", name, "spec.llsynth", success=success)

    def test_portable_python(self):
        self.generate(PORTABLE)
        result = self.run_cmd(sys.executable, "tool.py", text="3 1 3 2")
        self.assertEqual(result.stdout.strip(), "4")

    def test_portable_c(self):
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler or not shutil.which(compiler[0]):
            self.skipTest("C compiler unavailable")
        self.generate(PORTABLE, "c11")
        self.run_cmd(*compiler, "-std=c11", "tool.c", "-lm", "-o", "tool")
        result = self.run_cmd(self.work / "tool", text="3 1 3 2")
        self.assertEqual(result.stdout.strip(), "4")

    def test_parsec_choice_backtracks_many_checks_progress(self):
        self.generate(PARSEC)
        for words, result in [("a b a c z", "1"), ("z", "1"), ("a z", "0"),
                              ("a c z z", "0"), ("", "0")]:
            with self.subTest(words=words):
                out = self.run_cmd(sys.executable, "tool.py", text=words)
                self.assertEqual(out.stdout.strip(), result)

    def test_invalid_data_reports_the_library_error(self):
        source = r'''.language "Decode"
.import std.RunLength as R
.token INT: Int = /-?[0-9]+/
.skip /[ \t\n]+/
.rule Start
'syntax: pairs:INT* EOF
'semantics: return len(R__decode($pairs));
.end
'''
        self.generate(source)
        self.assertEqual(self.run_cmd(sys.executable, "tool.py", text="").stdout.strip(), "0")
        for data in ["3 4 1", "5 0", "5 -1"]:
            with self.subTest(data=data):
                error = self.run_cmd(sys.executable, "tool.py", text=data, success=False)
                self.assertIn("expected value/count pairs", error.stderr)


if __name__ == "__main__":
    unittest.main()
