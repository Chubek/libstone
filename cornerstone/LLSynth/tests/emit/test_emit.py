#!/usr/bin/env python3
"""Run with python3 tests/emit/test_emit.py from cornerstone/LLSynth.

Requires Perl, a C99 compiler (CC, default cc), and a C++17 compiler
(CXX, default c++).
"""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
DRIVER = HERE.parents[1] / "scripts" / "llsynth.pl"
SPEC = HERE / "number.llsynth"
PROGRAM = b"19 + 23\n"
CC = shlex.split(os.environ.get("CC", "cc"))
CXX = shlex.split(os.environ.get("CXX", "c++"))


def run(*args, cwd, data=None):
    result = subprocess.run(
        args, cwd=cwd, input=data, capture_output=True, timeout=60
    )
    if result.returncode:
        raise AssertionError(
            f"{shlex.join(map(str, args))} exited {result.returncode}\n"
            f"{result.stdout.decode(errors='replace')}"
            f"{result.stderr.decode(errors='replace')}"
        )
    return result.stdout


def generate(directory, *args):
    run("perl", DRIVER, "--program-target", "bytecode", *args, SPEC, cwd=directory)


class EmitterTests(unittest.TestCase):
    def test_cli_parity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            python, c, cpp = (root / name for name in ("python", "c", "cpp"))
            for directory in (python, c, cpp):
                directory.mkdir()

            generate(python, "-T", "python3", "--package", "calc")
            generate(c, "-T", "c99", "--split", "none")
            generate(cpp, "-T", "cpp17", "--split", "all")
            run(*CC, "-std=c99", "-pedantic-errors", "EmitNumber.c", "-lm",
                "-o", "calc", cwd=c)
            self.assertTrue((cpp / "EmitNumber_api.cpp").is_file())
            run(*CXX, "-std=c++17", "-pedantic-errors", *sorted(cpp.glob("*.cpp")),
                "-lm", "-o", "calc", cwd=cpp)

            python_tool = (sys.executable, "-m", "calc")
            tools = (
                (python, python_tool),
                (c, (c / "calc",)),
                (cpp, (cpp / "calc",)),
            )
            bytecode = run(*python_tool, "--compile", cwd=python, data=PROGRAM)
            self.assertTrue(bytecode.startswith(b"LLSBC\x01"))
            for directory, tool in tools:
                with self.subTest(backend=directory.name):
                    self.assertEqual(run(*tool, cwd=directory, data=PROGRAM), b"42\n")
                    self.assertEqual(
                        run(*tool, "--compile", cwd=directory, data=PROGRAM), bytecode
                    )
                    self.assertEqual(
                        run(*tool, "--run-bytecode", cwd=directory, data=bytecode), b"42\n"
                    )

    def test_c_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "parts").mkdir()
            generate(
                root, "-T", "c99", "--driver-mode", "library", "--split", "explicit",
                "--module-prefix", "Calc", "--header", "calc.h",
                "--runtime", "parts/runtime.c", "--table", "parts/spec.c",
            )
            run(*CC, "-std=c99", "-pedantic-errors", "-I.", HERE / "library.c",
                "Calc.c", "parts/runtime.c", "parts/spec.c", "-lm", "-o", "test",
                cwd=root)
            self.assertEqual(run(root / "test", cwd=root), b"")


if __name__ == "__main__":
    unittest.main()
