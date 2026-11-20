"""Build the generated native backend and exercise it as an external C client."""

from __future__ import annotations

import os
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory


ROOT = Path(__file__).resolve().parents[1]
EXAMPLE = ROOT / "examples" / "native-backend"
RV64IM = ROOT / "stdspec" / "riscv" / "rv64im.arch"
TMP_ROOT = Path("/tmp/opencode")
RV_FLAGS = ("--target=riscv64-linux-gnu", "-march=rv64im", "-mabi=lp64")
TRAPS = (
    "add overflow", "add underflow", "subtract underflow", "subtract overflow",
    "divide by zero", "divide overflow",
)
ELF_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")


def run(*command, cwd):
    result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=90)
    if result.returncode != 0:
        raise AssertionError(
            f"{' '.join(map(str, command))} exited {result.returncode}\n"
            f"{result.stdout}{result.stderr}"
        )
    return result.stdout


def unicorn_exit_status(path, uc_api, regs):
    """Load our freestanding ELF; handle only the Linux exit ecall and ebreak."""
    data = path.read_bytes()
    (ident, kind, machine, _, entry, phoff, _, _, _, phsize, phnum,
     *_) = ELF_HEADER.unpack_from(data)
    if ident[:7] != b"\x7fELF\x02\x01\x01" or kind != 2 or machine != 243:
        raise AssertionError(f"expected a little-endian executable ELF for RV64: {path}")
    if phsize != PROGRAM_HEADER.size:
        raise AssertionError(f"unexpected ELF program header size: {path}")

    uc = uc_api.Uc(uc_api.UC_ARCH_RISCV, uc_api.UC_MODE_RISCV64)
    page = 4096
    mapped = set()
    for i in range(phnum):
        kind, _, offset, addr, _, filesz, memsz, _ = PROGRAM_HEADER.unpack_from(
            data, phoff + i * phsize
        )
        if kind != 1 or memsz == 0:  # PT_LOAD
            continue
        if filesz > memsz or offset + filesz > len(data):
            raise AssertionError(f"invalid ELF load segment: {path}")
        for start in range(addr & -page, addr + memsz, page):
            if start not in mapped:
                uc.mem_map(start, page)
                mapped.add(start)
        if filesz:
            uc.mem_write(addr, data[offset:offset + filesz])

    stack, stack_size = 0x40000000, 1024 * 1024
    uc.mem_map(stack, stack_size)
    uc.reg_write(regs.UC_RISCV_REG_SP, stack + stack_size)
    outcome = []

    def interrupt(machine, exception, _):
        if exception == 8 and machine.reg_read(regs.UC_RISCV_REG_A7) == 93:
            outcome.append(machine.reg_read(regs.UC_RISCV_REG_A0) & 0xff)
        elif exception == 3:  # RISC-V breakpoint exception (ebreak)
            outcome.append(-signal.SIGTRAP)
        else:
            outcome.append(f"unexpected RV64 exception {exception}")
        machine.emu_stop()

    uc.hook_add(uc_api.UC_HOOK_INTR, interrupt)
    try:
        uc.emu_start(entry, 0, timeout=1_000_000, count=100_000)
    except uc_api.UcError as exc:
        if exc.errno != uc_api.UC_ERR_INSN_INVALID:
            raise
        # Some Unicorn versions stop at ebreak with INSN_INVALID instead of
        # reporting a breakpoint interrupt. Do not accept other illegal opcodes.
        pc = uc.reg_read(regs.UC_RISCV_REG_PC)
        if uc.mem_read(pc, 4) != bytes.fromhex("73 00 10 00"):
            raise
        return -signal.SIGTRAP
    if not outcome:
        raise AssertionError(f"{path} did not exit or trap within the emulation budget")
    return outcome[0]


class NativeBackendEndToEndTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which("cc") or shutil.which("clang")
        cmake = shutil.which("cmake")
        if not cc or not cmake:
            raise unittest.SkipTest("a host C compiler and CMake are required")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)
        work = tempfile.TemporaryDirectory(prefix="archon-native-e2e-", dir=TMP_ROOT)
        cls.addClassCleanup(work.cleanup)
        cls.work = Path(work.name)
        generated = write_generated_directory(
            generate_ir(
                "gated", [compile_file(RV64IM)],
                options={"profile": "native", "abi": "LP64", "oracle": False},
            ),
            cls.work / "generated",
        )
        build = cls.work / "build"
        run(cmake, "-S", generated, "-B", build, f"-DCMAKE_C_COMPILER={cc}",
            "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic", cwd=cls.work)
        run(cmake, "--build", build, "--target", "archon_backend", cwd=cls.work)
        for source, args in (
            ("client.c", ("choice.s", "scale.s")),
            ("test_client.c", ("coverage.s",)),
        ):
            exe = cls.work / Path(source).stem
            run(cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", generated,
                EXAMPLE / source, build / "libarchon_backend.a", "-o", exe, cwd=cls.work)
            run(exe, *args, cwd=cls.work)
        cls.programs = None

    @classmethod
    def link_programs(cls):
        if cls.programs is not None:
            return cls.programs
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            raise unittest.SkipTest("clang and ld.lld are required for RV64IM assemble/link")
        if not re.search(r"^\s*riscv64\s+-", run(clang, "--print-targets", cwd=cls.work), re.M):
            raise unittest.SkipTest("clang was built without a RISC-V 64-bit target")

        def assemble(source, name, *options):
            obj = cls.work / f"{name}.o"
            run(clang, *RV_FLAGS, "-ffreestanding", "-fno-pic", "-fno-pie", "-fno-stack-protector",
                *options, "-c", source, "-o", obj, cwd=cls.work)
            return obj

        def link(name, *objects):
            elf = cls.work / name
            run(lld, "-m", "elf64lriscv", "-static", "-e", "_start",
                *objects, "-o", elf, cwd=cls.work)
            return elf

        start = assemble(EXAMPLE / "start.S", "start")
        choice, scale, coverage = (
            assemble(cls.work / f"{name}.s", name) for name in ("choice", "scale", "coverage")
        )
        demo = assemble(EXAMPLE / "target.c", "demo-target", "-O2")
        programs = {"interop": link("interop", start, demo, choice, scale)}
        for case, name in enumerate(("coverage", *TRAPS)):
            target = assemble(EXAMPLE / "test_target.c", f"coverage-{case}-target",
                              "-O2", f"-DTRAP_CASE={case}")
            programs[name] = link(f"coverage-{case}", start, target, coverage)
        cls.programs = programs
        return programs

    def test_host_public_c_api(self):
        # Both fixture binaries use the exported ABI, not backend internals.
        choice = (self.work / "choice.s").read_text()
        scale = (self.work / "scale.s").read_text()
        coverage = (self.work / "coverage.s").read_text()
        self.assertIn("choose_max:", choice)
        self.assertNotIn("choose_max:", scale)
        self.assertIn("scale_max:", scale)
        pressure = coverage.split("\nspill_pressure:\n", 1)[1]
        # More stack writes than the function's single incoming parameter; this
        # leaf has no calls or locals, so the extra stack traffic is spilling.
        stack = r"\s+\w+,\s*(-?\d+)\((?:x2|sp)\)"
        written = set(re.findall(r"^\s*sd" + stack, pressure, re.M))
        read = set(re.findall(r"^\s*ld" + stack, pressure, re.M))
        self.assertGreater(len(written & read), 1)

    def test_cross_assemble_and_link(self):
        for name, program in self.link_programs().items():
            with self.subTest(program=name):
                ident, kind, machine, _, entry, *_ = ELF_HEADER.unpack_from(program.read_bytes())
                self.assertEqual(ident[:7], b"\x7fELF\x02\x01\x01")
                self.assertEqual((kind, machine), (2, 243))
                self.assertNotEqual(entry, 0)

    def test_execute_rv64(self):
        # Always assemble and link first, even on systems without an emulator.
        programs = self.link_programs()
        qemu = (os.environ.get("ARCHON_QEMU_RISCV64") or shutil.which("qemu-riscv64")
                or shutil.which("qemu-riscv64-static"))
        if not qemu:
            try:
                import unicorn
                from unicorn import riscv_const
            except ImportError:
                self.skipTest("linked RV64IM ELFs; execution requires qemu-riscv64, "
                              "ARCHON_QEMU_RISCV64, or Unicorn with RISC-V support")
            if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
                self.skipTest("linked RV64IM ELFs; installed Unicorn lacks RISC-V support")

        for name, program in programs.items():
            with self.subTest(program=name):
                expected = -signal.SIGTRAP if name in TRAPS else 0
                if qemu:
                    result = subprocess.run(
                        ["sh", "-c", 'ulimit -c 0; exec "$@"', "qemu", qemu, program],
                        cwd=self.work, capture_output=True, text=True, timeout=10,
                    )
                    self.assertEqual(result.returncode, expected, result.stdout + result.stderr)
                else:
                    self.assertEqual(unicorn_exit_status(program, unicorn, riscv_const), expected)


if __name__ == "__main__":
    unittest.main()
