"""Verify and execute typed indirect calls through the generated C backend."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory
from test_irgen_native import EXAMPLE, RV64IM, RV_FLAGS, TMP_ROOT, run, unicorn_exit_status


CLIENT = r"""
#include "Backend.h"

#include <assert.h>
#include <stdio.h>

#define NONE ARCHON_MIR_NONE
#define I64 ARCHON_MIR_I64
#define PTR ARCHON_MIR_PTR
#define FUNC_PTR ARCHON_MIR_FUNC_PTR
#define VOID ARCHON_MIR_VOID

static void invalid(const ArchonMIRModule *module, size_t instruction)
{
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    assert(archon_backend_compile(module, NULL, &artifact, &diag) == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_VERIFY);
    assert(diag.function == 0 && diag.block == 0 && diag.instruction == instruction);
    assert(!artifact.data && !artifact.size);
}

int main(void)
{
    const ArchonMIRType ten_params[] = {I64, I64, I64, I64, I64, I64, I64, I64, I64, I64};
    const ArchonMIRType capture_params[] = {FUNC_PTR};
    ArchonMIRType runner_types[] = {I64, I64, FUNC_PTR, FUNC_PTR, I64, I64, FUNC_PTR, I64, I64};
    const ArchonMIRType internal_types[] = {I64, I64, I64, I64, I64, I64, I64, I64, I64, I64,
                                           I64, I64};
    const ArchonMIRImport imports[] = {
        {"host_ten", I64, ten_params, 10},
        {"host_capture", VOID, capture_params, 1},
    };
    const ArchonMIRGlobal global = {"data", NULL, 1};
    const ArchonValue capture[] = {2};
    ArchonValue args[] = {0, 1, 4, 0, 1, 4, 0, 1, 4, 1};
    ArchonMIRInst runner[] = {
        {ARCHON_MIR_ADD, 1, 0, 0, 0, NULL, NULL, 0},
        {ARCHON_MIR_FUNC_ADDR, 2, NONE, NONE, 0, "host_ten", NULL, 0},
        {ARCHON_MIR_FUNC_ADDR, 3, NONE, NONE, 0, "host_capture", NULL, 0},
        {ARCHON_MIR_CALL_INDIRECT, NONE, 3, NONE, 0, "host_capture", capture, 1},
        {ARCHON_MIR_CONST, 4, NONE, NONE, 42, NULL, NULL, 0},
        {ARCHON_MIR_CALL_INDIRECT, 5, 2, NONE, 0, "archon_ten", args, 10},
        {ARCHON_MIR_FUNC_ADDR, 6, NONE, NONE, 0, "archon_ten", NULL, 0},
        {ARCHON_MIR_CALL_INDIRECT, 7, 6, NONE, 0, "host_ten", args, 10},
        {ARCHON_MIR_ADD, 8, 5, 7, 0, NULL, NULL, 0},
    };
    const ArchonMIRInst internal[] = {
        {ARCHON_MIR_ADD, 10, 0, 8, 0, NULL, NULL, 0},
        {ARCHON_MIR_ADD, 11, 10, 9, 0, NULL, NULL, 0},
    };
    const ArchonMIRBlock blocks[] = {
        {.insts = runner, .inst_count = sizeof runner / sizeof runner[0],
         .term = {.kind = ARCHON_MIR_RETURN, .value = 8}},
        {.insts = internal, .inst_count = sizeof internal / sizeof internal[0],
         .term = {.kind = ARCHON_MIR_RETURN, .value = 11}},
    };
    const ArchonMIRFunction fns[] = {
        {"archon_indirect", I64, runner_types, 9, 1, &blocks[0], 1, NULL, 0},
        {"archon_ten", I64, internal_types, 12, 10, &blocks[1], 1, NULL, 0},
    };
    const ArchonMIRModule module = {
        ARCHON_MIR_SCHEMA_VERSION, fns, 2, imports, 2, &global, 1
    };
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    ArchonBackendStatus status;
    const ArchonMIRInst internal_addr = runner[6];

    runner[1].symbol = "data";
    invalid(&module, 1);
    runner[1].symbol = "host_ten";
    runner_types[2] = PTR;
    invalid(&module, 1);
    runner_types[2] = FUNC_PTR;
    runner[5].symbol = "data";
    invalid(&module, 5);
    runner[5].symbol = "archon_ten";
    runner[5].arg_count = 9;
    invalid(&module, 5);
    runner[5].arg_count = 10;
    args[9] = 2;
    invalid(&module, 5);
    args[9] = 1;
    runner[5].dst = NONE;
    invalid(&module, 5);
    runner[5].dst = 5;
    runner[6].symbol = "host_capture";
    invalid(&module, 7);
    runner_types[6] = PTR;
    runner[6] = (ArchonMIRInst){ARCHON_MIR_GLOBAL_ADDR, 6, NONE, NONE, 0, "data", NULL, 0};
    invalid(&module, 7);
    runner[6] = (ArchonMIRInst){ARCHON_MIR_PTR_ADD, 6, 2, 4, 0, NULL, NULL, 0};
    invalid(&module, 6);
    runner[6] = internal_addr;
    runner_types[6] = FUNC_PTR;

    status = archon_backend_compile(&module, NULL, &artifact, &diag);
    if (status != ARCHON_BACKEND_OK) {
        fprintf(stderr, "stage %d, instruction %zu: %s\n",
                (int)diag.stage, diag.instruction, diag.message);
        return 1;
    }
    assert(fwrite(artifact.data, 1, artifact.size, stdout) == artifact.size);
    archon_backend_artifact_destroy(&artifact);
    return 0;
}
"""


TARGET = r"""
typedef unsigned long word;
typedef word (*callback)(word, word, word, word, word, word, word, word, word, word);

extern word archon_indirect(word);
static callback captured;

void host_capture(callback p)
{
    captured = p;
    __asm__ volatile ("li t4, -1; li t5, -1; li t6, -1" ::: "t4", "t5", "t6");
}

word host_ten(word a, word b, word c, word d, word e, word f, word g, word h,
              word i, word j)
{
    __asm__ volatile ("li t4, -2; li t5, -2; li t6, -2" ::: "t4", "t5", "t6");
    return (a == 7 && b == 14 && c == 42 && d == 7 && e == 14 && f == 42 &&
            g == 7 && h == 14 && i == 42 && j == 14) ? 2000 : 0;
}

int target_main(void)
{
    word result = archon_indirect(7);
    return result == 2063 && captured == host_ten ? 0 : 1;
}
"""


class NativeIndirectCallTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc, cmake = shutil.which("cc") or shutil.which("clang"), shutil.which("cmake")
        if not cc or not cmake:
            raise unittest.SkipTest("a host C compiler and CMake are required")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)
        work = tempfile.TemporaryDirectory(prefix="archon-indirect-", dir=TMP_ROOT)
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
        client = cls.work / "client.c"
        client.write_text(CLIENT)
        exe = cls.work / "client"
        run(cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", generated,
            client, build / "libarchon_backend.a", "-o", exe, cwd=cls.work)
        cls.assembly = cls.work / "indirect.s"
        cls.assembly.write_text(run(exe, cwd=cls.work))

    def test_call_uses_runtime_address(self):
        text = self.assembly.read_text()
        for symbol in ("host_capture", "host_ten", "archon_ten"):
            self.assertRegex(text, rf"(?m)^\s*lla x\d+, {symbol}$")
        self.assertEqual(text.count("\tjalr x1, 0(x7)\n"), 3)
        self.assertNotRegex(text, r"(?m)^\s*call\b")

    def test_execute_rv64(self):
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            self.skipTest("clang and ld.lld are required for RV64IM assemble/link")
        if not re.search(r"^\s*riscv64\s+-", run(clang, "--print-targets", cwd=self.work), re.M):
            self.skipTest("clang was built without a RISC-V 64-bit target")
        target = self.work / "target.c"
        target.write_text(TARGET)
        for source in (self.assembly, target, EXAMPLE / "start.S"):
            run(clang, *RV_FLAGS, "-O2", "-ffreestanding", "-fno-pic", "-fno-pie",
                "-fno-stack-protector", "-c", source, "-o", f"{source.stem}.o", cwd=self.work)
        elf = self.work / "indirect"
        run(lld, "-m", "elf64lriscv", "-static", "-e", "_start",
            "start.o", "indirect.o", "target.o", "-o", elf, cwd=self.work)
        qemu = (os.environ.get("ARCHON_QEMU_RISCV64") or shutil.which("qemu-riscv64")
                or shutil.which("qemu-riscv64-static"))
        if qemu:
            result = subprocess.run([qemu, elf], capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        else:
            try:
                import unicorn
                from unicorn import riscv_const
            except ImportError:
                self.skipTest("linked RV64IM ELF; execution requires QEMU or Unicorn")
            if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
                self.skipTest("linked RV64IM ELF; installed Unicorn lacks RISC-V support")
            self.assertEqual(unicorn_exit_status(elf, unicorn, riscv_const), 0)


if __name__ == "__main__":
    unittest.main()
