#!/usr/bin/env python3
"""End-to-end regression tests for the Cephyr compiler driver.

CTest should invoke this file with the Cephyr executable as its sole positional
argument.  The suite intentionally uses only Python's standard library.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest


FIXTURES = Path(__file__).resolve().parent / "fixtures" / "cli_regression"
RV64 = "--target=riscv64-linux-gnu"
AARCH64 = "--target=aarch64-linux-gnu"
ELF_PROGRAM = struct.Struct("<IIQQQQQQ")
ELF_SECTION = struct.Struct("<IIQQQQIIQQ")
ELF_SYMBOL = struct.Struct("<IBBHQQ")
ELF_RELA = struct.Struct("<QQq")
CEPHYR: Path | None = None
QEMU: str | None = None
QEMU_AARCH64: str | None = None


def temporary_directory(prefix: str) -> tempfile.TemporaryDirectory[str]:
    scratch = Path("/tmp/opencode")
    return tempfile.TemporaryDirectory(
        prefix=prefix, dir=scratch if scratch.is_dir() else None
    )


def toml_string(value: os.PathLike[str] | str) -> str:
    # A JSON string is also a TOML basic string, including its escaping.
    return json.dumps(os.fspath(value))


class CephyrCLIRegressionTests(unittest.TestCase):
    maxDiff = 4096

    @classmethod
    def setUpClass(cls) -> None:
        if CEPHYR is None:
            raise RuntimeError("the test entry point did not configure Cephyr")
        if not CEPHYR.is_file():
            raise RuntimeError(f"Cephyr executable does not exist: {CEPHYR}")

        temporary = temporary_directory("cephyr-cli-regression-")
        cls.addClassCleanup(temporary.cleanup)
        cls.root = Path(temporary.name)
        cls.cache_home = cls.root / "xdg-cache"
        cls.cache_home.mkdir()

    def setUp(self) -> None:
        name = self.id().rsplit(".", 1)[-1]
        self.work = self.root / name
        self.work.mkdir()

    def invoke(
        self,
        *arguments: os.PathLike[str] | str,
        ok: bool = True,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        input_bytes: bytes | None = None,
        timeout: int = 300,
    ) -> subprocess.CompletedProcess[bytes]:
        process_env = os.environ.copy()
        process_env.pop("CEPHYR_CONFIG", None)
        process_env.update(
            {
                "LC_ALL": "C",
                "XDG_CACHE_HOME": str(self.cache_home),
            }
        )
        if env:
            process_env.update(env)
        result = subprocess.run(
            [str(CEPHYR), *map(os.fspath, arguments)],
            cwd=cwd or self.work,
            env=process_env,
            input=input_bytes,
            capture_output=True,
            check=False,
            timeout=timeout,
        )
        output = (result.stdout + result.stderr).decode("utf-8", errors="replace")
        self.assertNotIn("Traceback (most recent call last)", output)
        self.assertNotIn("AddressSanitizer:DEADLYSIGNAL", output)
        if ok:
            self.assertEqual(
                result.returncode,
                0,
                f"{[str(CEPHYR), *map(os.fspath, arguments)]!r} failed:\n{output}",
            )
        else:
            self.assertGreater(
                result.returncode,
                0,
                f"{[str(CEPHYR), *map(os.fspath, arguments)]!r} did not report "
                f"a normal failure (status {result.returncode}):\n{output}",
            )
        return result

    def assert_elf(self, path: Path, expected_type: int, expected_machine: int = 243) -> bytes:
        data = path.read_bytes()
        self.assertGreaterEqual(len(data), 64)
        self.assertEqual(data[:7], b"\x7fELF\x02\x01\x01")
        kind, machine = struct.unpack_from("<HH", data, 16)
        self.assertEqual(kind, expected_type)
        self.assertEqual(machine, expected_machine)
        self.assertEqual(struct.unpack_from("<I", data, 48)[0], 0, "unexpected ELF flags")
        return data

    def elf_program_headers(self, data: bytes) -> list[tuple[int, ...]]:
        program_offset = struct.unpack_from("<Q", data, 32)[0]
        program_size, program_count = struct.unpack_from("<HH", data, 54)
        self.assertEqual(program_size, ELF_PROGRAM.size)
        self.assertGreater(program_count, 0)
        self.assertLessEqual(program_offset + program_size * program_count, len(data))
        return [
            ELF_PROGRAM.unpack_from(data, program_offset + index * program_size)
            for index in range(program_count)
        ]

    def assert_static_executable(self, path: Path, machine: int = 243) -> None:
        data = self.assert_elf(path, 2, machine)
        programs = self.elf_program_headers(data)
        program_types = {header[0] for header in programs}
        self.assertNotIn(2, program_types, "static output contains PT_DYNAMIC")
        self.assertNotIn(3, program_types, "static output contains PT_INTERP")
        entry = struct.unpack_from("<Q", data, 24)[0]
        self.assertTrue(
            any(
                kind == 1 and flags & 1 and start <= entry < start + file_size
                for kind, flags, _, start, _, file_size, _, _ in programs
            ),
            "static entry point is not in an executable, file-backed PT_LOAD",
        )
        self.assertTrue(os.access(path, os.X_OK))

    def elf_sections(self, data: bytes) -> list[tuple[int, ...]]:
        section_offset = struct.unpack_from("<Q", data, 40)[0]
        section_size, section_count = struct.unpack_from("<HH", data, 58)
        self.assertEqual(section_size, ELF_SECTION.size)
        self.assertGreater(section_count, 0)
        self.assertLessEqual(section_offset + section_size * section_count, len(data))
        return [
            ELF_SECTION.unpack_from(data, section_offset + index * section_size)
            for index in range(section_count)
        ]

    def elf_symbols(self, path: Path) -> list[tuple[str, int, int]]:
        data = self.assert_elf(path, 1)
        sections = self.elf_sections(data)
        result: list[tuple[str, int, int]] = []
        for section in sections:
            if section[1] != 2:  # SHT_SYMTAB
                continue
            offset, size, string_index, entry_size = (
                section[4],
                section[5],
                section[6],
                section[9],
            )
            self.assertEqual(entry_size, ELF_SYMBOL.size)
            self.assertLess(string_index, len(sections))
            strings = sections[string_index]
            self.assertEqual(strings[1], 3)  # SHT_STRTAB
            string_data = data[strings[4] : strings[4] + strings[5]]
            for at in range(offset, offset + size, entry_size):
                name_offset, info, _, section_index, _, _ = ELF_SYMBOL.unpack_from(data, at)
                if not name_offset:
                    continue
                end = string_data.find(b"\0", name_offset)
                self.assertNotEqual(end, -1)
                name = string_data[name_offset:end].decode("utf-8")
                result.append((name, info >> 4, section_index))
        self.assertTrue(result, f"{path} has no static symbol table")
        return result

    def assert_pic_shared_object(self, path: Path) -> None:
        data = self.assert_elf(path, 3)
        programs = self.elf_program_headers(data)
        dynamic: tuple[int, ...] | None = None
        writable_ranges = []
        for header in programs:
            kind, flags = header[:2]
            if kind == 1:  # PT_LOAD
                self.assertNotEqual(
                    flags & 3,
                    3,
                    "shared object has a writable executable load segment",
                )
                if flags & 2:
                    writable_ranges.append((header[3], header[3] + header[6]))
            elif kind == 2:  # PT_DYNAMIC
                dynamic = header
        self.assertTrue(writable_ranges)
        self.assertIsNotNone(dynamic, "shared object has no PT_DYNAMIC")

        dynamic_tags: dict[int, list[int]] = {}
        assert dynamic is not None
        for at in range(dynamic[2], dynamic[2] + dynamic[5], 16):
            tag, value = struct.unpack_from("<qQ", data, at)
            if tag == 0:
                break
            dynamic_tags.setdefault(tag, []).append(value)
        self.assertNotIn(22, dynamic_tags, "shared object contains DT_TEXTREL")
        for flags in dynamic_tags.get(30, ()):  # DT_FLAGS / DF_TEXTREL
            self.assertFalse(flags & 4, "shared object sets DF_TEXTREL")

        # The address-taken global and its pointer require dynamic relocation.
        # Match the loader's table rather than accepting any SHT_RELA section.
        self.assertIn(7, dynamic_tags)  # DT_RELA
        (relocation_address,) = dynamic_tags[7]
        (relocation_size,) = dynamic_tags[8]  # DT_RELASZ
        self.assertEqual(dynamic_tags[9], [ELF_RELA.size])  # DT_RELAENT
        self.assertGreaterEqual(relocation_size, 2 * ELF_RELA.size)
        relocation_section = [
            section for section in self.elf_sections(data)
            if section[1] == 4 and section[3] == relocation_address  # SHT_RELA
        ]
        self.assertEqual(len(relocation_section), 1)
        offset = relocation_section[0][4]
        self.assertEqual(relocation_section[0][5], relocation_size)
        for target, _, _ in ELF_RELA.iter_unpack(data[offset : offset + relocation_size]):
            self.assertTrue(
                any(start <= target < end for start, end in writable_ranges),
                f"shared PIC relocation targets a non-writable address {target:#x}",
            )

    def run_rv64(self, executable: Path, expected_stdout: bytes = b"") -> None:
        if not QEMU:
            self.skipTest("validated RV64 ELF; execution requires qemu-riscv64")
        result = subprocess.run(
            [QEMU, str(executable)],
            cwd=self.work,
            capture_output=True,
            check=False,
            timeout=20,
        )
        self.assertEqual(
            result.returncode,
            0,
            f"{executable.name} exited with {result.returncode}:\n"
            f"{(result.stdout + result.stderr).decode('utf-8', errors='replace')}",
        )
        self.assertEqual(result.stdout, expected_stdout)

    def test_10_preprocessor_command_line_and_line_markers(self) -> None:
        output = self.work / "preprocessed.c"
        common = (
            "-E",
            "-std=c11",
            "-x",
            "c",
            "-include",
            FIXTURES / "include" / "cephyr_cli_forced.h",
            "-I",
            FIXTURES / "include",
            "-isystem",
            FIXTURES / "system",
            "-DCEPHYR_CLI_COMMAND_VALUE=5",
            "-DCEPHYR_CLI_REMOVE_ME=1",
            "-UCEPHYR_CLI_REMOVE_ME",
        )
        self.invoke(*common, "-P", FIXTURES / "cephyr_cli_preprocess.c", "-o", output)
        preprocessed = output.read_text(encoding="utf-8")
        self.assertRegex(
            preprocessed,
            r"(?s)int\s+cephyr_cli_preprocessed\s*=\s*"
            r"17\s*\+\s*13\s*\+\s*19\s*\+\s*5\s*;",
        )
        self.assertNotRegex(preprocessed, r"(?m)^\s*#\s*(?:line\s+)?\d+")
        self.assertNotIn("comment sentinel", preprocessed)
        for macro in (
            "CEPHYR_CLI_FORCED_VALUE",
            "CEPHYR_CLI_USER_VALUE",
            "CEPHYR_CLI_SYSTEM_VALUE",
            "CEPHYR_CLI_COMMAND_VALUE",
        ):
            self.assertNotIn(macro, preprocessed)

        marked = self.invoke(*common, FIXTURES / "cephyr_cli_preprocess.c").stdout.decode(
            "utf-8"
        )
        self.assertRegex(marked, r"(?m)^\s*#\s*(?:line\s+)?\d+")
        self.assertIn("cephyr_cli_preprocess.c", marked)

        comments = self.invoke(
            *common, "-C", "-P", FIXTURES / "cephyr_cli_preprocess.c"
        ).stdout
        self.assertIn(b"/* Cephyr CLI block comment sentinel. */", comments)
        self.assertIn(b"// Cephyr CLI line comment sentinel.", comments)

    def test_10b_preprocessing_character_values_follow_target(self) -> None:
        source = r"""
#if ('\377' == -1) != SIGNED_CHAR || ('\377' == 255) == SIGNED_CHAR
#error wrong plain char signedness
#endif
#if (L'\xffffffff' == -1) != SIGNED_WCHAR
#error wrong wchar_t sign extension
#elif (L'\xffffffff' == 4294967295U) == SIGNED_WCHAR
#error wrong wchar_t value
#elif (L'a' - L'b' < 0) != SIGNED_WCHAR
#error wrong wchar_t type
#endif
#if '12345' != 0x32333435 || '\377abc' != (0xff616263 - 0x100000000) || L'xy' != L'y'
#error wrong multicharacter conversion
#endif
#if 'é' != '\u00e9' || '\u00e9' != 0xc3a9 || u'\U0001f642' != 0xde42 || U'🙂' != 0x1f642
#error wrong execution character encoding
#endif
int target_characters_ok;
"""
        for target, signed_char, signed_wchar in (
            ("--target=x86_64-linux-gnu", 1, 1),
            (RV64, 0, 1),
            (AARCH64, 0, 0),
        ):
            with self.subTest(target=target):
                # The user macro environment does not change ABI properties.
                check = self.invoke(
                    target, "-E", "-P", "-x", "c",
                    f"-DSIGNED_CHAR={signed_char}", f"-DSIGNED_WCHAR={signed_wchar}",
                    "-D__CHAR_UNSIGNED__=1" if signed_char else "-U__CHAR_UNSIGNED__",
                    "-", input_bytes=source.encode("utf-8"),
                )
                self.assertIn(b"target_characters_ok", check.stdout)

    def test_11_config_preprocessing_and_target_override(self) -> None:
        shutil.copytree(FIXTURES / "include", self.work / "configured-include")
        shutil.copytree(FIXTURES / "system", self.work / "configured-system")
        config = self.work / "preprocess.toml"
        config.write_text(
            textwrap.dedent(
                """\
                target = "riscv64-linux-gnu"

                [preprocess]
                include_dirs = ["configured-include"]
                system_include_dirs = ["configured-system"]
                forced_includes = ["configured-include/cephyr_cli_forced.h"]
                defines = [
                  "CEPHYR_CLI_COMMAND_VALUE=7",
                  "CEPHYR_CLI_REMOVE_ME=1",
                ]
                undefines = ["CEPHYR_CLI_REMOVE_ME"]
                line_markers = false
                """
            ),
            encoding="utf-8",
        )
        result = self.invoke(
            "--config",
            config,
            "--target",
            "riscv64-linux-gnu",
            "-E",
            "-std=c11",
            FIXTURES / "cephyr_cli_preprocess.c",
        )
        self.assertRegex(
            result.stdout.decode("utf-8"),
            r"(?s)int\s+cephyr_cli_preprocessed\s*=\s*"
            r"17\s*\+\s*13\s*\+\s*19\s*\+\s*7\s*;",
        )
        self.assertNotRegex(
            result.stdout, rb"(?m)^\s*#\s*(?:line\s+)?\d+"
        )

        invalid = self.work / "invalid-target.toml"
        invalid.write_text('target = "i686-linux-gnu"\n', encoding="utf-8")
        destination = self.work / "invalid-target.o"
        failure = self.invoke(
            "--config",
            invalid,
            "-c",
            FIXTURES / "cephyr_cli_library.c",
            "-o",
            destination,
            ok=False,
        )
        self.assertFalse(destination.exists())
        self.assertIn(b"unsupported target", failure.stderr.lower())

    def test_11b_blackstone_default_and_opt_out(self) -> None:
        source = b"unsigned long f(unsigned long x) { return x * 1UL; }\n"
        args = ("--no-factir", "--emit-mir", "-x", "c", "-", "-o", "-")
        default = self.invoke(*args, input_bytes=source).stdout
        config = self.work / "Cephyr.toml"
        config.write_text("[blackstone]\nenabled = true\n", encoding="utf-8")
        self.assertEqual(default, self.invoke(*args, input_bytes=source).stdout)
        config.write_text("[blackstone]\nenabled = false\n", encoding="utf-8")
        original = self.invoke(*args, input_bytes=source).stdout
        multiply = re.search(rb"(?m)^mul (\d+) (\d+) \d+$", original)
        self.assertIsNotNone(multiply, original)
        self.assertNotRegex(default, rb"(?m)^mul ")
        self.assertNotRegex(default, rb"(?m)^(copy|zext) ")
        self.assertRegex(default, rb"(?m)^load (\d+) \d+ 0\nret \1$")

    def test_11c_blackstone_config_validation(self) -> None:
        invalid = self.work / "invalid.toml"
        for contents, diagnostic in (
            ("blackstone = false", b"'blackstone' must be a table"),
            ('[blackstone]\nenabled = "false"', b"'blackstone.enabled' must be a boolean"),
            ("[blackstone]\nenable = false", b"unknown configuration key 'blackstone.enable'"),
        ):
            with self.subTest(contents=contents):
                invalid.write_text(contents + "\n", encoding="utf-8")
                result = self.invoke("--config", invalid, "--emit-mir", "-o", "-",
                                     FIXTURES / "cephyr_cli_library.c", ok=False)
                self.assertIn(diagnostic, result.stderr)
                self.assertFalse(result.stdout)

    def test_12_custom_lua_target_selected_by_config_and_cli(self) -> None:
        bank = self.work / "custom-targets"
        bank.mkdir()
        triple = "riscv64-cephyr-cli-elf"
        (bank / f"{triple}.lua").write_text(
            textwrap.dedent(
                f"""\
                local family = "RISCV64"
                return {{
                    triple = "{triple}",
                    architecture = family .. "IM",
                    abi = "LP64",
                    os = "none",
                    specification = "@archon/stdspec/riscv/rv64im.arch",
                    assembler = {{ "unused-assembler" }},
                    linker = {{ "unused-linker" }},
                    archiver = {{ "unused-archiver" }},
                    assemble_flags = {{}},
                    link_flags = {{}},
                }}
                """
            ),
            encoding="utf-8",
        )
        config = self.work / "target.toml"
        config.write_text(
            'target = "riscv64-linux-gnu"\n[paths]\ntriple_dir = "custom-targets"\n',
            encoding="utf-8",
        )
        source = self.work / "target.c"
        source.write_text(
            "#if defined(__linux__) || __riscv_xlen != 64 || !defined(__LP64__)\n"
            "#error wrong target macros\n"
            "#endif\n"
            "int cephyr_cli_target_selected;\n",
            encoding="utf-8",
        )
        result = self.invoke("--config", config, "--target", triple, "-v", "-E", "-P", source)
        self.assertIn(f"{triple} (RISCV64IM/LP64)".encode(), result.stderr)
        self.assertIn(b"cephyr_cli_target_selected", result.stdout)

    def test_13_auto_target_policy_and_explicit_overrides(self) -> None:
        source = self.work / "target.c"
        source.write_text("int cephyr_cli_auto_target;\n", encoding="utf-8")
        # A tool's configured cross triple must not decide the host.
        marker = self.work / "assembler-invoked"
        tool = self.work / "cross-clang"
        tool.write_text(
            "#!/bin/sh\n"
            f"touch {toml_string(marker)}\n"
            "printf '%s\\n' riscv64-linux-gnu\n",
            encoding="utf-8",
        )
        tool.chmod(0o755)
        toml = self.work / "Cephyr.toml"
        cross_tool = (
            f'[tools]\nassembler = [{toml_string(tool)}, "--target=riscv64-linux-gnu"]\n'
        )
        toml.write_text(cross_tool, encoding="utf-8")
        default = self.invoke("-v", "-E", source)
        self.assertIn(b"x86_64-linux-gnu (AMD64/SysVAMD64)", default.stderr)
        self.assertNotIn(b"[auto]", default.stderr)

        toml.write_text('target = "riscv64-unknown-elf"\n' + cross_tool, encoding="utf-8")
        explicit = self.invoke("-v", "-E", source)
        self.assertIn(b"riscv64-unknown-elf (RISCV64IM/LP64)", explicit.stderr)

        toml.write_text('target = "auto"\n' + cross_tool, encoding="utf-8")
        override = self.invoke("--target=riscv64-unknown-elf", "-v", "-E", source)
        self.assertIn(b"riscv64-unknown-elf (RISCV64IM/LP64)", override.stderr)
        self.assertNotIn(b"[auto]", override.stderr)

        host = os.uname().machine.lower()
        native_auto = {
            "x86_64": b"x86_64-linux-gnu (AMD64/SysVAMD64)",
            "aarch64": b"aarch64-linux-gnu (AArch64/AAPCS64)",
        }
        host = {"amd64": "x86_64", "arm64": "aarch64"}.get(host, host)
        if host in native_auto and sys.maxsize > 2**32 and sys.byteorder == "little" and os.confstr("CS_GNU_LIBC_VERSION"):
            for args in ((), ("--target=auto",)):
                selected = self.invoke(*args, "-v", "-E", source)
                self.assertIn(native_auto[host] + b" [auto]", selected.stderr)
        elif host in native_auto:
            for args in ((), ("--target=auto",)):
                with self.subTest(args=args):
                    rejected = self.invoke(*args, "-v", "-E", source, ok=False)
                    self.assertIn(b"no Cephyr native C lowering target for host", rejected.stderr)
                    self.assertIn(b"--target=TRIPLE", rejected.stderr)
                    self.assertNotIn(b"riscv64-linux-gnu (RISCV64IM/LP64)", rejected.stderr)

        bank = FIXTURES.parents[2] / "triples"
        script = bank / "riscv64-unknown-elf.lua"
        paths = f'[paths]\ntriple_script = {toml_string(script)}\n'
        for target_config, args in (
            ("", ()),
            ('target = "auto"\n', ()),
            ('target = "riscv64-unknown-elf"\n', ("--target=auto",)),
            ('target = "riscv64-unknown-elf"\n', ("--target=riscv64-unknown-elf",)),
        ):
            with self.subTest(target_config=target_config, args=args):
                toml.write_text(target_config + paths + cross_tool, encoding="utf-8")
                selected = self.invoke(*args, "-v", "-E", source)
                self.assertIn(b"riscv64-unknown-elf (RISCV64IM/LP64)", selected.stderr)
                self.assertNotIn(b"[auto]", selected.stderr)

        new_target = self.invoke("--target=x86_64-linux-gnu", "-v", "-E", source)
        self.assertIn(b"x86_64-linux-gnu (AMD64/SysVAMD64)", new_target.stderr)

        toml.write_text('target = "riscv64-linux-gnu"\n' + paths, encoding="utf-8")
        mismatched = self.invoke("--target=auto", "-E", source, ok=False)
        self.assertIn(b"not requested triple 'riscv64-linux-gnu'", mismatched.stderr)
        self.assertFalse(marker.exists(), "target selection invoked a configured cross-assembler")

    def test_20_syntax_failure_is_atomic_and_sarif_is_valid(self) -> None:
        self.invoke(
            "-std=c11",
            "-fsyntax-only",
            FIXTURES / "cephyr_cli_semantics.c",
        )

        missing = self.work / "must-not-exist.o"
        failure = self.invoke(
            "-c", FIXTURES / "cephyr_cli_bad.c", "-o", missing, ok=False
        )
        self.assertFalse(missing.exists())
        self.assertTrue(failure.stderr)

        preserved = self.work / "preserved.o"
        preserved.write_bytes(b"previous output")
        self.invoke(
            "-c", FIXTURES / "cephyr_cli_bad.c", "-o", preserved, ok=False
        )
        self.assertEqual(preserved.read_bytes(), b"previous output")

        sarif_output = self.work / "sarif-must-not-exist.o"
        sarif_result = self.invoke(
            "--diagnostics-format=sarif",
            "-c",
            FIXTURES / "cephyr_cli_bad.c",
            "-o",
            sarif_output,
            ok=False,
        )
        self.assertFalse(sarif_output.exists())
        document = json.loads(sarif_result.stderr.decode("utf-8"))
        self.assertEqual(document["version"], "2.1.0")
        self.assertEqual(document["runs"][0]["tool"]["driver"]["name"], "Cephyr")
        self.assertTrue(
            any(
                result["level"] == "error"
                and any(
                    loc["physicalLocation"]["artifactLocation"]["uri"].endswith(
                        "/cephyr_cli_bad.c"
                    )
                    and loc["physicalLocation"]["region"]["startLine"] == 2
                    for loc in result.get("locations", ())
                )
                for result in document["runs"][0]["results"]
            ),
            document,
        )

    def test_21_cpp_output_preserves_diagnostic_source_mapping(self) -> None:
        result = self.invoke(
            "-x",
            "cpp-output",
            "-fsyntax-only",
            FIXTURES / "cephyr_cli_preprocessed.i",
            ok=False,
        )
        diagnostic = result.stderr.decode("utf-8", errors="replace")
        self.assertIn("cephyr-mapped-input.c", diagnostic)
        self.assertRegex(diagnostic, r"cephyr-mapped-input\.c:70[01]")

    def test_22_unsupported_constructs_do_not_publish_artifacts(self) -> None:
        cases = (
            ("complex", "double _Complex f(void) { return 1.5; }\n", rb"complex|_Complex"),
        )
        destination = self.work / "protected.mir"
        destination.write_bytes(b"previous output")
        for name, source, message in cases:
            with self.subTest(name=name):
                result = self.invoke(
                    AARCH64, "--emit-mir", "-x", "c", "-", "-o", destination,
                    input_bytes=source.encode(), ok=False,
                )
                self.assertRegex(result.stderr, message)
                self.assertEqual(destination.read_bytes(), b"previous output")

    def test_23_link_failure_is_atomic(self) -> None:
        source = self.work / "unresolved.S"
        source.write_text(
            ".text\n.globl _start\n_start:\n call cephyr_cli_missing_symbol\n",
            encoding="ascii",
        )
        destination = self.work / "link-result"
        for previous in (None, b"previous executable"):
            with self.subTest(existing_destination=previous is not None):
                if previous is not None:
                    destination.write_bytes(previous)
                result = self.invoke(
                    "-static", "-nostdlib", source, "-o", destination, ok=False
                )
                self.assertRegex(result.stderr, rb"undefined (?:symbol|reference)")
                self.assertIn(b"cephyr_cli_missing_symbol", result.stderr)
                if previous is None:
                    self.assertFalse(destination.exists())
                else:
                    self.assertEqual(destination.read_bytes(), previous)

    def test_24_static_assertions_and_char_type_identity(self) -> None:
        self.invoke(
            "-fsyntax-only", "-x", "c", "-",
            input_bytes=(
                b'_Static_assert(sizeof(unsigned long) == 8 && (1U << 31), "LP64");\n'
                b'_Static_assert(sizeof(+(unsigned char)1) == sizeof(int), "promotion");\n'
                b'int main(int n, char **args) {\n'
                b'  _Static_assert(_Alignof(int) == 4, "alignment");\n'
                b'  return n && args[0][0] == 0;\n'
                b'}\n'
            ),
        )
        invalid = self.invoke(
            "-fsyntax-only", "-x", "c", "-", ok=False,
            input_bytes=b'_Static_assert(sizeof(int) == 8, "int is not 64-bit");\n',
        )
        self.assertIn(b"static assertion failed: int is not 64-bit", invalid.stderr)
        incompatible = self.invoke(
            "-fsyntax-only", "-x", "c", "-", ok=False,
            input_bytes=b"void f(const char *); void f(const unsigned char *);\n",
        )
        self.assertIn(b"conflicting types", incompatible.stderr)
        self.invoke(
            "-fsyntax-only", "-x", "c", "-",
            input_bytes=(
                b"struct node;\n"
                b"struct node *identity(struct node *);\n"
                b"typedef struct node node;\n"
                b"node *identity(node *value) { return value; }\n"
            ),
        )
        shadowed = self.invoke(
            "-fsyntax-only", "-x", "c", "-", ok=False,
            input_bytes=(
                b"struct n;\n"
                b"void f(struct n *outer) { { struct n; struct n *inner = outer; } }\n"
            ),
        )
        self.assertIn(b"incompatible object-pointer conversion", shadowed.stderr)

    def test_30_emit_mir_is_explicit_and_does_not_delegate_user_c(self) -> None:
        poison = self.work / "poison-path"
        poison.mkdir()
        marker = self.work / "host-compiler-was-called"
        script = (
            "#!/bin/sh\n"
            f"printf called > {toml_string(marker)}\n"
            "exit 97\n"
        )
        for name in ("cc", "c++", "gcc", "g++", "clang", "clang++"):
            tool = poison / name
            tool.write_text(script, encoding="utf-8")
            tool.chmod(0o755)

        self.invoke(
            "-fsyntax-only", "-std=c11", FIXTURES / "cephyr_cli_semantics.c",
            env={"PATH": str(poison), "TMPDIR": str(self.work / "missing-tmpdir")},
        )
        self.assertFalse(marker.exists(), "syntax-only invoked a compiler tool")

        mir = self.work / "semantics.mir"
        self.invoke(
            "--target",
            "riscv64-linux-gnu",
            "--emit-mir",
            "-std=c11",
            FIXTURES / "cephyr_cli_semantics.c",
            "-o",
            mir,
            env={"PATH": str(poison)},
        )
        text = mir.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("archon-mir 1\n"), text[:200])
        self.assertRegex(text, r"(?m)^function\s+main\b")
        self.assertRegex(text, r"(?m)^global\s+\S*cephyr_cli_values\b")
        for operation in (
            "cbr", "ptr_add", "mul", "load", "store",
            "load_s8", "load_u8", "load_s16", "load_u16", "load_s32", "load_u32",
            "store8", "store16", "store32",
        ):
            self.assertRegex(text, rf"(?m)^{operation}\s")
        self.assertFalse(marker.exists(), "Cephyr delegated user C to a host compiler")

    def test_31_emit_mir_default_file_and_stdout(self) -> None:
        source = self.work / "mir_output.c"
        source.write_text("int cli_mir(void) { return 42; }\n", encoding="ascii")
        result = self.invoke("--emit-mir", source)
        self.assertEqual(result.stdout, b"")
        text = (self.work / "mir_output.mir").read_bytes()
        self.assertTrue(text.startswith(b"archon-mir 1\n"))
        self.assertIn(b"\nfunction cli_mir ", text)
        self.assertEqual(self.invoke("--emit-mir", "-o", "-", source).stdout, text)

    def test_40_packaged_tools_cold_cache(self) -> None:
        source = self.work / "packaged.c"
        source.write_text('#include <stdio.h>\nint main(void) { puts("packaged"); return 0; }\n')
        for triple, em in (("x86_64-linux-gnu", 62), ("aarch64-linux-gnu", 183), ("riscv64-linux-gnu", 243)):
            for stage in ("--emit-mir", "-S", "-c", "link"):
                with self.subTest(triple=triple, stage=stage):
                    output = self.work / (triple + stage)
                    env = {"PATH": "/nonexistent", "XDG_CACHE_HOME": str(self.work / (output.name + "-cache"))}
                    self.invoke("--target", triple, *(() if stage == "link" else (stage,)), source,
                                "-o", output, env=env)
                    data = output.read_bytes()
                    if stage == "--emit-mir": self.assertTrue(data.startswith(b"archon-mir"))
                    elif stage == "-S": self.assertIn(b"main:", data)
                    else:
                        self.assertEqual(data[:4], b"\x7fELF")
                        self.assertEqual(struct.unpack_from("<HH", data, 16), (1 if stage == "-c" else 2, em))
                    if stage == "link":
                        runner = {62: None, 183: QEMU_AARCH64, 243: QEMU}[em]
                        if runner or triple.startswith(os.uname().machine + "-"):
                            result = subprocess.run([*([runner] if runner else []), output],
                                                    check=True, capture_output=True, timeout=10)
                            self.assertEqual(result.stdout, b"packaged\n")

    def test_41_backend_checks_imported_spec_without_generator(self) -> None:
        root = self.work / "archon"
        spec = root / "stdspec" / "amd64"
        shutil.copytree(Path(__file__).resolve().parents[2] / "Archon/stdspec/amd64", spec)
        config = self.work / "target.toml"
        config.write_text(f'[paths]\narchon_root = {toml_string(root)}\n')
        args = ("--config", config, "--target=x86_64-linux-gnu", "-S", "-o", "-", "-")
        source = b"int verified(void) { return 42; }\n"
        env = {"PATH": "/nonexistent"}
        # The spec-only directory deliberately contains no Python modules.
        self.assertIn(b"verified:", self.invoke(*args, input_bytes=source, env=env).stdout)
        with (spec / "integer.arch").open("a") as out:
            out.write("\n// no longer the packaged source\n")
        self.assertIn(b"differs from packaged backend", self.invoke(
            *args, input_bytes=source, env=env, ok=False).stderr)

    def test_50_assembly_object_and_language_selection(self) -> None:
        assembly = self.work / "semantics.s"
        self.invoke(
            "-std=c11",
            "-S",
            FIXTURES / "cephyr_cli_semantics.c",
            "-o",
            assembly,
        )
        text = assembly.read_text(encoding="utf-8")
        self.assertIn("main:", text)
        self.assertNotIn("archon-mir 1", text)

        obj = self.work / "semantics.o"
        self.invoke("-std=c11", "-c", FIXTURES / "cephyr_cli_semantics.c", "-o", obj)
        self.assert_elf(obj, 1, 62)

        extensionless = self.work / "cephyr_cli_extensionless"
        extensionless.write_text(
            (FIXTURES / "cephyr_cli_library.c").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        self.invoke(
            "-x",
            "c",
            extensionless,
            "-x",
            "none",
            FIXTURES / "cephyr_cli_library.c",
            "-fsyntax-only",
        )

        plain_assembly = self.work / "cephyr_cli_plain_assembly"
        plain_assembly.write_text(
            ".text\n.globl cephyr_cli_asm_symbol\n"
            "cephyr_cli_asm_symbol:\n  ret\n",
            encoding="ascii",
        )
        asm_obj = self.work / "plain-assembly.o"
        self.invoke("-x", "assembler", "-c", plain_assembly, "-o", asm_obj)
        self.assert_elf(asm_obj, 1, 62)

    def test_51_binary_stdout_is_embedding_only(self) -> None:
        for options in (("-c",), ("-shared", "--stdlib=none", "-nostdlib")):
            with self.subTest(options=options):
                result = self.invoke(
                    *options, "-x", "c", "-", "-o", "-",
                    input_bytes=b"int answer(void) { return 42; }\n", ok=False,
                )
                self.assertEqual(result.stdout, b"")
                self.assertIn(b"binary output to stdout is not supported", result.stderr)

    def test_60_shared_none_and_packaged_shared_rejection(self) -> None:
        rejected = self.work / "packaged-libc.so"
        result = self.invoke(
            "-shared",
            FIXTURES / "cephyr_cli_library.c",
            "-o",
            rejected,
            ok=False,
        )
        self.assertFalse(rejected.exists())
        self.assertIn(b"shared", result.stderr.lower())

        shared = self.work / "freestanding.so"
        source = FIXTURES / "cephyr_cli_shared_pic.c"
        self.invoke(
            "--config", FIXTURES.parent / "riscv64-cross-dso.toml", RV64, "-shared",
            "--stdlib=none",
            "-nostdlib",
            "-ffreestanding",
            source,
            "-o",
            shared,
        )
        self.assert_pic_shared_object(shared)

        obj = self.work / "explicit-pic.o"
        self.invoke(RV64, "-fPIC", "-c", source, "-o", obj)
        self.assert_elf(obj, 1)
        explicit = self.work / "explicit-pic.so"
        self.invoke("--config", FIXTURES.parent / "riscv64-cross-dso.toml", RV64, "-shared", "--stdlib=none", obj, "-o", explicit)
        self.assert_pic_shared_object(explicit)

    def test_65_static_binding_across_translation_units(self) -> None:
        objects = []
        for side in ("left", "right"):
            source = FIXTURES / f"cephyr_cli_static_{side}.c"
            obj = self.work / f"{side}.o"
            self.invoke(RV64, "-std=c11", "-c", source, "-o", obj)
            symbols = self.elf_symbols(obj)
            for suffix in ("cephyr_cli_static_value", "cephyr_cli_static_step"):
                matched = [item for item in symbols if item[0].endswith(suffix)]
                self.assertEqual(len(matched), 1, symbols)
                self.assertEqual(matched[0][1], 0, f"{suffix} is not STB_LOCAL")
                self.assertGreater(matched[0][2], 0, f"{suffix} is not defined")
            self.assertTrue(
                any(
                    name == f"cephyr_cli_static_{side}" and bind == 1 and section
                    for name, bind, section in symbols
                ),
                symbols,
            )
            objects.append(obj)

        executable = self.work / "private-symbols.elf"
        self.invoke(
            "-static", "-nostdlib", "-ffreestanding",
            FIXTURES / "cephyr_cli_start.S",
            FIXTURES / "cephyr_cli_static_main.c",
            RV64, *objects, "-o", executable,
        )
        self.assert_static_executable(executable)
        self.run_rv64(executable)

    def test_70_freestanding_control_pointer_width_and_globals(self) -> None:
        executable = self.work / "semantics.elf"
        self.invoke(
            "--target",
            "riscv64-linux-gnu",
            "-std=c11",
            "-static",
            "-nostdlib",
            "-ffreestanding",
            "-DCEPHYR_CLI_EXIT_SYSCALL=93",
            "-x",
            "assembler-with-cpp",
            FIXTURES / "cephyr_cli_start.S",
            "-x",
            "none",
            FIXTURES / "cephyr_cli_semantics.c",
            "-o",
            executable,
        )
        self.assert_static_executable(executable)
        self.run_rv64(executable)

    def test_80_archive_library_multi_tu_and_libc_puts(self) -> None:
        archiver = shutil.which("ar") or shutil.which("llvm-ar")
        if not archiver:
            self.skipTest("archive integration requires ar or llvm-ar")

        library_object = self.work / "cephyr_cli_library.o"
        self.invoke(
            RV64, "-std=c11",
            "-c",
            FIXTURES / "cephyr_cli_library.c",
            "-o",
            library_object,
        )
        archive = self.work / "libcephyr_cli_regression.a"
        archived = subprocess.run(
            [archiver, "rcs", archive, library_object],
            cwd=self.work,
            capture_output=True,
            check=False,
            timeout=30,
        )
        self.assertEqual(
            archived.returncode,
            0,
            (archived.stdout + archived.stderr).decode("utf-8", errors="replace"),
        )

        executable = self.work / "puts.elf"
        self.invoke(
            RV64, "-std=c11",
            "-static",
            FIXTURES / "cephyr_cli_puts_main.c",
            "-L",
            self.work,
            "-lcephyr_cli_regression",
            "-o",
            executable,
        )
        self.assert_static_executable(executable)
        self.run_rv64(executable, b"cephyr multi-tu ok\n")

    def test_85_variadic_libc_across_registers_and_stack(self) -> None:
        source = FIXTURES.parents[2] / "stdlib" / "tests" / "variadic-cephyr.c"
        executable = self.work / "vararg.elf"
        self.invoke(RV64, source, "-o", executable)
        self.assert_static_executable(executable)
        self.run_rv64(executable, b"-7 65535 -9000000000 4 5 6 7 stack 12345678901234\n")

    def test_86_function_pointer_null_and_indirect_call(self) -> None:
        executable = self.work / "function-pointer.elf"
        self.invoke(
            RV64, "-o", executable, "-x", "c", "-",
            input_bytes=(
                b'typedef int (*unary)(int);\n'
                b'static int next(int x) { return x + 1; }\n'
                b'int main(void) {\n'
                b'  unary callback = (void *)0;\n'
                b'  if (callback != 0) return 1;\n'
                b'  callback = next;\n'
                b'  if (!callback || (*callback)(3) != 4) return 2;\n'
                b'  return 0;\n'
                b'}\n'
            ),
        )
        self.assert_static_executable(executable)
        self.run_rv64(executable, b"")

    def test_87_switch_fallthrough_and_nested_loops(self) -> None:
        executable = self.work / "switch.elf"
        self.invoke(RV64, FIXTURES / "switch_flow.c", "-o", executable)
        self.assert_static_executable(executable)
        self.run_rv64(executable, b"")
        invalid = self.invoke(
            "-fsyntax-only", "-x", "c", "-", ok=False,
            input_bytes=(
                b"int f(unsigned u) { switch (u) {\n"
                b"  case -1: return 0; case ~0U: return 1;\n"
                b"} return 2; }\n"
            ),
        )
        self.assertIn(b"duplicate case", invalid.stderr)

    def test_88_native_goto_across_c_scopes(self) -> None:
        self._check_native_c11_fixture("goto")

    def test_89_native_goto_constraints(self) -> None:
        output = self.work / "protected.mir"
        output.write_bytes(b"previous output")
        for source, diagnostic in (
            (b"int f(void) { return 0; goto nowhere; }\n", b"undefined label 'nowhere'"),
            (b"int f(void) { goto there; }\nint g(void) { there: return 0; }\n",
             b"undefined label 'there'"),
            (b"int f(void) { here: ; { here: return 0; } }\n", b"duplicate label 'here'"),
            (b"int f(int n) { goto there; { int a[n]; there: return 0; } }\n",
             b"goto enters the scope of a variable-length array"),
        ):
            with self.subTest(source=source):
                failure = self.invoke(
                    "--emit-mir", "-x", "c", "-", "-o", output,
                    input_bytes=source, ok=False,
                )
                self.assertIn(diagnostic, failure.stderr)
                self.assertEqual(output.read_bytes(), b"previous output")

    def _check_native_c11_fixture(self, feature: str) -> None:
        source = FIXTURES / f"cephyr_cli_native_{feature}.c"
        for target, machine, runner in (
            ("--target=x86_64-linux-gnu", 62, os.uname().machine in {"x86_64", "amd64"}),
            (AARCH64, 183, QEMU_AARCH64 or os.uname().machine in {"aarch64", "arm64"}),
            (RV64, 243, QEMU),
        ):
            with self.subTest(target=target):
                executable = self.work / f"{feature}-{machine}.elf"
                build = self.invoke(target, "-std=c11", source, "-o", executable)
                self.assertNotIn(b"control reaches the end", build.stderr)
                self.assert_static_executable(executable, machine)
                if runner and machine == 243:
                    self.run_rv64(executable)
                elif runner and machine == 183:
                    result = self.run_aarch64(executable)
                    self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
                elif runner:
                    result = subprocess.run(
                        [executable], cwd=self.work, capture_output=True, timeout=20,
                    )
                    self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))

    def test_90_default_amd64_bundled_runtime(self) -> None:
        source = FIXTURES / "linux_runtime.c"
        executable = self.work / "amd64.elf"
        self.invoke(source, "-o", executable)
        self.assert_static_executable(executable, 62)
        if os.uname().machine not in {"x86_64", "amd64"}:
            self.skipTest("validated AMD64 ELF; execution requires an x86-64 Linux host")
        result = subprocess.run(
            [executable, "argument"], env={"CEPHYR_RUNTIME_TEST": "ok"},
            cwd=self.work, capture_output=True, timeout=30,
        )
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (0, b"argument -7 2 3 4 5 6 7 8\n", b""))

    def test_91_amd64_sysv_shared_and_system_link(self) -> None:
        if os.uname().machine not in {"x86_64", "amd64"} or not shutil.which("cc"):
            self.skipTest("SysV ABI interoperability requires x86-64 Linux and cc")
        shared = self.work / "libabi.so"
        self.invoke("-shared", "--stdlib=none", FIXTURES / "scalar_abi_cephyr.c", "-o", shared)
        self.assert_elf(shared, 3, 62)
        native = self.work / "system.elf"
        result = subprocess.run(
            ["cc", FIXTURES / "scalar_abi_system.c", "-L", self.work, "-labi",
             "-Wl,-rpath,$ORIGIN", "-o", native], capture_output=True, timeout=30,
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
        subprocess.run([native], check=True, timeout=30)

        # Also exercise Cephyr's system-CRT/libc driver path on the same ABI.
        conf = self.work / "system.toml"
        conf.write_text('stdlib = "system"\nstatic_link = false\n[tools]\nlinker = ["cc"]\n')
        native_main = self.work / "main.c"
        native_main.write_text('int puts(const char *); int main(void) { puts("system libc"); return 0; }\n')
        dynamic = self.work / "dynamic.elf"
        self.invoke("--config", conf, native_main, "-o", dynamic)
        linked = subprocess.run([dynamic], capture_output=True, check=True, timeout=30)
        self.assertEqual(linked.stdout, b"system libc\n")

    def run_aarch64(self, executable: Path, *args: str, env=None) -> subprocess.CompletedProcess[bytes]:
        if os.uname().machine in {"aarch64", "arm64"} and sys.byteorder == "little":
            launcher = []
        elif QEMU_AARCH64:
            launcher = [QEMU_AARCH64]
        else:
            self.skipTest("validated AArch64 ELF; execution requires AArch64 Linux or qemu-aarch64")
        return subprocess.run(
            [*launcher, executable, *args], env=env, cwd=self.work, capture_output=True, timeout=30,
        )

    def test_92_aarch64_linux_bundled_runtime(self) -> None:
        executable = self.work / "runtime.elf"
        self.invoke(AARCH64, FIXTURES / "linux_runtime.c", "-o", executable)
        self.assert_static_executable(executable, 183)
        result = self.run_aarch64(executable, "argument", env={"CEPHYR_RUNTIME_TEST": "ok"})
        self.assertEqual((result.returncode, result.stdout, result.stderr),
                         (0, b"argument -7 2 3 4 5 6 7 8\n", b""))

    def test_93_aarch64_pic_and_aapcs64_interop(self) -> None:
        source = FIXTURES / "scalar_abi_cephyr.c"
        shared = self.work / "libabi.so"
        self.invoke("--config", FIXTURES.parent / "aarch64-cross-dso.toml", AARCH64, "-shared", "--stdlib=none", source, "-o", shared)
        self.assert_elf(shared, 3, 183)
        self.assertTrue(any(header[0] == 2 for header in self.elf_program_headers(shared.read_bytes())))

        obj = self.work / "scalar.o"
        self.invoke(AARCH64, "-fPIC", "-c", source, "-o", obj)
        self.assert_elf(obj, 1, 183)
        system_obj = self.work / "reference.o"
        compile_reference = subprocess.run(
            ["clang", "--target=aarch64-linux-gnu", "-O2", "-ffreestanding",
             "-fno-stack-protector", "-c", FIXTURES / "scalar_abi_system.c", "-o", system_obj],
            capture_output=True, timeout=30,
        )
        self.assertEqual(compile_reference.returncode, 0, compile_reference.stderr.decode(errors="replace"))
        executable = self.work / "scalar.elf"
        self.invoke(AARCH64, obj, system_obj, "-o", executable)
        self.assert_static_executable(executable, 183)
        result = self.run_aarch64(executable)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_94_native_multidimensional_array_initializers(self) -> None:
        self._check_native_c11_fixture("array_init")

    def test_95_excess_nested_initializers_are_diagnosed(self) -> None:
        for declaration in (
            b"int a[2][2] = {{1, 2, 3}, 4};",
            b"int a[][2] = {{1, 2, 3}};",
            b'char a[][2] = {"abc"};',
        ):
            for source in (declaration, b"int f(void) { " + declaration + b" return 0; }"):
                with self.subTest(source=source):
                    failure = self.invoke(
                        "-fsyntax-only", "-x", "c", "-", input_bytes=source, ok=False,
                    )
                    self.assertRegex(failure.stderr, rb"too many elements|too long")

    def test_96_native_block_static_duration(self) -> None:
        self._check_native_c11_fixture("block_static")
        obj = self.work / "block-static.o"
        self.invoke(RV64, "-c", FIXTURES / "cephyr_cli_native_block_static.c", "-o", obj)
        symbols = [
            (bind, section) for name, bind, section in self.elf_symbols(obj)
            if name.startswith("__cephyr_tu_block_")
        ]
        self.assertEqual(len(symbols), 7)
        self.assertTrue(all(bind == 0 and section for bind, section in symbols), symbols)

    def test_97_block_static_initializer_constraints(self) -> None:
        for source, message in (
            (b"int f(int n) { static int x = n; return x; }", b"arithmetic constant expression"),
            (b"int a; int f(void) { int a; static int *x = &a; return *x; }",
             b"supported symbol address"),
            (b"int f(void) { static int a[]; return a != 0; }", b"incomplete type"),
            (b"int f(void) { for (static int i; ; ) return i; }",
             rb"auto(?:matic)? or register objects"),
        ):
            with self.subTest(source=source):
                failure = self.invoke(
                    "-fsyntax-only", "-x", "c", "-", input_bytes=source, ok=False,
                )
                self.assertRegex(failure.stderr, message)

    def test_98_native_integer_casts_of_floating_literals(self) -> None:
        self._check_native_c11_fixture("literal_cast")

    def test_99_out_of_range_literal_integer_cast_is_diagnosed(self) -> None:
        failure = self.invoke(
            "-fsyntax-only", "-x", "c", "-", ok=False,
            input_bytes=b"int f(void) { return (int)1.0e100; }\n",
        )
        self.assertIn(b"floating constant is not representable", failure.stderr)

    def test_100_native_function_name_object(self) -> None:
        self._check_native_c11_fixture("function_name")
        obj = self.work / "function-name.o"
        self.invoke(RV64, "-c", FIXTURES / "cephyr_cli_native_function_name.c", "-o", obj)
        symbols = [
            (bind, section) for name, bind, section in self.elf_symbols(obj)
            if name.startswith("__cephyr_tu_func_")
        ]
        # Only the three address-taken names require storage; sizeof does not.
        self.assertEqual(len(symbols), 3)
        self.assertTrue(all(bind == 0 and section for bind, section in symbols), symbols)

    def test_101_function_name_object_constraints(self) -> None:
        for source, message in (
            (b"const char *name = __func__;", rb"only available within a function body"),
            (b"int f(void) { __func__[0] = 0; return 0; }", rb"modifiable scalar lvalue"),
        ):
            with self.subTest(source=source):
                failure = self.invoke(
                    "-fsyntax-only", "-x", "c", "-", input_bytes=source + b"\n", ok=False,
                )
                self.assertRegex(failure.stderr, message)

    def test_102_native_sizeof_in_integer_constant_expressions(self) -> None:
        self._check_native_c11_fixture("ice_sizeof")
        mir = self.work / "ice-sizeof.mir"
        self.invoke("--emit-mir", FIXTURES / "cephyr_cli_native_ice_sizeof.c", "-o", mir)
        text = mir.read_text(encoding="ascii")
        # Neither a runtime nor an ICE sizeof operand references its storage
        # or callee. In particular, sizeof __func__ needs no name object.
        for unused in ("argument_value", "external_object", "incomplete", "__cephyr_tu_func_"):
            self.assertNotIn(unused, text)

    def test_103_sizeof_ice_checks_unevaluated_operand_types(self) -> None:
        for source, message in (
            (b'_Static_assert(sizeof future == 1, ""); char future;',
             rb"no valid declaration for 'future'"),
            (b'extern int a[]; _Static_assert(1 || sizeof a, "");',
             rb"sizeof requires a complete object type"),
            (b'int f(void); _Static_assert(0 ? sizeof f : 1, "");',
             rb"sizeof requires a complete object type"),
            (b'_Static_assert(sizeof sizeof(void), "");',
             rb"sizeof requires a complete object type"),
            (b'_Static_assert(sizeof(int(void)) > 0, "");',
             rb"sizeof requires a complete object type"),
            (b'_Static_assert(_Alignof(int[]) > 0, "");',
             rb"_Alignof requires a complete object type"),
            (b'int f(int x); _Static_assert(sizeof(f()) != 0, "");',
             rb"sizeof requires .*valid operand"),
            (b'int f(int *x); _Static_assert(sizeof(f(1)) != 0, "");',
             rb"sizeof requires .*valid operand"),
            (b'int f(void) { int a[1]; return sizeof(a = 0); }',
             rb"sizeof requires .*valid operand"),
            (b'_Static_assert(sizeof(&(1 + 2)), "");',
             rb"sizeof requires .*valid operand"),
            (b'int f(register int n) { _Static_assert(sizeof &n, ""); return 0; }',
             rb"cannot take the address of a register object"),
            (b'extern int a[]; _Static_assert(sizeof(a + a == a), "");',
             rb"sizeof requires .*valid operand"),
            (b'int a[] = { sizeof a };', rb"sizeof requires a complete object type"),
            (b'int f(void) { int a[] = { sizeof a }; return 0; }',
             rb"sizeof requires a complete object type"),
            (b'int f(void) { static int a[] = { sizeof a }; return 0; }',
             rb"sizeof requires a complete object type"),
            (b'int f(unsigned n) { char a[n] = "x"; return sizeof a; }',
             rb"variable-length arrays cannot have an initializer"),
        ):
            with self.subTest(source=source):
                failure = self.invoke(
                    "-fsyntax-only", "-x", "c", "-", input_bytes=source + b"\n", ok=False,
                )
                self.assertRegex(failure.stderr, message)


    def test_104_native_generic_selection(self) -> None:
        self._check_native_c11_fixture("generic")
        mir = self.work / "generic.mir"
        self.invoke("--emit-mir", FIXTURES / "cephyr_cli_native_generic.c", "-o", mir)
        text = mir.read_text(encoding="ascii")
        self.assertNotIn("controller", text)
        self.assertNotIn("unselected", text)
        nested = "&value"
        for _ in range(24):
            nested = f"&_Generic(0, int: *({nested}))"
        self.invoke(
            "-fsyntax-only", "-x", "c", "-",
            input_bytes=f"int value; int *p(void) {{ return {nested}; }}\n".encode(),
        )

    def test_105_generic_selection_constraints(self) -> None:
        for source, message in (
            (b"int n = _Generic(0, default: 1, default: 2);",
             b"duplicate default _Generic association"),
            (b"typedef signed int I; int n = _Generic(0, int: 1, I: 2);",
             b"compatible types in multiple _Generic associations"),
            (b"int n = _Generic(0, int: 1, void: 2);", b"complete object type"),
            (b"int n = _Generic(0, int: 1, int (void): 2);", b"complete object type"),
            (b"int n = _Generic(0, int: 1, int []: 2);", b"complete object type"),
            (b"struct S; int n = _Generic(0, int: 1, struct S: 2);", b"complete object type"),
            (b"int f(int n) { return _Generic(0, int: 1, int (*)[n]: 2); }",
             b"variable-length arrays are not supported"),
            (b"int n = _Generic(0, unsigned int: 1);", b"matches no association"),
            (b"extern int (*p)[]; int n = _Generic(p, int (*)[2]: 1, int (*)[3]: 2);",
             b"matches multiple associations"),
            (b"int n = _Generic(0, int: 1, default: future); int future;",
             b"no valid declaration"),
            (b"int n = _Generic(0, int: 1, default: 1 = 2);",
             b"invalid expression in _Generic association"),
        ):
            with self.subTest(source=source):
                failure = self.invoke(
                    "-fsyntax-only", "-x", "c", "-", input_bytes=source + b"\n", ok=False,
                )
                self.assertIn(message, failure.stderr)

    def test_106_native_utf8_string_literals(self) -> None:
        self._check_native_c11_fixture("utf8_literals")

    def test_107_native_extended_identifiers(self) -> None:
        self._check_native_c11_fixture("extended_identifiers")


def parse_arguments() -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "cephyr",
        nargs="?",
        help="path to the Cephyr compiler executable (or set CEPHYR_TEST_CLI)",
    )
    parser.add_argument(
        "--qemu",
        help="qemu-riscv64 executable; auto-detected when omitted",
    )
    parser.add_argument("--qemu-aarch64", help="qemu-aarch64 executable; auto-detected when omitted")
    arguments, unittest_args = parser.parse_known_args()
    if unittest_args[:1] == ["--"]:
        unittest_args.pop(0)
    return arguments, unittest_args


def main() -> int:
    global CEPHYR, QEMU, QEMU_AARCH64
    arguments, unittest_args = parse_arguments()
    executable = arguments.cephyr or os.environ.get("CEPHYR_TEST_CLI")
    if not executable:
        raise SystemExit("pass the Cephyr executable or set CEPHYR_TEST_CLI")
    CEPHYR = Path(executable).resolve()
    QEMU = (
        arguments.qemu
        or os.environ.get("CEPHYR_TEST_QEMU")
        or os.environ.get("ARCHON_QEMU_RISCV64")
        or shutil.which("qemu-riscv64")
        or shutil.which("qemu-riscv64-static")
    )
    QEMU_AARCH64 = (
        arguments.qemu_aarch64
        or os.environ.get("CEPHYR_TEST_QEMU_AARCH64")
        or os.environ.get("ARCHON_QEMU_AARCH64")
        or shutil.which("qemu-aarch64")
        or shutil.which("qemu-aarch64-static")
    )
    program = unittest.main(
        module=__name__, argv=[sys.argv[0], *unittest_args], exit=False
    )
    return 0 if program.result.wasSuccessful() else 1


if __name__ == "__main__":
    raise SystemExit(main())
