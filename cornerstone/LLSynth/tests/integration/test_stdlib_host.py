"""Check that the optional host and analysis libraries link together."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "scripts" / "llsynth.pl"
IMPORTS = ("ipc", "shell", "egraph", "assembler", "isa", "flowgraph")
NATIVE_LIBDIR = os.environ.get("LLSYNTH_NATIVE_LIBDIR")


class HostLibraryIntegration(unittest.TestCase):
    def test_combined_imports_without_runtime_calls(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            spec = work / "combined.llsynth"
            spec.write_text(
                '.language "Combined"\n'
                + "".join(f".import std.{module} as Module{i}\n"
                          for i, module in enumerate(IMPORTS))
                + ".rule Start\n'syntax: EOF\n'semantics: return 42;\n.end\n",
                encoding="utf-8",
            )
            generated = subprocess.run(
                [str(DRIVER), "--mode=interpret", "-T", "python3",
                 "--depfile", "combined.d", "-o", "combined.py", str(spec)],
                cwd=work, capture_output=True, text=True, timeout=60,
            )
            self.assertEqual(generated.returncode, 0, generated.stderr)
            depfile = (work / "combined.d").read_text(encoding="utf-8")
            for module in IMPORTS:
                self.assertIn(f"/stdlib/{module}.llsynth", depfile)
            # An unused runtime binding must not require its native library.
            result = subprocess.run(
                [sys.executable, "combined.py"], cwd=work, input="",
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "42")

    @unittest.skipUnless(NATIVE_LIBDIR, "set LLSYNTH_NATIVE_LIBDIR to the built libraries")
    def test_analysis_facet_uses_egraph_and_flowgraph(self):
        with tempfile.TemporaryDirectory() as temporary:
            work = Path(temporary)
            example = ROOT / "examples" / "analysis" / "simplify.llsynth"
            generated = subprocess.run(
                [str(DRIVER), "--mode=analyze", "-o", "simplify.py", str(example)],
                cwd=work, capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(generated.returncode, 0, generated.stderr)
            env = {**os.environ, "LLSYNTH_LIBRARY_PATH": str(Path(NATIVE_LIBDIR).resolve())}
            result = subprocess.run(
                [sys.executable, "simplify.py", "--analyze"], cwd=work,
                env=env, input="(add (add value 0) 0)\n",
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            report, status = result.stdout.splitlines()
            self.assertEqual(json.loads(report)["term"], "value")
            self.assertEqual(status, "analysis OK")


if __name__ == "__main__":
    unittest.main()
