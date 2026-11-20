#!/usr/bin/env python3
"""In-process Python API regression tests; no Cephyr executable is needed."""

from __future__ import annotations

import os
import sys
import tempfile
import textwrap
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import ModuleType
from unittest.mock import Mock, patch

import cephyr


class CephyrPythonLoaderTests(unittest.TestCase):
    def test_incompatible_cephyr_abi(self) -> None:
        library = Mock(_name="wrong-abi.so")
        library.cephyr_abi_version.return_value = cephyr.ABI_VERSION + 1
        with patch.object(cephyr, "_open_library", return_value=library):
            with self.assertRaisesRegex(RuntimeError, "ABI version"):
                cephyr.Compiler()
        library.cephyr_python_version.assert_not_called()
        library.cephyr_compile.assert_not_called()

    def test_incompatible_cpython_version(self) -> None:
        library = Mock(_name="wrong-python.so")
        library.cephyr_abi_version.return_value = cephyr.ABI_VERSION
        library.cephyr_python_version.return_value = 2 << 16
        with patch.object(cephyr, "_open_library", return_value=library):
            with self.assertRaisesRegex(RuntimeError, "built for CPython 2.0"):
                cephyr.Compiler()
        library.cephyr_compile.assert_not_called()

    def test_subinterpreter_is_rejected_before_loading(self) -> None:
        try:
            from concurrent import interpreters
        except ImportError:
            self.skipTest("the public subinterpreters module requires Python 3.14+")
        interpreter = interpreters.create()
        try:
            interpreter.exec(textwrap.dedent("""\
                import cephyr

                try:
                    cephyr.Compiler("/not-a-library")
                except RuntimeError as exc:
                    assert "main interpreter" in str(exc), str(exc)
                else:
                    raise AssertionError("Cephyr accepted a subinterpreter")
                """))
        finally:
            interpreter.close()


class CephyrPythonAPITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        scratch = Path("/tmp/opencode")
        temporary = tempfile.TemporaryDirectory(
            prefix="cephyr-python-", dir=scratch if scratch.is_dir() else None
        )
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        config = cls.root / "Cephyr.toml"
        config.write_text(
            'target = "x86_64-linux-gnu"\ncache_enabled = false\n', encoding="utf-8"
        )
        cls.config_args = ("--config", config)
        cls.compiler = cephyr.Compiler()

    def test_native_assembly(self) -> None:
        self.assertRegex(self.compiler.version, r"\d+\.\d+")
        result = self.compiler.compile(
            (*self.config_args, "-x", "c", "-", "-S", "-o", "-"),
            "int cephyr_python_add(int a, int b) { return a + b; }\n",
        )
        self.assertEqual(result.status, cephyr.Status.OK, result.diagnostics)
        self.assertTrue(result.ok)
        self.assertIsInstance(result.output, bytes)
        self.assertIsInstance(result.diagnostics, bytes)
        self.assertIn(b"cephyr_python_add:", result.output)

    def test_archon_does_not_replace_host_imports(self) -> None:
        host_archon = ModuleType("archon")
        before = list(sys.path)
        with patch.dict(sys.modules, {"archon": host_archon}):
            result = self.compiler.compile(
                (*self.config_args, "-x", "c", "-", "-S", "-o", "-"),
                "int host_imports(void) { return 1; }\n",
            )
            self.assertTrue(result.ok, result.diagnostics)
            self.assertIs(sys.modules["archon"], host_archon)
            self.assertEqual(sys.path, before)

    def test_structured_diagnostics_and_repeated_calls(self) -> None:
        args = (*self.config_args, b"-x", b"c", b"-", b"-fsyntax-only")
        failures = [
            self.compiler.compile(
                (*args, "--diagnostics-format=sarif"),
                b"int broken(void) {\n    return (3 + );\n}\n",
            )
            for _ in range(2)
        ]
        ok = self.compiler.compile(args, b"int valid(void) { return 7; }\n")
        self.assertTrue(ok.ok, ok.diagnostics)
        self.assertEqual(ok.output, b"")
        for failure in failures:
            self.assertEqual(failure.status, cephyr.Status.COMPILE_ERROR)
            self.assertFalse(failure.ok)
            self.assertEqual(failure.output, b"")
            document = failure.sarif()
            self.assertEqual(document["version"], "2.1.0")
            run = document["runs"][0]
            self.assertEqual(run["tool"]["driver"]["name"], "Cephyr")
            self.assertTrue(
                any(
                    diagnostic["level"] == "error"
                    and any(
                        location["physicalLocation"]["region"]["startLine"] == 2
                        for location in diagnostic.get("locations", ())
                    )
                    for diagnostic in run["results"]
                ),
                document,
            )

    def test_filesystem_paths_and_source_encoding(self) -> None:
        source_file = self.root / os.fsdecode(b"\xff.c")
        source = 'const char *msg = "café";\n'
        source_file.write_bytes(source.encode("utf-8"))
        args = (*self.config_args, "-E", "-P", "-o", "-")
        results = (
            self.compiler.compile((*args, "-x", "c", "-"), source),
            self.compiler.compile((*args, "-x", "c", "-"), source.encode("utf-8")),
            self.compiler.compile((*args, os.fsencode(source_file))),
            self.compiler.compile((*args, source_file)),
        )
        for result in results:
            self.assertTrue(result.ok, result.diagnostics)
            self.assertIn(b"caf\xc3\xa9", result.output)
            self.assertEqual(result.output, results[0].output)

    def test_argument_validation(self) -> None:
        with self.assertRaises(TypeError):
            self.compiler.compile("-fsyntax-only")
        with self.assertRaises(ValueError):
            self.compiler.compile([b"--help\0"])
        with self.assertRaises(ValueError):
            self.compiler.compile(["--help"], archon_root=b"archon\0")
        result = self.compiler.compile(["--help"], resource_root=b"")
        self.assertEqual(result.status, cephyr.Status.INVALID_ARGUMENT)
        self.assertEqual((result.output, result.diagnostics), (b"", b""))

    def test_stdin_length_preserves_nul(self) -> None:
        result = self.compiler.compile(
            (*self.config_args, "-x", "c", "-", "-fsyntax-only"),
            b"int valid_prefix;\n\0int suffix;\n",
        )
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"NUL byte", result.diagnostics)

    def test_explicit_library_overrides_environment(self) -> None:
        library = os.environ.get("CEPHYR_LIBRARY")
        if library is None:
            self.skipTest("no explicit test library in the environment")
        missing = self.root / "missing.so"
        with patch.dict(os.environ, CEPHYR_LIBRARY=str(missing)):
            with self.assertRaises(OSError):
                cephyr.Compiler()
            result = cephyr.compile(["--version"], library=os.fsencode(library))
        self.assertTrue(result.ok, result.diagnostics)
        self.assertIn(self.compiler.version.encode("utf-8"), result.output)

    def test_threaded_calls(self) -> None:
        # Each thread owns a wrapper; both share the native compiler.
        def syntax_check(i: int) -> cephyr.Result:
            return cephyr.compile(
                (*self.config_args, "-x", "c", "-", "-fsyntax-only"),
                f"int f{i}(void) {{ return {i}; }}\n",
            )

        with ThreadPoolExecutor(max_workers=2) as pool:
            for result in pool.map(syntax_check, range(4)):
                self.assertTrue(result.ok, result.diagnostics)


if __name__ == "__main__":
    unittest.main()
