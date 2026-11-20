"""Build and exercise the generated native sandbox with freestanding ELF fixtures."""

import os
from pathlib import Path
import platform
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import time
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, pipeline_stages, sandbox


ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get("CC", "cc"))
STRICT = ("-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-pedantic")
NATIVE = sys.platform == "linux" and platform.machine() == "x86_64" and struct.calcsize("P") == 8
EXIT = "mov $60, %eax; xor %edi, %edi; syscall"


class SandboxGenerationTests(unittest.TestCase):
    def test_discovery_and_default_generation(self):
        db = compile_file(ROOT / "stdspec" / "riscv" / "rv64i.arch")
        self.assertIn(sandbox, pipeline_stages())
        full = generate_ir("anf", [db])
        self.assertEqual(full["RunSandbox.c"], sandbox.emit({})["RunSandbox.c"])
        self.assertNotIn("RunSandbox.c", generate_ir("anf", [db], run_stages=False))

    @unittest.skipUnless(CC, "a C compiler is required")
    def test_unsupported_host_fails_closed(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as tmp:
            source = Path(tmp) / "RunSandbox.c"
            binary = Path(tmp) / "RunSandbox"
            source.write_text(sandbox.emit({})["RunSandbox.c"], encoding="utf-8")
            subprocess.run([CC, *STRICT, "-U__linux__", str(source), "-o", str(binary)], check=True)
            result = subprocess.run([str(binary), "/bin/true"], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 126, result.stderr)
            self.assertIn("unsupported host", result.stderr)


@unittest.skipUnless(NATIVE and CC, "requires a Linux x86-64 host and C compiler")
class NativeSandboxTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        directory = tempfile.TemporaryDirectory(dir="/tmp/opencode")
        cls.addClassCleanup(directory.cleanup)
        cls.root = Path(directory.name)
        cls.cwd = cls.root / "cwd"
        cls.cwd.mkdir()
        cls.source = cls.root / "RunSandbox.c"
        cls.runner = cls.root / "RunSandbox"
        cls.source.write_text(sandbox.emit({})["RunSandbox.c"], encoding="utf-8")
        subprocess.run([CC, *STRICT, str(cls.source), "-o", str(cls.runner)], check=True)
        cls.make_elf("simple-exit", EXIT)
        cls.probe = cls.run_runner("simple-exit")

    @classmethod
    def make_elf(cls, name, body):
        source = ".global _start\n.text\n_start:\n" + body + "\n"
        subprocess.run(
            [CC, "-nostdlib", "-static", "-no-pie", "-Wl,-z,noexecstack", "-x", "assembler",
             "-o", str(cls.cwd / name), "-"],
            input=source, text=True, capture_output=True, check=True,
        )

    @classmethod
    def run_runner(cls, *args, runner=None, cwd=None, **kwargs):
        return subprocess.run(
            [str(runner or cls.runner), *args], cwd=cwd or cls.cwd,
            capture_output=True, text=True, timeout=12, **kwargs,
        )

    def require_kernel(self):
        stderr = self.probe.stderr
        if self.probe.returncode == 126 and any(
            mode in stderr for mode in (
                "create executable memfd (requires Linux 6.3+ and executable memfds)",
                "restrict filesystem (requires Landlock ABI 3+)",
            )
        ) and any(reason in stderr for reason in (
            "Function not implemented", "Operation not supported", "Invalid argument",
            "Permission denied", "Operation not permitted",
        )):
            self.skipTest(stderr.strip())
        self.assertEqual(self.probe.returncode, 0, stderr)

    def test_cwd_native_elf_with_literal_arguments_and_empty_env(self):
        self.require_kernel()
        self.make_elf("hello static", r"""
            cmpq $2, (%rsp)
            jne bad
            mov 16(%rsp), %rax
            cmpb $36, (%rax)
            jne bad
            cmpq $0, 32(%rsp)
            jne bad
            mov $1, %eax
            mov $1, %edi
            lea hello(%rip), %rsi
            mov $6, %edx
            syscall
            mov $1, %eax
            mov $2, %edi
            lea again(%rip), %rsi
            mov $6, %edx
            syscall
            mov $60, %eax
            mov $42, %edi
            syscall
        bad:
            mov $60, %eax
            mov $43, %edi
            syscall
        .section .rodata
        hello: .ascii "hello\n"
        again: .ascii "again\n"
        """)
        result = self.run_runner("--", "hello static", "$TEST_LITERAL", env={**os.environ, "SECRET": "hidden"})
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertEqual(result.stdout, "hello\nagain\n")
        self.assertEqual(result.stderr, "RunSandbox: child exit 42\n")

    def test_reserved_exits_do_not_collide_with_launcher_status(self):
        self.require_kernel()
        for number in (124, 125, 126):
            with self.subTest(number=number):
                self.make_elf("reserved-exit", f"mov $60, %eax; mov ${number}, %edi; syscall")
                result = self.run_runner("reserved-exit")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertEqual(result.stderr, f"RunSandbox: child exit {number}\n")

    def test_private_setup_descriptor_is_not_available_after_exec(self):
        self.require_kernel()
        self.make_elf("no-descriptors", r"""
            mov $1, %eax
            mov $4, %edi
            lea marker(%rip), %rsi
            mov $1, %edx
            syscall
            cmp $-9, %rax
            jne bad
            mov $60, %eax
            xor %edi, %edi
            syscall
        bad:
            mov $60, %eax
            mov $1, %edi
            syscall
        .section .rodata
        marker: .ascii "x"
        """)
        dir_fd = os.open(self.cwd, os.O_RDONLY | os.O_DIRECTORY)
        try:
            result = self.run_runner("no-descriptors", pass_fds=(dir_fd,))
        finally:
            os.close(dir_fd)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_filesystem_network_process_and_stdin_calls_are_rejected(self):
        self.require_kernel()
        for name, os_call in {
            "filesystem": 'mov $257, %eax; mov $-100, %edi; lea path(%rip), %rsi; xor %edx, %edx; syscall',
            "network": "mov $41, %eax; mov $2, %edi; mov $1, %esi; xor %edx, %edx; syscall",
            "process": "mov $57, %eax; syscall",
            "stdin": "xor %eax, %eax; xor %edi, %edi; mov %rsp, %rsi; mov $1, %edx; syscall",
        }.items():
            with self.subTest(name=name):
                self.make_elf(name, f'{os_call}\n{EXIT}\n.section .rodata\npath: .asciz "test-file"\n')
                result = self.run_runner(name, input="not forwarded")
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn("SIGSYS: syscall policy", result.stderr)
                self.assertEqual(result.stdout, "")

    def test_landlock_denies_file_access_independently_of_seccomp(self):
        self.require_kernel()
        source = self.root / "landlock-test.c"
        source.write_text(
            '#define main runner_main\n#include "RunSandbox.c"\n#undef main\n'
            'int main(void) {\n'
            '    int fd = open("simple-exit", O_RDONLY);\n'
            '    if (fd < 0 || close(fd) < 0) return 1;\n'
            '    if (prctl(PR_SET_NO_NEW_PRIVS, 1UL, 0UL, 0UL, 0UL) < 0 || restrict_filesystem() < 0)\n'
            '        return 2;\n'
            '    if (open("simple-exit", O_RDONLY) >= 0 || errno != EACCES) return 3;\n'
            '    return 0;\n'
            '}\n',
            encoding="ascii",
        )
        probe = source.with_suffix("")
        subprocess.run([CC, *STRICT, str(source), "-o", str(probe)], check=True)
        result = subprocess.run([str(probe)], cwd=self.cwd, text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_wall_cpu_and_child_signal_status(self):
        self.require_kernel()
        self.make_elf("spin", "jmp _start")
        wall = self.run_runner("--timeout-ms", "50", "spin")
        self.assertEqual(wall.returncode, 124, wall.stderr)
        self.assertIn("wall timeout", wall.stderr)
        cpu = self.run_runner("--timeout-ms", "10000", "spin")
        self.assertEqual(cpu.returncode, 1, cpu.stderr)
        self.assertIn("SIGXCPU: CPU limit", cpu.stderr)
        self.make_elf("fault", "ud2")
        fault = self.run_runner("fault")
        self.assertEqual(fault.returncode, 1, fault.stderr)
        self.assertIn(f"child signal {int(signal.SIGILL)}", fault.stderr)

    def test_interrupt_supervisor(self):
        self.require_kernel()
        self.make_elf("interruptible", "jmp _start")
        with subprocess.Popen(
            [str(self.runner), "interruptible"], cwd=self.cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True,
        ) as runner:
            time.sleep(0.1)
            runner.send_signal(signal.SIGTERM)
            stdout, stderr = runner.communicate(timeout=5)
            self.assertEqual(runner.returncode, 126, stderr)
            self.assertEqual(stdout, "")
            self.assertIn(f"interrupted by signal {int(signal.SIGTERM)}", stderr)

    def test_output_budget(self):
        self.require_kernel()
        self.make_elf("lots-of-output", r"""
            mov $32, %r12d
        next:
            mov $1, %eax
            mov $1, %edi
            lea block(%rip), %rsi
            mov $4096, %edx
            syscall
            dec %r12d
            jne next
            """ + EXIT + '\n.section .rodata\nblock: .fill 4096,1,65\n')
        result = self.run_runner("lots-of-output")
        self.assertEqual(result.returncode, 125, result.stderr)
        self.assertEqual(result.stdout, "A" * 65536)
        self.assertIn("output limit", result.stderr)

    def test_address_space_budget(self):
        self.require_kernel()
        self.make_elf("large-bss", EXIT + "\n.bss\n.skip 134217728\n")
        result = self.run_runner("large-bss")
        self.assertIn(result.returncode, (1, 126), result.stderr)
        self.assertNotIn("child exit 0", result.stderr)

    def test_invalid_files_and_cli(self):
        shell = self.cwd / "shell-script"
        shell.write_text("#!/bin/sh\nprintf not-executed\n", encoding="ascii")
        shell.chmod(0o755)
        wrong = self.cwd / "wrong-arch"
        payload = bytearray((self.cwd / "simple-exit").read_bytes())
        payload[18:20] = struct.pack("<H", 183)  # AArch64
        wrong.write_bytes(payload)
        wrong.chmod(0o755)
        noexec = self.cwd / "not-executable"
        noexec.write_bytes((self.cwd / "simple-exit").read_bytes())
        noexec.chmod(0o644)
        fifo = self.cwd / "fifo"
        os.mkfifo(fifo, 0o755)
        (self.cwd / "symlink").symlink_to("simple-exit")
        dynamic = self.cwd / "dynamic"
        subprocess.run(
            [CC, "-no-pie", "-x", "c", "-o", str(dynamic), "-"],
            input="int main(void) {return 0;}\n", text=True, capture_output=True, check=True,
        )
        for args in (
            (), ("--timeout-ms", "-1", "simple-exit"), ("--timeout-ms", "30001", "simple-exit"),
            ("--typo",), ("does-not-exist",), (".",), ("fifo",), ("symlink",), (noexec.name,),
        ):
            with self.subTest(args=args):
                result = self.run_runner(*args)
                self.assertEqual(result.returncode, 126, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertNotIn("RunSandbox: child exit", result.stderr)
        self.require_kernel()
        for name in (wrong.name, shell.name, dynamic.name):
            with self.subTest(name=name):
                result = self.run_runner(name)
                self.assertEqual(result.returncode, 126, result.stderr)
                self.assertEqual(result.stdout, "")
                self.assertIn("unsupported executable", result.stderr)

    def test_setup_failure_does_not_execute_program(self):
        self.require_kernel()
        # Force a kernel rejection in a separate test-only translation unit.
        # The generated runner has no failure injection or bypass option.
        source = self.source.read_text(encoding="utf-8")
        for old, replacement, reason in (
            ("syscall(SYS_landlock_restrict_self,", "syscall(-1L,", "restrict filesystem"),
            ("syscall(SYS_seccomp,", "syscall(-1L,", "install seccomp filter"),
            ("IMAGE_FD, empty, args, env, AT_EMPTY_PATH",
             "IMAGE_FD, empty, (void *)(uintptr_t)1, env, AT_EMPTY_PATH", "exec sealed ELF"),
        ):
            with self.subTest(call=old):
                fake = self.root / "unsupported.c"
                self.assertIn(old, source)
                modified = source.replace(old, replacement)
                if reason == "exec sealed ELF":
                    modified = modified.replace("    (void)syscall(SYS_execveat,", "    (void)args;\n    (void)syscall(SYS_execveat,")
                fake.write_text(modified, encoding="utf-8")
                runner = self.root / "BrokenSandbox"
                subprocess.run([CC, *STRICT, str(fake), "-o", str(runner)], check=True)
                result = self.run_runner("simple-exit", runner=runner)
                self.assertEqual(result.returncode, 126, result.stderr)
                self.assertIn(reason, result.stderr)
                self.assertNotIn("RunSandbox: child exit", result.stderr)
