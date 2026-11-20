"""Exercise the allocated physical record stream through the generated C ABI."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, write_generated_directory


SPEC = Path(__file__).resolve().parents[1] / "stdspec" / "riscv" / "rv64im.arch"
CLIENT = r"""
#include "Backend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define NONE ARCHON_MIR_NONE
#define I64 ARCHON_MIR_I64
#define PTR ARCHON_MIR_PTR
#define OK ARCHON_BACKEND_OK

static size_t polls, stop_at;

static int cancel(void *user)
{
    (void)user;
    return ++polls == stop_at;
}

int main(void)
{
    const ArchonMIRType types[] = {I64, PTR, I64, I64, ARCHON_MIR_FUNC_PTR};
    const ArchonMIRType arg_types[] = {PTR};
    const ArchonValue arg[] = {1};
    const ArchonMIRImport observer = {"observe", ARCHON_MIR_VOID, arg_types, 1};
    const uint64_t word = UINT64_C(0xfedcba9876543210);
    const ArchonMIRGlobal globals[] = {
        {"datum", &word, 1}, {"storage", NULL, 2}, {"external_data", NULL, 0},
    };
    const ArchonMIRInst insts[] = {
        {ARCHON_MIR_GLOBAL_ADDR, 1, NONE, NONE, 0, "datum", NULL, 0},
        {ARCHON_MIR_LOAD, 2, 1, NONE, (uint64_t)-8, NULL, NULL, 0},
        {ARCHON_MIR_ADD, 3, 0, 2, 0, NULL, NULL, 0},
        {ARCHON_MIR_STORE, NONE, 1, 3, 8, NULL, NULL, 0},
        {ARCHON_MIR_CALL, NONE, NONE, NONE, 0, "observe", arg, 1},
        {ARCHON_MIR_FUNC_ADDR, 4, NONE, NONE, 0, "ret", NULL, 0},
        {ARCHON_MIR_CALL_INDIRECT, NONE, 4, NONE, 0, "ret", NULL, 0},
    };
    const ArchonMIRBlock run[] = {
        {NULL, 0, insts, sizeof insts / sizeof insts[0],
            {.kind = ARCHON_MIR_CBR, .value = 3, .yes = {1, NULL, 0}, .no = {2, NULL, 0}}},
        {.term = {.kind = ARCHON_MIR_RETURN, .value = 3}},
        {.term = {.kind = ARCHON_MIR_TRAP, .value = NONE}},
    };
    const ArchonMIRBlock ret[] = {
        {.term = {.kind = ARCHON_MIR_RETURN, .value = NONE}},
    };
    const ArchonMIRFunction functions[] = {
        {"run", I64, types, 5, 1, run, 3, NULL, 0},
        {"ret", ARCHON_MIR_VOID, NULL, 0, 0, ret, 1, NULL, 0},
    };
    ArchonMIRModule module = {
        ARCHON_MIR_SCHEMA_VERSION, functions, 2, &observer, 1, globals, 3
    };
    ArchonBackendArtifact gas = {0}, physical = {0}, other = {0};
    ArchonBackendDiagnostic diag;
    ArchonBackendOptions options = {.cancelled = cancel};

    assert(archon_backend_compile(&module, NULL, &gas, &diag) == OK);
    assert(archon_backend_compile_physical(&module, &options, &physical, &diag) == OK);
    assert(physical.data && strlen(physical.data) == physical.size);
    stop_at = polls;
    assert(stop_at > 1);

    /* Cancellation at the last checkpoint must not publish a partial stream. */
    polls = 0;
    assert(archon_backend_compile_physical(&module, &options, &other, &diag) ==
        ARCHON_BACKEND_CANCELLED);
    assert(diag.stage == ARCHON_BACKEND_EMIT && !other.data && !other.size);

    options.cancelled = NULL;
    options.max_output_bytes = physical.size - 1;
    assert(archon_backend_compile_physical(&module, &options, &other, &diag) ==
        ARCHON_BACKEND_LIMIT);
    assert(diag.stage == ARCHON_BACKEND_EMIT && !other.data && !other.size);

    options.max_output_bytes = physical.size;
    assert(archon_backend_compile_physical(&module, &options, &other, &diag) == OK);
    assert(other.size == physical.size);
    assert(memcmp(other.data, physical.data, physical.size + 1) == 0);
    archon_backend_artifact_destroy(&other);

    /* Opting in must not affect the default assembler output on later calls. */
    assert(archon_backend_compile(&module, NULL, &other, &diag) == OK);
    assert(other.size == gas.size && memcmp(other.data, gas.data, gas.size + 1) == 0);
    assert(strstr(gas.data, "\t.option push\n\t.option norvc\n\t.option norelax\n\t.text\n"));
    assert(strstr(gas.data, "\tcall observe\n"));
    assert(strstr(gas.data, "\tlla x7, datum\n"));
    assert(strstr(gas.data, "\tlla x7, ret\n"));
    assert(strstr(gas.data, "\tjalr x1, 0(x7)\n"));
    assert(!strstr(gas.data, "archon-physical"));
    archon_backend_artifact_destroy(&other);

    module.functions = NULL;
    assert(archon_backend_compile_physical(&module, NULL, &other, &diag) ==
        ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_VERIFY && !other.data && !other.size);

    if (fwrite(physical.data, 1, physical.size, stdout) != physical.size)
        return 1;
    archon_backend_artifact_destroy(&physical);
    archon_backend_artifact_destroy(&gas);
    return 0;
}
"""


class NativeInspectionTests(unittest.TestCase):
    def test_allocated_records_are_deterministic_bounded_and_symbolic(self):
        cmake = shutil.which("cmake")
        cc = shutil.which("cc")
        if not cmake or not cc:
            self.skipTest("CMake and a host C compiler are required")

        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(prefix="archon-physical-",
                                         dir=tmp if tmp.is_dir() else None) as directory:
            root = Path(directory)
            generated = write_generated_directory(
                generate_ir("gated", [compile_file(SPEC)], options={"profile": "native", "oracle": False}),
                root / "generated",
            )
            build = root / "build"
            flags = "-Wall -Wextra -Werror -pedantic-errors"
            client = root / "client.c"
            client.write_text(CLIENT, encoding="ascii")
            executable = root / "inspection"
            for command in (
                (cmake, "-S", generated, "-B", build, f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_C_FLAGS={flags}"),
                (cmake, "--build", build, "--target", "archon_backend"),
                (cc, "-std=c99", *flags.split(), "-I", generated,
                 client, build / "libarchon_backend.a", "-o", executable),
            ):
                result = subprocess.run(command, capture_output=True, text=True, cwd=root, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([executable], capture_output=True, text=True, cwd=root, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)

        lines = result.stdout.splitlines()
        self.assertEqual(lines[:2], ["archon-physical 1", "target RISCV64IM/LP64"])
        self.assertRegex(lines[2], r"^archdb-sha256 [a-f0-9]{64}$")
        self.assertEqual(lines[3], "function run")
        self.assertEqual(lines[-7:], [
            "inst jalr x0 x1 0", "end-function ret", "global datum init 1",
            "word 0xfedcba9876543210", "global storage zero 2", "global external_data extern", "end",
        ])
        expected = [
            "label 0:0",
            "address-symbol x7 datum", "inst ld x7 x5 -8", "inst add x7 x5 x6",
            "inst sd x6 x5 8", "call-symbol x1 observe", "address-symbol x7 ret",
            "inst jalr x1 x7 0",
            "branch bne x5 x0 x28 0:3", "jump x28 0:2", "label 0:3", "jump x28 0:1",
            "label 0:1", "inst jalr x0 x1 0", "label 0:2", "inst ebreak", "label 0:4",
            "jump x28 0:4", "end-function run", "function ret", "label 1:0",
        ]
        self.assertRegex(lines[4], r"^inst addi x2 x2 -[0-9]+$")
        self.assertRegex(lines[5], r"^inst sd x1 x2 [0-9]+$")
        remaining = iter(lines)
        for record in expected:
            self.assertTrue(any(line == record for line in remaining), (record, lines))

        registers = r"x(?:[0-9]|[12][0-9]|3[01])"
        for line in lines:
            if line.startswith("inst "):
                self.assertRegex(line, rf"^inst [a-z]+(?: {registers})*(?: -?[0-9]+)?$")
            elif line.startswith(("branch ", "jump ")):
                self.assertIn("label " + line.split()[-1], lines)


if __name__ == "__main__":
    unittest.main()
