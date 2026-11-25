#!/usr/bin/env python3
"""In-process Python API regression tests; no Cephyr executable is needed."""

from __future__ import annotations

import ctypes
import gc
import json
import os
import re
import sys
import tempfile
import textwrap
import threading
import time
import traceback
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
from types import ModuleType
from unittest.mock import Mock, patch

import cephyr


class CephyrPythonLoaderTests(unittest.TestCase):
    def test_installed_layout_with_symlinked_wrapper(self) -> None:
        with tempfile.TemporaryDirectory(prefix="cephyr-loader-") as scratch:
            root = Path(scratch)
            libdir = root / "lib" / "multiarch"
            wrapper = libdir / "cephyr/python/cephyr/__init__.py"
            wrapper.parent.mkdir(parents=True)
            wrapper.symlink_to(Path(cephyr.__file__).resolve())
            wrapper.with_name("_sdk.py").symlink_to(Path(cephyr.__file__).with_name("_sdk.py").resolve())
            library = libdir / "libcephyr.so.1"
            library.touch()
            with (
                patch.object(cephyr, "_PACKAGE_FILE", wrapper),
                patch.dict(os.environ, clear=True),
                patch.object(cephyr._ct, "CDLL") as load,
            ):
                self.assertIs(cephyr._open_library(None), load.return_value)
                load.assert_called_once_with(str(library))

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
        temporary = tempfile.TemporaryDirectory(prefix="cephyr-python-")
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        config = cls.root / "Cephyr.toml"
        config.write_text(
            'target = "x86_64-linux-gnu"\nassembler_style = "as"\ncache_enabled = false\n', encoding="utf-8"
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

    def test_parse_c11(self) -> None:
        ast = self.compiler.parse_c11((*self.config_args, "-"),
                                     "int python_ast(void) { return 42; }\n")
        self.assertTrue(ast.ok, ast.diagnostics)
        document = json.loads(ast.output)
        self.assertEqual(next(iter(document)), "schema_version")
        self.assertEqual(document["schema_version"], 1)
        self.assertEqual(document["ast"]["items"][0]["kind"], "function_decl")
        assembly = self.compiler.compile(
            (*self.config_args, "-S", "-o", "-", "-x", "c11-ast-json", "-"), ast.output,
        )
        self.assertTrue(assembly.ok, assembly.diagnostics)
        self.assertIn(b"python_ast:", assembly.output)
        self.assertTrue(cephyr.parse_c11((*self.config_args, "-"), "int x;", format="sexp")
                        .output.startswith(b"(1 "))
        with self.assertRaises(ValueError):
            self.compiler.parse_c11(("-",), "int x;", format="xml")

    def test_emit_interface(self) -> None:
        events = []

        def desugar(data):
            events.append(data.stage)
            return "typedef unsigned long count_t;\nextern count_t python_interface;\n"

        def analyze(data):
            events.append(data.stage)
            return None

        result = self.compiler.emit_interface(
            (*self.config_args, "-"),
            "replace this input",
            sdk=(
                cephyr.Hook(cephyr.Stage.DESUGAR, "interface-desugar", desugar),
                cephyr.Hook(cephyr.Stage.ANALYZE, "interface-analyze", analyze),
            ),
        )
        self.assertTrue(result.ok, result.diagnostics)
        self.assertEqual(events, [cephyr.Stage.DESUGAR])
        self.assertRegex(result.output, rb"^\(interface\s+1\b")
        self.assertIn(b'"python_interface"', result.output)
        self.assertIn(b'"count_t"', result.output)
        plain = cephyr.emit_interface(
            (*self.config_args, "-"),
            b"typedef unsigned long count_t;\nextern count_t python_interface;\n",
        )
        self.assertTrue(plain.ok, plain.diagnostics)
        self.assertEqual(result.output, plain.output)

        invalid = self.compiler.emit_interface(
            (*self.config_args, "--diagnostics-format=sarif", "-"),
            '_Static_assert(0, "python interface assertion");\n',
        )
        self.assertEqual(invalid.status, cephyr.Status.COMPILE_ERROR)
        self.assertEqual(invalid.output, b"")
        self.assertTrue(any(
            "python interface assertion" in diagnostic["message"]["text"]
            for diagnostic in invalid.sarif()["runs"][0]["results"]
        ), invalid.diagnostics)

    def test_emit_mir(self) -> None:
        events = []

        def analyze(data):
            events.append(data.stage)
            return None

        args = (*self.config_args, "--mir-opt", "-")
        source = "unsigned long python_mir(unsigned long x) { return x * 1UL; }\n"
        mir = self.compiler.emit_mir(
            iter(args), source, sdk=(cephyr.Hook(cephyr.Stage.ANALYZE, "mir-analyze", analyze),)
        )
        self.assertTrue(mir.ok, mir.diagnostics)
        self.assertEqual(events, [cephyr.Stage.ANALYZE])
        self.assertTrue(mir.output.startswith(b"archon-mir 1\n"))
        self.assertIn(b"\nfunction python_mir ", mir.output)
        self.assertNotIn(b"\nmul ", mir.output)
        cli_options = self.compiler.compile(("--emit-mir", "-o", "-", *args), source)
        self.assertTrue(cli_options.ok, cli_options.diagnostics)
        self.assertEqual(cli_options.output, mir.output)

        path = self.root / "python-output.mir"
        written = cephyr.emit_mir((*args, "-o", path), source.encode())
        self.assertTrue(written.ok, written.diagnostics)
        self.assertEqual(written.output, b"")
        self.assertEqual(path.read_bytes(), mir.output)

        ast = self.compiler.parse_c11(args, source)
        self.assertTrue(ast.ok, ast.diagnostics)
        imported = self.compiler.emit_mir((*args[:-1], "-x", "c11-ast-json", "-"), ast.output)
        self.assertTrue(imported.ok, imported.diagnostics)
        self.assertEqual(imported.output, mir.output)

        invalid = self.compiler.emit_mir((*args, "another.c"), source)
        self.assertEqual(invalid.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"exactly one", invalid.diagnostics)
        self.assertEqual(invalid.output, b"")

    def test_native_artifact_capture(self) -> None:
        result = self.compiler.compile(
            (*self.config_args, "-c", "-o", "-", "-"),
            b"int cephyr_python_object(void) { return 3; }\n",
        )
        self.assertTrue(result.ok, result.diagnostics)
        self.assertGreaterEqual(len(result.output), 64)
        self.assertEqual(result.output[:7], b"\x7fELF\x02\x01\x01")
        self.assertEqual(result.output[16:20], b"\x01\0\x3e\0")  # ET_REL, EM_X86_64
        self.assertIn(b"cephyr_python_object\0", result.output)

    def test_bad_tool_artifact_is_not_published(self) -> None:
        tool = self.root / "assembler.py"
        tool.write_text(textwrap.dedent("""\
            import sys
            from pathlib import Path

            out = Path(sys.argv[sys.argv.index("-o") + 1])
            mode = sys.argv[1]
            if mode == "directory":
                out.mkdir()
            elif mode == "empty":
                out.touch()
            elif mode == "oversize":
                with out.open("wb") as fp:
                    fp.truncate(128 * 1024 * 1024 + 1)
            elif mode == "failed":
                out.write_bytes(b"partial output")
                sys.exit(1)
            """), encoding="utf-8")
        config = self.root / "bad-artifact.toml"
        for mode, diagnostic in (
            ("missing", b"did not produce a regular native artifact"),
            ("directory", b"did not produce a regular native artifact"),
            ("empty", b"cannot read native artifact"),
            ("oversize", b"captured native artifact exceeds 128 MiB"),
            ("failed", b"exited with status 1"),
        ):
            with self.subTest(mode=mode):
                argv = json.dumps([sys.executable, str(tool), mode], ensure_ascii=False)
                config.write_text(f"[tools]\nassembler = {argv}\n", encoding="utf-8")
                result = self.compiler.compile(
                    ("--config", config, "--target=x86_64-linux-gnu", "-c",
                     "-x", "assembler", "-", "-o", "-"),
                    b".text\n",
                )
                self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR, result.diagnostics)
                self.assertEqual(result.output, b"")
                self.assertIn(diagnostic, result.diagnostics)

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
        class MisleadingLength(bytes):
            def __len__(self) -> int:
                return 1

        result = self.compiler.compile(
            (*self.config_args, "-x", "c", "-", "-fsyntax-only"),
            MisleadingLength(b"int valid_prefix;\n\0int suffix;\n"),
        )
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"NUL byte", result.diagnostics)

    def test_argument_validation_uses_underlying_bytes(self) -> None:
        class MisleadingBytes(bytes):
            def __contains__(self, key) -> bool:
                return False

        class MisleadingString(str):
            def encode(self, *args) -> bytes:
                return b"--version"

        for argument in (MisleadingBytes(b"--version\0"), MisleadingString("--version\0")):
            with self.assertRaises(ValueError):
                self.compiler.compile([argument])

    def test_copies_output_by_size_and_destroys_result(self) -> None:
        data = ctypes.create_string_buffer(b"prefix\0suffix")

        def get_output(result, size) -> int:
            size._obj.value = len(data) - 1
            return ctypes.addressof(data)

        with (
            patch.object(self.compiler, "_output", get_output),
            patch.object(self.compiler, "_destroy", wraps=self.compiler._destroy) as destroy,
        ):
            self.assertEqual(self.compiler.compile(["--version"]).output, b"prefix\0suffix")
            destroy.assert_called_once()

        def fail_copy(result, size) -> int:
            size._obj.value = 1
            return 0

        with (
            patch.object(self.compiler, "_output", fail_copy),
            patch.object(self.compiler, "_destroy", wraps=self.compiler._destroy) as destroy,
            self.assertRaisesRegex(RuntimeError, "null buffer with a nonzero size"),
        ):
            self.compiler.compile(["--version"])
        destroy.assert_called_once()

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

    def test_python_runs_while_native_tool_is_active(self) -> None:
        helper = self.root / "assembler.py"
        entered = self.root / "host_tool_entered"
        proceed = self.root / "host_tool_proceed"
        helper.write_text(textwrap.dedent("""\
            import os
            import sys
            import time
            from pathlib import Path

            entered, proceed = map(Path, sys.argv[1:3])
            entered.touch()
            deadline = time.monotonic() + 10
            while not proceed.is_file():
                if time.monotonic() >= deadline:
                    raise SystemExit("host tool could not run concurrently with Python")
                time.sleep(0.01)
            os.execvp("as", ["as", *sys.argv[3:]])
            """), encoding="utf-8")
        config = self.root / "parallel.toml"
        command = json.dumps([sys.executable, str(helper), str(entered), str(proceed)],
                             ensure_ascii=False)
        config.write_text(
            'target = "x86_64-linux-gnu"\nassembler_style = "as"\ncache_enabled = false\n'
            f"[tools]\nassembler = {command}\n",
            encoding="utf-8",
        )
        with ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(
                self.compiler.compile,
                ("--config", config, "-c", "-o", "-", "-"),
                "int outside_the_gil(void) { return 19; }\n",
            )
            deadline = time.monotonic() + 30
            try:
                while not entered.is_file() and not future.done() and time.monotonic() < deadline:
                    time.sleep(0.01)
                if not entered.is_file():
                    self.fail(f"host tool was not invoked: {future.result(timeout=10)}")
                # This call must finish while a distinct request is still waiting for its tool.
                version = self.compiler.compile(["--version"])
                self.assertTrue(version.ok, version.diagnostics)
            finally:
                proceed.touch()
            result = future.result(timeout=30)
        self.assertTrue(result.ok, result.diagnostics)
        self.assertTrue(result.output.startswith(b"\x7fELF"))


class CephyrPythonSDKTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.compiler = cephyr.Compiler()

    def test_sdk_pipeline(self) -> None:
        owner = threading.get_ident()
        snapshots: list[cephyr.HookInput] = []

        def track(data: cephyr.HookInput) -> None:
            gc.collect()  # Registration owns all descriptors and callback pointers.
            self.assertEqual(threading.get_ident(), owner)
            self.assertEqual(data.input_name, b"<stdin>")
            self.assertEqual(data.target_triple, b"x86_64-linux-gnu")
            self.assertEqual(data.target_architecture, b"AMD64")
            self.assertEqual(data.target_abi, b"SysVAMD64")
            snapshots.append(data)

        def desugar(data: cephyr.HookInput) -> str:
            track(data)
            self.assertEqual(data.mir_schema_version, 0)
            self.assertEqual(data.text, b"answer is seven;\n")
            return "int answer(void) { return 7; }\n"

        def analyze(data: cephyr.HookInput) -> cephyr.HookResult:
            track(data)
            self.assertEqual(data.mir_schema_version, 1)
            self.assertIn(b"const 0 7\n", data.text)
            return cephyr.HookResult(diagnostics=(
                cephyr.Diagnostic(cephyr.Severity.WARNING, "sdk.python", "Seven is odd",
                                  "original.c", 1, 11),
            ))

        def rewrite(data: cephyr.HookInput) -> bytes:
            track(data)
            self.assertIn(b"const 0 7\n", data.text)
            return data.text.replace(b"const 0 7\n", b"const 0 8\n")

        def assembly(data: cephyr.HookInput) -> str:
            track(data)
            self.assertIn(b"const 0 8\n", data.text)
            self.assertTrue(data.position_independent)
            return "\t.text\n\t.globl answer\nanswer:\n\tmovl $8, %eax\n\tret\n"

        def not_applicable(data: cephyr.HookInput) -> None:
            self.fail(f"unexpected callback at {data.stage}")

        stage = cephyr.Stage
        # Registration order is per-stage, with unfiltered and exact matches.
        hooks = (
            cephyr.Hook(stage.ASSEMBLY, "backend", assembly, "x86_64-linux-gnu"),
            cephyr.Hook(stage.ANALYZE, "skip", not_applicable, "aarch64-linux-gnu"),
            cephyr.Hook(stage.REWRITE, "rewrite", rewrite, "x86_64-linux-gnu"),
            cephyr.Hook(stage.DESUGAR, "desugar", desugar),
            cephyr.Hook(stage.ANALYZE, "analyze", analyze),
        )
        result = cephyr.compile(
            ["--target=x86_64-linux-gnu", "-S", "-fPIC", "-o", "-",
             "--diagnostics-format=sarif", "-"],
            b"answer is seven;\n", sdk=iter(hooks),
        )
        self.assertTrue(result.ok, result.diagnostics)
        self.assertIn(b"movl $8", result.output)
        self.assertEqual([data.stage for data in snapshots], list(stage))
        diagnostic, = result.sarif()["runs"][0]["results"]
        self.assertEqual(diagnostic["ruleId"], "sdk.python")
        location, = diagnostic["locations"]
        self.assertEqual(location["physicalLocation"]["region"]["startColumn"], 11)
        # Retaining snapshots after compilation does not retain borrowed native data.
        self.assertTrue(self.compiler.compile(["--version"]).ok)
        self.assertIn(b"const 0 7\n", snapshots[1].text)
        self.assertIn(b"const 0 8\n", snapshots[-1].text)

    def test_blackstone_runs_between_sdk_rewrite_and_backend(self) -> None:
        def rewrite(data: cephyr.HookInput) -> bytes:
            # Introduce a multiply-by-one opportunity only after lowering.
            self.assertIn(b"\nmul ", data.text)
            mir, count = re.subn(rb"(?m)^(const \d+) 7$", rb"\1 1", data.text)
            self.assertEqual(count, 1)
            return mir

        def assembly(data: cephyr.HookInput) -> str:
            self.assertNotIn(b"\nmul ", data.text)
            self.assertRegex(data.text, rb"(?m)^load (\d+) \d+ 0\nret \1$")
            return ".text\n"

        triple = "x86_64-linux-gnu"
        result = self.compiler.compile(
            [f"--target={triple}", "-S", "-o", "-", "-"],
            "unsigned long scale(unsigned long x) { return x * 7UL; }",
            sdk=(cephyr.Hook(cephyr.Stage.REWRITE, "make-identity", rewrite),
                 cephyr.Hook(cephyr.Stage.ASSEMBLY, "observe-mir", assembly, triple)),
        )
        self.assertTrue(result.ok, result.diagnostics)

    def test_sdk_rejection_and_invalid_output(self) -> None:
        def run(callback):
            return self.compiler.compile(
                ["--target=x86_64-linux-gnu", "-E", "-P", "-"],
                "int answer;\n", sdk=[cephyr.Hook(cephyr.Stage.DESUGAR, "check", callback)],
            )

        result = run(lambda _: cephyr.HookResult(reject=True))
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"callback rejected the input", result.diagnostics)
        self.assertEqual(result.output, b"")

        result = run(lambda _: cephyr.HookResult(diagnostics=(
            cephyr.Diagnostic(cephyr.Severity.ERROR, "sdk.policy", "Not allowed"),
        )))
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"sdk.policy", result.diagnostics)

        result = run(lambda _: b"int valid_prefix;\n\0int suffix;\n")
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"invalid text output", result.diagnostics)
        self.assertTrue(run(lambda _: b"").ok)  # Empty is a real replacement.
        self.assertIn(b"answer", run(lambda _: None).output)
        with self.assertRaisesRegex(TypeError, "an SDK hook must return"):
            run(lambda _: False)

    def test_sdk_callback_exception_is_reraised_after_native_cleanup(self) -> None:
        class HookFailure(BaseException):
            pass

        expected = HookFailure("hook failed")

        def fail(data):
            raise expected

        hook = cephyr.Hook(cephyr.Stage.DESUGAR, "fail", fail)
        with patch.object(self.compiler, "_destroy", wraps=self.compiler._destroy) as destroy:
            try:
                self.compiler.compile(["-E", "-"], "ignored", sdk=[hook])
            except HookFailure as actual:
                self.assertIs(actual, expected)
                trace = traceback.extract_tb(actual.__traceback__)
                self.assertEqual(trace[-1].name, "fail")
                self.assertNotIn("invoke", [frame.name for frame in trace])
            else:
                self.fail("callback exception was not propagated")
            destroy.assert_called_once()
        self.assertTrue(self.compiler.compile(["-E", "-"], "int after_failure;\n").ok)

    def test_same_sdk_can_be_used_by_concurrent_requests(self) -> None:
        barrier = threading.Barrier(2, timeout=10)

        def desugar(data: cephyr.HookInput) -> bytes:
            barrier.wait()
            if data.text == b"fail":
                raise ValueError("request-local hook failure")
            return b"int parallel_sdk;\n"

        sdk = [cephyr.Hook(cephyr.Stage.DESUGAR, "parallel", desugar)]
        with ThreadPoolExecutor(max_workers=2) as pool:
            args = ("--target=x86_64-linux-gnu", "-E", "-P", "-")
            good = pool.submit(self.compiler.compile, args, b"success", sdk=sdk)
            failed = pool.submit(self.compiler.compile, args, b"fail", sdk=sdk)
            with self.assertRaisesRegex(ValueError, "request-local hook failure"):
                failed.result(timeout=15)
            result = good.result(timeout=15)
        self.assertTrue(result.ok, result.diagnostics)
        self.assertIn(b"parallel_sdk", result.output)

    def test_sdk_registration_uses_native_validation(self) -> None:
        hook = cephyr.Hook(cephyr.Stage.DESUGAR, "duplicate", lambda _: None)
        result = self.compiler.compile(["--version"], sdk=[hook, hook])
        self.assertEqual(result.status, cephyr.Status.INVALID_ARGUMENT)
        self.assertIn(b"hook name must be unique", result.diagnostics)

    def test_sdk_stage_output_rules(self) -> None:
        result = self.compiler.compile(
            ["--target=x86_64-linux-gnu", "-fsyntax-only", "-"], "int answer;\n",
            sdk=[cephyr.Hook(cephyr.Stage.ANALYZE, "cannot-replace", lambda data: data.text)],
        )
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"analysis hooks cannot replace text", result.diagnostics)

        result = self.compiler.compile(
            ["--target=x86_64-linux-gnu", "-S", "-o", "-", "-"],
            "int answer(void) { return 7; }\n",
            sdk=[cephyr.Hook(cephyr.Stage.ASSEMBLY, "missing-output", lambda _: None,
                             "x86_64-linux-gnu")],
        )
        self.assertEqual(result.status, cephyr.Status.COMPILE_ERROR)
        self.assertIn(b"did not submit assembly", result.diagnostics)


if __name__ == "__main__":
    unittest.main()
