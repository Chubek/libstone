"""Run std.flowgraph on generated Python and C11 tools using real Wetstone."""

import ctypes
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


LLSYNTH = Path(__file__).resolve().parents[2]
ROOT = LLSYNTH.parents[1]
DRIVER = LLSYNTH / "scripts/llsynth.pl"
CC = shlex.split(os.environ.get("CC", "cc"))
CXX = shlex.split(os.environ.get("CXX", "c++"))


DIAMOND = """
let nodes: Vec<%FG->Block> = [
    %FG->Block([1, 2], [%FG->Step([9], [7])]),
    %FG->Block([3], [%FG->Step([7], [4, 7]), %FG->Step([4], [4])]),
    %FG->Block([3], [%FG->Step([7], [4]), %FG->Step([], [4, 6])]),
    %FG->Block([], [%FG->Step([12, 7, 4, 7], [])]),
    %FG->Block([3], [%FG->Step([777], [4])])
];
let lives = FG:liveness(nodes, 0, 100);
require lives == [
    %FG->Live(true, [9, 12], [7, 12]),
    %FG->Live(true, [7, 12], [4, 7, 12]),
    %FG->Live(true, [7, 12], [4, 7, 12]),
    %FG->Live(true, [4, 7, 12], []),
    %FG->Live(false, [], [])
] else Throw(!Mismatch, "diamond liveness");
let reaches = FG:reachingDefinitions(nodes, 0, 100);
let entryDef = %FG->Definition(0, 0, 7);
let leftDefs = [%FG->Definition(1, 0, 7), %FG->Definition(1, 1, 4)];
let rightDefs = [entryDef, %FG->Definition(2, 1, 4), %FG->Definition(2, 1, 6)];
let joined = [entryDef, leftDefs[0], leftDefs[1], rightDefs[1], rightDefs[2]];
require reaches == [
    %FG->Reaching(true, [], [entryDef]),
    %FG->Reaching(true, [entryDef], leftDefs),
    %FG->Reaching(true, [entryDef], rightDefs),
    %FG->Reaching(true, joined, joined),
    %FG->Reaching(false, [], [])
] else Throw(!Mismatch, "diamond reaching definitions");
return 42;
"""

LOOP = """
let nodes: Vec<%FG->Block> = [
    %FG->Block([], []),
    %FG->Block([2], []),
    %FG->Block([1, 3], [%FG->Step([4294967294], [4294967294, 4294967294])]),
    %FG->Block([], [%FG->Step([4294967294], [])])
];
let lives = FG:liveness(nodes, 1, 100);
let live = [4294967294];
require lives[1] == %FG->Live(true, live, live) && lives[2] == %FG->Live(true, live, live)
    && lives[3] == %FG->Live(true, live, []) else Throw(!Mismatch, "loop liveness");
let reaches = FG:reachingDefinitions(nodes, 1, 100);
let def = [%FG->Definition(2, 0, 4294967294)];
require reaches[0] == %FG->Reaching(false, [], [])
    && reaches[1] == %FG->Reaching(true, def, def)
    && reaches[2] == %FG->Reaching(true, def, def)
    && reaches[3] == %FG->Reaching(true, def, def)
    else Throw(!Mismatch, "loop reaching definitions");
return 42;
"""


@unittest.skipUnless(shutil.which("cmake") and CXX and shutil.which(CXX[0]),
                     "flowgraph requires CMake and a C++20 compiler")
class FlowgraphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        temp = tempfile.TemporaryDirectory(prefix=".flowgraph-test-", dir=ROOT)
        cls.addClassCleanup(temp.cleanup)
        cls.workspace = Path(temp.name)
        cls.build = cls.workspace / "native"
        cls.env = dict(os.environ, TMPDIR=str(cls.workspace))
        current = cls.env.get("LD_LIBRARY_PATH")
        cls.env["LD_LIBRARY_PATH"] = str(cls.build) + (os.pathsep + current if current else "")
        cls.run_cmd("cmake", "-S", LLSYNTH / "stdlib/flowgraph", "-B", cls.build)
        cls.run_cmd("cmake", "--build", cls.build)

    @classmethod
    def run_cmd(cls, *args, env=None, success=True):
        command = list(map(str, args))
        result = subprocess.run(command, input="", text=True, capture_output=True,
                                cwd=cls.workspace, env=cls.env if env is None else env, timeout=60)
        if success and result.returncode != 0:
            raise AssertionError(f"{shlex.join(command)}\n{result.stdout}{result.stderr}")
        if not success and result.returncode == 0:
            raise AssertionError(f"{shlex.join(command)} unexpectedly succeeded\n{result.stdout}")
        return result

    def generate(self, action, target, extra=()):
        spec = self.workspace / "tool.llsynth"
        spec.write_text(
            '.language "Flowgraph"\n.import std.flowgraph as FG\n'
            '.exception !Mismatch(part: String) "unexpected {part}"\n'
            ".rule Start\n'syntax: EOF\n'semantics:\n" + action + "\n.end\n",
            encoding="utf-8")
        output = self.workspace / ("tool.py" if target == "python3" else "tool.c")
        self.run_cmd("perl", DRIVER, "--mode=interpret", "-T", target, "-o", output,
                     *extra, spec)
        return output

    def assert_actions(self, tool, action, expected=None):
        result = self.run_cmd(*tool, success=expected is None)
        if expected is None:
            self.assertEqual(result.stdout.strip(), "42")
        else:
            self.assertIn(expected, result.stderr, action)

    def test_python_diamond_and_loop(self):
        for action in (DIAMOND, LOOP):
            with self.subTest(action=action):
                py = self.generate(action, "python3")
                self.assert_actions([sys.executable, py], action)

    @unittest.skipUnless(CC and shutil.which(CC[0]), "C compiler unavailable")
    def test_c_targets(self):
        for target, action in (("c11", DIAMOND), ("c11", LOOP), ("c99", DIAMOND)):
            with self.subTest(target=target, action=action):
                generated = self.generate(action, target)
                binary = self.workspace / "flowgraph"
                self.run_cmd(*CC, f"-std={target}", "-pedantic-errors", generated, "-L",
                             self.build, "-lllsynth_flowgraph", "-lm", "-o", binary)
                self.assert_actions([binary], action)

    def test_native_packet_limits(self):
        lib = ctypes.CDLL(str(self.build / "libllsynth_flowgraph.so"))
        analyze = lib.llsynth_flowgraph_v1_analyze
        analyze.argtypes = (ctypes.c_char_p, ctypes.c_int64, ctypes.c_int64)
        analyze.restype = ctypes.c_char_p

        def packet(words):
            return "".join(f"{word:08x}" for word in words).encode("ascii")

        self.assertEqual(analyze(b"0123", 0, 10), b"truncated flowgraph request")
        self.assertEqual(analyze(b"z" * 8, 0, 10), b"invalid flowgraph word")
        self.assertEqual(analyze(packet([1, 0, 0, 0, 1]), 0, 10),
                         b"trailing data in flowgraph request")
        self.assertEqual(analyze(b"0" * (8 * 8193), 0, 10), b"flowgraph request is too large")
        # The self-edge feeds all definitions back, so they occur on both
        # sides of the block (each occurrence occupies three words).
        graph = packet([1, 0, 1, 0, 1, 0, 5500, *range(5500)])
        self.assertEqual(analyze(graph, 1, 10), b"flowgraph result is too large")

    def test_invalid_graphs_and_no_partial_results(self):
        cases = [
            ("[], 0, 10", "expected 1 to 512 blocks"),
            ("[%FG->Block([], [])], 1, 10", "entry block is outside"),
            ("[%FG->Block([], [])], 0, -1", "evaluation limit must be"),
            ("[%FG->Block([], [])], 0, 100001", "evaluation limit must be"),
            ("[%FG->Block([], [%FG->Step([-1], [])])], 0, 10", "IDs must be"),
            ("[%FG->Block([], [%FG->Step([], [4294967295])])], 0, 10", "IDs must be"),
            ("[%FG->Block([], []), %FG->Block([9], [])], 0, 10", "successor block is outside"),
            ("[%FG->Block([0, 0], [])], 0, 10", "duplicate flowgraph successor"),
            ("[%FG->Block([0, 1, 2], []), %FG->Block([], []), %FG->Block([], [])], 0, 10",
             "block has more than two successors"),
        ]
        for args, message in cases:
            with self.subTest(args=args):
                action = f"return len(FG:liveness({args}));"
                output = self.generate(action, "python3")
                self.assert_actions([sys.executable, output], action, f"invalid flowgraph: {message}")
        for analysis in ("liveness", "reachingDefinitions"):
            action = f"return len(FG:{analysis}([%FG->Block([], [])], 0, 0));"
            output = self.generate(action, "python3")
            self.assert_actions([sys.executable, output], action,
                                "incomplete flowgraph analysis: fixed-point evaluation limit exhausted")

        # Three initial visits are not enough: the latch must requeue the entry.
        action = LOOP.replace("FG:reachingDefinitions(nodes, 1, 100)",
                              "FG:reachingDefinitions(nodes, 1, 3)")
        output = self.generate(action, "python3")
        self.assert_actions([sys.executable, output], action, "fixed-point evaluation limit exhausted")

    def test_exact_budget_and_import_without_runtime_library(self):
        env = dict(self.env)
        env.pop("LD_LIBRARY_PATH")
        unused = self.generate("return 42;", "python3", ("--manifest", self.workspace / "flow.json"))
        self.assertEqual(self.run_cmd(sys.executable, unused, env=env).stdout.strip(), "42")
        self.assertIn("stdlib/flowgraph.llsynth",
                      (self.workspace / "flow.json").read_text(encoding="utf-8"))

        exact = self.generate("return FG:liveness([%FG->Block([], [])], 0, 1)[0].Reachable;",
                              "python3")
        self.assertEqual(self.run_cmd(sys.executable, exact).stdout.strip(), "true")


if __name__ == "__main__":
    unittest.main()
