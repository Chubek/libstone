"""Link-wrapper generation, argument transport and real toolchain smoke tests."""

import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import generate_ir, pipeline_stages
from archon.irgen import linker_script


ROOT = Path(__file__).resolve().parents[1]
TMP_ROOT = Path("/tmp/opencode")
LINUX_X86_64 = sys.platform == "linux" and platform.machine() == "x86_64"
PROGRAMS = {"gnu": "ld.bfd", "gold": "ld.gold", "mold": "mold", "lld": "ld.lld", "yld": "qobjld"}

_STUB = r"""#!/bin/sh
printf '%s\000' "$@" > "$ARCHON_TEST_ARGV"
while [ "$#" -gt 1 ]; do
    if [ "$1" = -o ]; then
        printf '%s\n' 'stub image' > "$2" || exit 99
        exit "${ARCHON_TEST_STATUS:-0}"
    fi
    shift
done
exit 99
"""

_ENTRY = """\
.text
.globl _start
_start:
    mov $60, %eax
    mov $43, %edi
    syscall
.section .note.GNU-stack,"",@progbits
"""

_LTO_ENTRY = """\
extern __attribute__((visibility("hidden"))) int answer(void);
void _start(void) {
    __asm__ volatile ("syscall" :: "a"(60L), "D"((long)answer()) : "rcx", "r11", "memory");
    __builtin_unreachable();
}
"""

_LTO_HELPER = '__attribute__((visibility("hidden"))) int answer(void) { return 42; }\n'


class IRGenLinkerScript(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(
            prefix="archon-linker-", dir=TMP_ROOT if TMP_ROOT.is_dir() else None,
        )
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.generated = self.root / "generated dir"
        self.generated.mkdir()
        self.log = self.root / "arguments"
        self.env = dict(os.environ, ARCHON_TEST_ARGV=str(self.log))

    def emit(self, **options):
        for name, source in linker_script.emit({"options": options}).items():
            (self.generated / name).write_text(source, encoding="utf-8")
        return self.generated / "Linker.sh"

    def stub(self, name):
        executable = self.root / name
        executable.write_text(_STUB, encoding="utf-8")
        executable.chmod(0o755)
        return executable

    def link(self, *args, **env):
        return subprocess.run(
            ["sh", self.generated / "Linker.sh", *args],
            cwd=self.root,
            env={**self.env, **env},
            capture_output=True,
            text=True,
            timeout=30,
            check=False,
        )

    def argv(self):
        return self.log.read_bytes().decode("utf-8").split("\0")[:-1]

    def require_tools(self, *names):
        missing = [name for name in names if shutil.which(name) is None]
        if missing:
            self.skipTest(f"required tool(s) unavailable: {', '.join(missing)}")

    def run_tool(self, *args):
        return subprocess.run(
            args, cwd=self.root, capture_output=True, text=True, timeout=30, check=True
        ).stdout

    def test_discovery_and_option_validation(self):
        self.assertIn(linker_script, pipeline_stages())
        archdb = compile_file(ROOT / "stdspec/riscv/rv32i.arch")
        files = generate_ir("gated", [archdb])
        self.assertEqual(files["Linker.sh"], linker_script.emit({})["Linker.sh"])
        self.assertNotIn("LTO.lua", files)
        for provider in PROGRAMS:
            for lto in (False, True):
                files = linker_script.emit({"options": {"linker": provider, "lto": lto}})
                self.assertIn("Linker.sh", files)
                self.assertEqual("LTO.lua" in files, provider == "yld" and lto)

        for options, message in (
            ({"linker": "wasm"}, "one of"),
            ({"linker": []}, "one of"),
            ({"lto": "true"}, "boolean"),
            ({"lto": 0}, "boolean"),
            ({"lto": None}, "boolean"),
            ({"linker_script": None}, "nonempty path"),
            ({"linker_script": ""}, "nonempty path"),
            ({"linker_script": "nul\0.ld"}, "without NULs"),
            ({"linker_script": "layout.lua"}, "requires|require"),
            ({"linker": "yld", "linker_script": "layout.LUA"}, "lowercase"),
            ({"linker_program": "-bad"}, "must not start"),
            ({"linker": "gnu", "lto": True, "linker_program": "ld"}, "lto_driver"),
            ({"linker": "lld", "lto_driver": "clang"}, "requires LTO"),
        ):
            with self.subTest(options=options), self.assertRaisesRegex(ArchonError, message):
                linker_script.emit({"options": options})

    def test_provider_defaults_use_exact_executables(self):
        bindir = self.root / "mock bin"
        bindir.mkdir()
        (self.root / "input.o").touch()
        path = str(bindir) + os.pathsep + self.env["PATH"]
        for provider, program in PROGRAMS.items():
            with self.subTest(provider=provider):
                self.stub(f"mock bin/{program}")
                self.emit(linker=provider)
                result = self.link("-o", "test.out", "input.o", PATH=path)
                self.assertEqual(result.returncode, 0, result.stderr)
                args = self.argv()
                self.assertEqual(args[-1], "input.o")
                if provider == "lld":
                    self.assertEqual(args[:2], ["--lto-O0", "--lto-CGO0"])
                else:
                    self.assertEqual(args[0], "-o")
                self.assertNotIn("--lto-script", args)
                (bindir / program).unlink()

        self.log.unlink()
        self.stub("mock bin/ld")
        self.emit(linker="gnu", linker_program="./not available/ld.bfd")
        result = self.link("-o", "test.out", "input.o", PATH=path)
        self.assertEqual(result.returncode, 127, result.stderr)
        self.assertIn("not found", result.stderr)
        self.assertFalse(self.log.exists())

    def test_quoted_paths_override_entry_and_separate_yld_policy(self):
        tool = self.stub("custom linker '$(touch UNEXPECTED)'")
        default = "default '$(touch UNEXPECTED)' *.lua"
        layout = "--layout with spaces.lua"
        inputs = ("-first object.o", "@response $(touch UNEXPECTED).a", "last'\nobject.o")
        for file in (default, layout):
            (self.root / file).write_text("touch UNEXPECTED\n", encoding="utf-8")
        for file in inputs:
            (self.root / file).touch()
        self.emit(linker="yld", lto=True, linker_program=f"./{tool.name}", linker_script=default)
        policy = self.generated / "LTO.lua"
        self.assertIn("return { gc_sections = true, keep = {} }", policy.read_text())

        # The configured layout is relative to the caller, not the generated directory.
        result = self.link("-o", "test.out", "--", *inputs)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.argv()[:4], ["--lto-script", str(policy), "-T", default])

        result = self.link(
            "-o", "-output image", "--script", layout, "--entry", "start's symbol", "--", *inputs
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        args = self.argv()
        self.assertEqual(
            args[:6], ["--lto-script", str(policy), "-T", "./" + layout, "--entry=start's symbol", "-o"]
        )
        self.assertEqual(args[7:], ["./" + inputs[0], "./" + inputs[1], inputs[2]])
        self.assertEqual((self.root / "-output image").read_text(), "stub image\n")
        self.assertFalse((self.root / "UNEXPECTED").exists())
        self.assertFalse(list(self.root.glob(".archon-link.*")))

    def test_runtime_rejects_malformed_arguments_without_running_tool(self):
        tool = self.stub("test-linker")
        self.emit(linker_program=str(tool))
        for args, message in (
            ((), "missing -o"),
            (("-o", "output"), "no input files"),
            (("-o", "output", "-o", "other"), "duplicate -o"),
            (("-o", "output", "--script"), "needs a layout path"),
            (("-o", "output", "--script", "layout.lua", "input.o"), ".lua require"),
            (("-o", "output", "--script", "layout.LuA", "input.o"), "lowercase"),
            (("-o", "output", "--bad"), "unknown option"),
            (("-o", "output", "missing.o"), "cannot read input"),
        ):
            with self.subTest(args=args):
                result = self.link(*args)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn(message, result.stderr)
        self.assertFalse(self.log.exists())

    def test_output_cannot_alias_sources_or_tools(self):
        tool = self.stub("test-linker")
        layout = self.root / "layout.lua"
        layout.write_bytes(b"-- layout\n")
        source = self.root / "input.o"
        source.write_bytes(b"input fixture")
        wrapper = self.emit(linker="yld", lto=True, linker_program=str(tool), linker_script="layout.lua")
        policy = self.generated / "LTO.lua"
        hardlink = self.root / "same input"
        hardlink.hardlink_to(source)
        symlink = self.root / "same layout"
        symlink.symlink_to(layout)
        for path, role in (
            (source, "input"), (hardlink, "input"), (layout, "layout script"),
            (symlink, "layout script"), (wrapper, "wrapper"), (policy, "LTO policy"),
            (tool, "selected tool"),
        ):
            with self.subTest(path=path):
                before = path.read_bytes()
                result = self.link("-o", path, "input.o")
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("output aliases " + role, result.stderr)
                self.assertEqual(path.read_bytes(), before)
        override = self.root / "other.lua"
        override.touch()
        result = self.link("-o", layout, "--script", override, "input.o")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("output aliases configured layout script", result.stderr)
        self.assertFalse(self.log.exists())

    def test_failed_tool_does_not_replace_output_or_leave_partial_image(self):
        tool = self.stub("broken-linker")
        self.emit(linker_program=str(tool))
        (self.root / "input.o").touch()
        output = self.root / "final image"
        output.write_bytes(b"previous binary")
        result = self.link("-o", output, "input.o", ARCHON_TEST_STATUS="19")
        self.assertEqual(result.returncode, 19, result.stderr)
        self.assertEqual(output.read_bytes(), b"previous binary")
        self.assertFalse(list(self.root.glob(".archon-link.*")))

    @unittest.skipUnless(LINUX_X86_64, "requires a Linux x86-64 host")
    def test_gcc_lto_driver_arguments(self):
        self.require_tools("gcc", "readelf")
        (self.root / "ir.c").write_text(_LTO_HELPER)
        ir = "gimple input,1.o"
        self.run_tool("gcc", "-flto", "-c", "ir.c", "-o", ir)
        (self.root / "native.o").touch()
        (self.root / "layout,2.ld").touch()
        tool = self.stub("custom gcc")
        plugin = self.root / "fake lto plugin.so"
        plugin.touch()
        tool.write_text(
            '#!/bin/sh\ncase $1 in\n'
            '    -print-file-name=liblto_plugin.so) printf "%s\\n" "$ARCHON_TEST_PLUGIN"; exit 0 ;;\n'
            '    -print-prog-name=lto-wrapper) printf "%s\\n" "$ARCHON_TEST_WRAPPER"; exit 0 ;;\n'
            '    -fuse-ld=*) if [ "$2" = -print-prog-name=ld ]; then exit 1; fi ;;\n'
            'esac\n' + _STUB.split("\n", 1)[1]
        )
        for provider, ld_name in (("gnu", "bfd"), ("gold", "gold"), ("mold", "mold")):
            with self.subTest(provider=provider):
                self.emit(linker=provider, lto=True, lto_driver="./custom gcc")
                result = self.link(
                    "-o", "out", "--script", "layout,2.ld", "--entry", "_start", ir, "native.o",
                    ARCHON_TEST_PLUGIN=str(plugin), ARCHON_TEST_WRAPPER=str(tool),
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                args = self.argv()
                self.assertEqual(
                    args[:13],
                    ["-Xlinker", "-T", "-Xlinker", "layout,2.ld", "-Xlinker", "--entry=_start",
                     f"-fuse-ld={ld_name}", "-O2", "-flto", "-fuse-linker-plugin",
                     "-nostdlib", "-static", "-no-pie"],
                )
                self.assertEqual(args[-4:], ["-Xlinker", ir, "-Xlinker", "native.o"])
        self.log.unlink()
        result = self.link("-o", "out", ir, ARCHON_TEST_PLUGIN="liblto_plugin.so")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("must provide a readable liblto_plugin.so", result.stderr)
        self.assertFalse(self.log.exists())

    @unittest.skipUnless(LINUX_X86_64, "requires a Linux x86-64 host")
    def test_native_objects_with_installed_linkers(self):
        self.require_tools("cc", "readelf")
        (self.root / "start.s").write_text(_ENTRY)
        (self.root / "other.s").write_text(".text\nnop\n")
        (self.root / "layout.ld").write_text(
            "ENTRY(_start)\nSECTIONS { . = 0x450000; .text : { *(.text*) } }\n"
        )
        self.run_tool("cc", "-c", "start.s", "-o", "start object.o")
        self.run_tool("cc", "-m32", "-c", "other.s", "-o", "other arch.o")
        for provider in ("gnu", "gold", "lld", "mold"):
            with self.subTest(provider=provider):
                self.require_tools(PROGRAMS[provider])
                # mold need not implement arbitrary GNU section scripts.
                if provider == "mold":
                    self.emit(linker=provider)
                else:
                    self.emit(linker=provider, linker_script="layout.ld")
                output = self.root / "program"
                result = self.link("-o", output, "start object.o")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(subprocess.run([output], timeout=10, check=False).returncode, 43)
                if provider != "mold":
                    # Gold may add a build-id note before the code.
                    self.assertRegex(self.run_tool("readelf", "-h", output), r"Entry point address:\s+0x450[0-9a-f]{3}\b")
                original = output.read_bytes()
                result = self.link("-o", output, "start object.o", "other arch.o")
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(output.read_bytes(), original)

    @unittest.skipUnless(LINUX_X86_64, "requires a Linux x86-64 host")
    def test_lto_cross_translation_unit_optimization(self):
        self.require_tools("nm")
        (self.root / "start.c").write_text(_LTO_ENTRY)
        (self.root / "answer.c").write_text(_LTO_HELPER)
        for provider, compiler, extra in (
            ("gnu", "gcc", ["-fno-fat-lto-objects"]),
            ("gold", "gcc", ["-fno-fat-lto-objects"]),
            ("lld", "clang", []),
        ):
            with self.subTest(provider=provider):
                self.require_tools(PROGRAMS[provider], compiler)
                if compiler == "gcc":
                    plugin = self.run_tool(compiler, "-print-file-name=liblto_plugin.so").strip()
                    if not Path(plugin).is_file():
                        self.skipTest("the GCC linker plugin is unavailable")
                for base in ("start", "answer"):
                    self.run_tool(
                        compiler, "-O2", "-flto", *extra, "-ffreestanding", "-c",
                        base + ".c", "-o", base + ".o"
                    )
                output = self.root / "optimized"
                self.emit(linker=provider, lto=True)
                result = self.link("-o", output, "--entry", "_start", "start.o", "answer.o")
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(subprocess.run([output], timeout=10, check=False).returncode, 42)
                self.assertNotIn("answer ", self.run_tool("nm", "-P", output))

                previous = output.read_bytes()
                self.run_tool(compiler, "-O2", "-ffreestanding", "-c", "start.c", "-o", "native.o")
                result = self.link("-o", output, "native.o")
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("LTO needs a standalone", result.stderr)
                self.assertEqual(output.read_bytes(), previous)

                self.emit(linker=provider)
                result = self.link("-o", output, "--entry", "_start", "start.o", "answer.o")
                if provider == "lld":
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn("answer ", self.run_tool("nm", "-P", output))
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("cannot use GCC GIMPLE", result.stderr)
                    self.assertEqual(output.read_bytes(), previous)


if __name__ == "__main__":
    unittest.main()
