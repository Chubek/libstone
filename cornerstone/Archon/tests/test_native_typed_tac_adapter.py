"""Build the generated typed TAC bridge as strict C99 and execute its RV64 output."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import generate_ir, pipeline_stages, write_generated_directory

from test_irgen_native import RV64IM, RV_FLAGS, TMP_ROOT, run


CLIENT = r"""
#include "TypedTACBackend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#define WORD ir_type(IR_TYPE_BITS, 64, 0)
#define UNSIGNED ir_type(IR_TYPE_UINT, 64, 0)
#define SIGNED ir_type(IR_TYPE_SINT, 64, 0)
#define BOOL ir_type(IR_TYPE_BOOL, 1, 0)
#define NONE IR_TEMP_NONE

static IR_Block start(IR_Program *program)
{
    ir_program_init(program);
    return ir_add_block(program, "entry");
}

static IR_Operation *definition(IR_Program *program, IR_Temp temp)
{
    assert(temp < program->temporary_count);
    return &program->operations[program->temporaries[temp].defining_operation];
}

static void fail(const IR_Program *program, ArchonBackendStatus status,
    size_t block, const char *message)
{
    ArchonBackendArtifact output = {0};
    ArchonBackendDiagnostic diag;
    assert(archon_typed_tac_backend_compile(program, "tac_bad", NULL, &output, &diag)
        == status);
    assert(diag.status == status && diag.stage == ARCHON_BACKEND_VERIFY);
    assert(diag.block == block && strstr(diag.message, message));
    assert(!output.data && !output.size);
}

/* Exercise a critical edge, an unconditional branch, duplicate conditional
 * targets, forward references, and a loop with a two-value phi permutation.
 * Each phi deliberately lists its predecessor blocks in a different order. */
static void emit_cfg(unsigned flags)
{
    IR_Program program;
    IR_Block entry = start(&program), yes = ir_add_block(&program, "yes");
    IR_Block header = ir_add_block(&program, "header"), body = ir_add_block(&program, "body");
    IR_Block exit = ir_add_block(&program, "exit"), rev = ir_add_block(&program, "reverse");
    IR_Block no = ir_add_block(&program, "no");
    /* A non-entry value is first in the TAC table, not first in the ABI. */
    IR_Temp rev_init = ir_build_constant(&program, rev, UNSIGNED, 0);
    int rev_term = ir_build_branch(&program, rev, header, NONE);
    IR_Temp limit = ir_add_parameter(&program, UNSIGNED, "limit");
    IR_Temp a0 = ir_add_parameter(&program, WORD, "a");
    IR_Temp b0 = ir_add_parameter(&program, WORD, "b");
    IR_Temp mode = ir_add_parameter(&program, SIGNED, "mode");
    IR_Temp s0 = ir_build_constant(&program, entry, SIGNED, 0);
    IR_Temp u0 = ir_build_constant(&program, entry, UNSIGNED, 0);
    IR_Temp u1 = ir_build_constant(&program, entry, UNSIGNED, 1);
    IR_Temp false_value = ir_build_constant(&program, entry, BOOL, 0);
    IR_Temp true_value = ir_build_constant(&program, entry, BOOL, 1);
    IR_Temp reverse = ir_build_binary(&program, entry, IR_OP_LT, BOOL, mode, s0);
    IR_PhiInput index_inputs[] = { {body, NONE}, {entry, u0}, {rev, rev_init} };
    IR_PhiInput a_inputs[] = { {rev, b0}, {body, NONE}, {entry, a0} };
    IR_PhiInput b_inputs[] = { {entry, b0}, {body, NONE}, {rev, a0} };
    IR_PhiInput parity_inputs[] = { {entry, false_value}, {rev, true_value}, {body, NONE} };
    IR_Temp index, a, b, parity, go, next, toggle, mask, extra, seven, diff, masked,
        merged, result, choose, xor_mask, other;
    ArchonBackendArtifact artifact = {0}, again = {0};
    ArchonBackendDiagnostic diag;
    char *data;

    assert(rev_term && rev_init == 0 && limit == 1);
    assert(ir_build_cond_branch(&program, entry, reverse, rev, header, NONE));
    index = ir_build_phi(&program, header, UNSIGNED, index_inputs, 3);
    a = ir_build_phi(&program, header, WORD, a_inputs, 3);
    b = ir_build_phi(&program, header, WORD, b_inputs, 3);
    parity = ir_build_phi(&program, header, BOOL, parity_inputs, 3);
    go = ir_build_binary(&program, header, IR_OP_LT, BOOL, index, limit);
    assert(ir_build_cond_branch(&program, header, go, body, exit, NONE));
    next = ir_build_binary(&program, body, IR_OP_ADD, UNSIGNED, index, u1);
    toggle = ir_build_binary(&program, body, IR_OP_EQ, BOOL, parity, false_value);
    assert(ir_build_cond_branch(&program, body, toggle, header, header, NONE));
    program.inputs[definition(&program, index)->first_input].temp = next;
    program.inputs[definition(&program, a)->first_input + 1].temp = b;
    program.inputs[definition(&program, b)->first_input + 1].temp = a;
    program.inputs[definition(&program, parity)->first_input + 2].temp = toggle;
    mask = ir_build_constant(&program, exit, WORD, UINT64_C(0xfedcba987654fefe));
    extra = ir_build_constant(&program, exit, WORD, UINT64_C(0x0101010101010101));
    seven = ir_build_constant(&program, exit, WORD, 7);
    diff = ir_build_binary(&program, exit, IR_OP_SUB, WORD, a, b);
    masked = ir_build_binary(&program, exit, IR_OP_AND, WORD, diff, mask);
    merged = ir_build_binary(&program, exit, IR_OP_OR, WORD, masked, extra);
    result = ir_build_binary(&program, exit, IR_OP_MUL, WORD, merged, seven);
    choose = ir_build_unary(&program, exit, IR_OP_COPY, BOOL, parity);
    assert(ir_build_cond_branch(&program, exit, choose, yes, no, NONE));
    assert(ir_build_return(&program, yes, result, NONE));
    xor_mask = ir_build_constant(&program, no, WORD, UINT64_C(0x5a5a5a5aa5a5a5a5));
    other = ir_build_binary(&program, no, IR_OP_XOR, WORD, result, xor_mask);
    assert(ir_build_return(&program, no, other, NONE));

    assert(ir_verify(&program, NULL));
    assert(archon_typed_tac_backend_compile_ex(&program, "tac_loop", NULL, flags, &artifact, &diag)
        == ARCHON_BACKEND_OK);
    assert(diag.status == ARCHON_BACKEND_OK);
    assert(archon_typed_tac_backend_compile_ex(&program, "tac_loop", NULL, flags, &again, NULL)
        == ARCHON_BACKEND_OK);
    assert(again.size == artifact.size && !memcmp(again.data, artifact.data, artifact.size + 1));
    assert(program.inputs[definition(&program, a)->first_input + 1].temp == b);
    data = artifact.data;
    assert(archon_typed_tac_backend_compile(&program, "tac_loop", NULL, &artifact, &diag)
        == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_INPUT && artifact.data == data);
    assert(fwrite(artifact.data, 1, artifact.size, stdout) == artifact.size);
    archon_backend_artifact_destroy(&again);
    archon_backend_artifact_destroy(&artifact);
    ir_program_destroy(&program);
}

static void reject_edge_errors(void)
{
    IR_Program program;
    IR_Block entry = start(&program), left = ir_add_block(&program, "left");
    IR_Block right = ir_add_block(&program, "right"), join = ir_add_block(&program, "join");
    IR_Temp condition = ir_build_constant(&program, entry, BOOL, 1), a, b, phi;
    IR_PhiInput incoming[2];
    size_t first, term, term_index;
    IR_Input return_value;
    assert(ir_build_cond_branch(&program, entry, condition, left, right, NONE));
    a = ir_build_constant(&program, left, WORD, 5);
    assert(ir_build_branch(&program, left, join, NONE));
    b = ir_build_constant(&program, right, WORD, 8);
    assert(ir_build_branch(&program, right, join, NONE));
    incoming[0] = (IR_PhiInput){ left, a };
    incoming[1] = (IR_PhiInput){ right, b };
    phi = ir_build_phi(&program, join, WORD, incoming, 2);
    assert(ir_build_return(&program, join, phi, NONE));
    first = definition(&program, phi)->first_input;
    term_index = program.operation_count - 1;
    term = program.operations[term_index].first_input;
    assert(ir_verify(&program, NULL));

    program.inputs[first + 1].predecessor = left;
    fail(&program, ARCHON_BACKEND_INVALID, join, "duplicate phi predecessor");
    program.inputs[first + 1].predecessor = right;

    return_value = program.inputs[term];
    program.inputs[first + 1] = return_value;
    --program.operations[term_index].first_input;
    --definition(&program, phi)->input_count;
    --program.input_count;
    fail(&program, ARCHON_BACKEND_INVALID, join, "phi omits a control-flow predecessor");
    ++program.input_count;
    ++definition(&program, phi)->input_count;
    ++program.operations[term_index].first_input;
    program.inputs[first + 1] = (IR_Input){ b, right };

    program.inputs[first + 1].temp = a;
    assert(ir_verify(&program, NULL));  /* This value is not available on right -> join. */
    fail(&program, ARCHON_BACKEND_INVALID, right, "does not dominate");
    program.inputs[first + 1].temp = b;
    program.inputs[term].temp = a;
    assert(ir_verify(&program, NULL));  /* A direct non-dominating use must also fail. */
    fail(&program, ARCHON_BACKEND_INVALID, join, "does not dominate");
    ir_program_destroy(&program);

    entry = start(&program);
    assert(ir_build_branch(&program, entry, entry, NONE));
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_INVALID, entry, "edge to entry");
    ir_program_destroy(&program);

    entry = start(&program);
    left = ir_add_block(&program, "unreachable");
    assert(ir_build_return(&program, entry, NONE, NONE));
    assert(ir_build_return(&program, left, NONE, NONE));
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_INVALID, left, "unreachable");
    ir_program_destroy(&program);
}

static void reject_types_and_operations(void)
{
    IR_Program program;
    IR_Block entry, yes, no;
    IR_Temp a, b, c, effect;
    size_t i;
    const enum IR_OpKind shifts[] = { IR_OP_SHL, IR_OP_LSHR, IR_OP_ASHR };

    for (i = 0; i < sizeof shifts / sizeof shifts[0]; ++i) {
        entry = start(&program);
        a = ir_add_parameter(&program, WORD, "value");
        b = ir_add_parameter(&program, WORD, "count");
        assert(ir_build_binary(&program, entry, shifts[i], WORD, a, b) != NONE);
        assert(ir_build_return(&program, entry, a, NONE));
        assert(ir_verify(&program, NULL));
        fail(&program, ARCHON_BACKEND_UNSUPPORTED, entry, "exact native primitive");
        ir_program_destroy(&program);
    }

    entry = start(&program);
    a = ir_add_parameter(&program, WORD, "untyped signedness");
    assert(ir_build_binary(&program, entry, IR_OP_LT, BOOL, a, a) != NONE);
    assert(ir_build_return(&program, entry, a, NONE));
    fail(&program, ARCHON_BACKEND_UNSUPPORTED, entry, "exact native primitive");
    ir_program_destroy(&program);

    entry = start(&program);
    a = ir_build_constant(&program, entry, ir_type(IR_TYPE_UINT, 32, 0), 0);
    assert(ir_build_return(&program, entry, a, NONE));
    fail(&program, ARCHON_BACKEND_UNSUPPORTED, entry, "64-bit scalar");
    ir_program_destroy(&program);

    entry = start(&program);
    a = ir_add_parameter(&program, BOOL, "ABI bool is not admitted");
    assert(a != NONE && ir_build_return(&program, entry, NONE, NONE));
    fail(&program, ARCHON_BACKEND_UNSUPPORTED, entry, "ABI parameters");
    ir_program_destroy(&program);

    entry = start(&program);
    a = ir_build_constant(&program, entry, BOOL, 2);
    assert(a != NONE && ir_build_return(&program, entry, NONE, NONE));
    fail(&program, ARCHON_BACKEND_INVALID, entry, "constant does not fit");
    ir_program_destroy(&program);

    entry = start(&program);
    a = ir_build_effect_start(&program, entry, 0);
    b = ir_build_call(&program, entry, "callee", WORD, NULL, 0, a, 0, &effect);
    assert(ir_build_return(&program, entry, b, effect));
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_UNSUPPORTED, entry, "effects, calls or memory");
    ir_program_destroy(&program);

    entry = start(&program);
    yes = ir_add_block(&program, "yes");
    no = ir_add_block(&program, "no");
    a = ir_add_parameter(&program, UNSIGNED, "unsigned");
    b = ir_add_parameter(&program, SIGNED, "signed");
    c = ir_build_constant(&program, entry, BOOL, 0);
    assert(ir_build_cond_branch(&program, entry, c, yes, no, NONE));
    assert(ir_build_return(&program, yes, a, NONE));
    assert(ir_build_return(&program, no, b, NONE));
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_INVALID, no, "return types differ");
    /* A void return cannot join a value return either. */
    --program.operations[program.operation_count - 1].input_count;
    --program.input_count;
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_INVALID, no, "return types differ");
    ir_program_destroy(&program);
}

static void reject_malformed_and_limit_work(void)
{
    IR_Program program, bad;
    IR_Operation many_phis[512] = {{0}};
    IR_BasicBlock many_blocks[256] = {{0}};
    IR_Block entry = start(&program);
    IR_Temp a = ir_build_constant(&program, entry, WORD, 5);
    IR_Temp b = ir_build_unary(&program, entry, IR_OP_COPY, WORD, a);
    IR_Temp c = ir_build_constant(&program, entry, WORD, 7);
    size_t operand = definition(&program, b)->first_input;
    size_t i;
    static char long_name[40001];
    assert(ir_build_return(&program, entry, b, NONE));
    program.inputs[operand].temp = c;
    assert(ir_verify(&program, NULL));
    fail(&program, ARCHON_BACKEND_INVALID, entry, "does not dominate");
    program.inputs[operand].temp = (IR_Temp)program.temporary_count;
    fail(&program, ARCHON_BACKEND_INVALID, entry, "invalid temporary");
    program.inputs[operand].temp = a;

    bad = program;
    bad.inputs = NULL;
    fail(&bad, ARCHON_BACKEND_INVALID, SIZE_MAX, "table storage");
    bad = program;
    bad.operation_capacity = 0;
    fail(&bad, ARCHON_BACKEND_INVALID, SIZE_MAX, "table storage");
    bad = program;
    bad.failed = 1;
    fail(&bad, ARCHON_BACKEND_INVALID, SIZE_MAX, "table storage");
    ir_program_destroy(&program);

    entry = start(&program);
    for (i = 0; i < 4097; ++i)
        assert(ir_build_constant(&program, entry, WORD, i) != NONE);
    assert(ir_build_return(&program, entry, NONE, NONE));
    fail(&program, ARCHON_BACKEND_LIMIT, SIZE_MAX, "table limit");
    ir_program_destroy(&program);

    entry = start(&program);
    memset(long_name, 'x', sizeof long_name - 1);
    assert(ir_add_parameter(&program, WORD, long_name) != NONE);
    for (i = 0; i < 1024; ++i)
        assert(ir_build_constant(&program, entry, WORD, i) != NONE);
    assert(ir_build_return(&program, entry, NONE, NONE));
    memset(program.strings, 'x', program.string_size);
    fail(&program, ARCHON_BACKEND_LIMIT, SIZE_MAX, "verification work limit");
    ir_program_destroy(&program);

    /* These are borrowed arrays, not owned IR builder allocations. The work
     * bound must apply before the verifier scans bogus phi input lists. */
    for (i = 0; i < 512; ++i) {
        many_phis[i].kind = IR_OP_PHI;
        many_phis[i].input_count = 256;
    }
    bad = (IR_Program){ .blocks = many_blocks, .block_count = 256, .block_capacity = 256,
        .operations = many_phis, .operation_count = 512, .operation_capacity = 512 };
    fail(&bad, ARCHON_BACKEND_LIMIT, SIZE_MAX, "verification work limit (phi predecessors)");
}

static int cancelled(void *user)
{
    return *(int *)user;
}

static void backend_options(void)
{
    IR_Program program;
    IR_Block entry = start(&program);
    int cancel = 1;
    ArchonBackendOptions options = { .cancelled = cancelled, .user = &cancel };
    ArchonBackendArtifact artifact = {0};
    ArchonBackendDiagnostic diag;
    assert(ir_build_return(&program, entry, NONE, NONE));
    assert(archon_typed_tac_backend_compile(&program, "tac_void", &options, &artifact, &diag)
        == ARCHON_BACKEND_CANCELLED);
    assert(diag.status == ARCHON_BACKEND_CANCELLED && !artifact.data && !artifact.size);
    cancel = 0;
    options.max_output_bytes = 1;
    assert(archon_typed_tac_backend_compile(&program, "tac_void", &options, &artifact, &diag)
        == ARCHON_BACKEND_LIMIT);
    assert(diag.stage == ARCHON_BACKEND_EMIT && !artifact.data && !artifact.size);
    assert(archon_typed_tac_backend_compile(&program, "not a symbol", NULL, &artifact, &diag)
        == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_VERIFY && !artifact.data && !artifact.size);
    assert(archon_typed_tac_backend_compile(&program, "tac_void", NULL, &artifact, NULL)
        == ARCHON_BACKEND_OK);
    assert(strstr(artifact.data, "tac_void:"));
    archon_backend_artifact_destroy(&artifact);
    ir_program_destroy(&program);
}

int main(int argc, char **argv)
{
    unsigned flags = argc == 2 && !strcmp(argv[1], "--raw") ? ARCHON_BACKEND_MACHINE_CODE : 0;
    reject_edge_errors();
    reject_types_and_operations();
    reject_malformed_and_limit_work();
    backend_options();
    emit_cfg(flags);
    return 0;
}
"""

CMAKE = """cmake_minimum_required(VERSION 3.16)
project(TypedTACNativeClient LANGUAGES C)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED YES)
set(CMAKE_C_EXTENSIONS NO)
if(USE_INSTALLED)
    find_package(ArchonBackend CONFIG REQUIRED)
else()
    add_subdirectory(generated)
endif()
add_executable(client client.c)
target_link_libraries(client PRIVATE Archon::TypedTACBackend)
"""


class NativeTypedTACAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc") or shutil.which("clang")
        cls.cmake = shutil.which("cmake")
        if not cls.cc or not cls.cmake:
            raise unittest.SkipTest("a host C compiler and CMake are required")
        TMP_ROOT.mkdir(parents=True, exist_ok=True)
        work = tempfile.TemporaryDirectory(prefix="archon-typed-tac-native-", dir=TMP_ROOT)
        cls.addClassCleanup(work.cleanup)
        cls.work = Path(work.name)
        cls.build = cls.work / "build"
        cls.empty = cls.work / "empty"
        cls.empty.mkdir()
        cls.db = compile_file(RV64IM)
        (cls.work / "client.c").write_text(CLIENT, encoding="ascii")
        (cls.work / "CMakeLists.txt").write_text(CMAKE, encoding="ascii")
        write_generated_directory(
            generate_ir("typed-tac", [cls.db],
                        options={"profile": "native", "abi": "LP64", "oracle": False}),
            cls.work / "generated",
        )
        cls.configure(cls.build)
        run(cls.cmake, "--build", cls.build, cwd=cls.work)
        cls.asm = cls.work / "tac_loop.s"
        cls.asm.write_bytes(cls.execute(cls.build / "client"))

    @classmethod
    def configure(cls, path, *options):
        run(cls.cmake, "-S", ".", "-B", path, f"-DCMAKE_C_COMPILER={cls.cc}",
            "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic", *options, cwd=cls.work)

    @classmethod
    def execute(cls, path, *args):
        # The runtime bridge must not rely on the generator, a compiler, any
        # Program.i file, or external tools at compile time for an IR instance.
        result = subprocess.run(
            [path, *args], cwd=cls.empty, env={**os.environ, "PATH": str(cls.empty)},
            capture_output=True, timeout=10,
        )
        if result.returncode:
            raise AssertionError(result.stderr.decode(errors="replace"))
        return result.stdout

    def test_cmake_build_and_install_interfaces(self):
        self.assertNotIn("typed_tac_native", {stage.__name__.rsplit(".", 1)[-1]
                                            for stage in pipeline_stages()})
        self.assertIn("\ntac_loop:\n", self.asm.read_text(encoding="ascii"))
        prefix = self.work / "install"
        run(self.cmake, "--install", self.build / "generated", "--prefix", prefix, cwd=self.work)
        consumer = self.work / "consumer"
        self.configure(consumer, "-DUSE_INSTALLED=ON", f"-DCMAKE_PREFIX_PATH={prefix}")
        run(self.cmake, "--build", consumer, cwd=self.work)
        self.assertEqual(self.execute(consumer / "client"), self.asm.read_bytes())
        self.assertEqual(list(self.empty.iterdir()), [])

    def test_adapter_is_native_only(self):
        legacy = generate_ir("typed-tac", [self.db], options={"oracle": False})
        for name in ("TypedTACBackend.c", "TypedTACBackend.h"):
            self.assertNotIn(name, legacy)
        for name in ("IR.c", "IR.h"):
            self.assertEqual((self.work / "generated" / name).read_text(encoding="utf-8"),
                             legacy[name])

    def test_independent_rv64_cfg_execution(self):
        clang, lld = shutil.which("clang"), shutil.which("ld.lld")
        if not clang or not lld:
            self.skipTest("Clang and ld.lld are required to link RV64IM")
        if not re.search(r"^\s*riscv64\s+-", run(clang, "--print-targets", cwd=self.work), re.M):
            self.skipTest("Clang lacks an RV64 target")
        entry, ret = 0x10000, 0x13ffc
        obj, binary = self.work / "tac_loop.o", self.work / "tac_loop.bin"
        run(clang, *RV_FLAGS, "-c", self.asm, "-o", obj, cwd=self.work)
        run(lld, "-m", "elf64lriscv", "--no-relax", "--oformat=binary", "-Ttext", hex(entry),
             "-e", "tac_loop", obj, "-o", binary, cwd=self.work)
        raw = self.execute(self.build / "client", "--raw")
        self.assertEqual(raw, binary.read_bytes())
        try:
            import unicorn
            from unicorn import riscv_const as regs
        except ImportError:
            self.skipTest("linked RV64IM; independent execution requires Unicorn")
        if not unicorn.uc_arch_supported(unicorn.UC_ARCH_RISCV):
            self.skipTest("installed Unicorn lacks RISC-V support")

        uc = unicorn.Uc(unicorn.UC_ARCH_RISCV, unicorn.UC_MODE_RISCV64)
        stack, stack_size = 0x20000, 0x10000
        body = raw
        self.assertGreater(len(body), 0)
        self.assertLess(len(body), ret - entry)
        uc.mem_map(entry, 0x4000)
        uc.mem_map(stack, stack_size)
        uc.mem_write(entry, body)
        mask64 = (1 << 64) - 1

        pairs = ((0, 0), (42, 57), (0, mask64), (mask64, 3),
                 (0x8000000000000000, 0x1234567890abcdef))
        for limit in (0, 1, 2, 3, 8, 19):
            for mode in (-(1 << 63), -1, 0, 6):
                for a, b in pairs:
                    with self.subTest(limit=limit, mode=mode, a=a, b=b):
                        for reg, value in ((regs.UC_RISCV_REG_A0, limit),
                                           (regs.UC_RISCV_REG_A1, a),
                                           (regs.UC_RISCV_REG_A2, b),
                                           (regs.UC_RISCV_REG_A3, mode & mask64),
                                           (regs.UC_RISCV_REG_RA, ret),
                                           (regs.UC_RISCV_REG_SP, stack + stack_size)):
                            uc.reg_write(reg, value)
                        uc.emu_start(entry, ret, timeout=1_000_000, count=5000)
                        self.assertEqual(uc.reg_read(regs.UC_RISCV_REG_PC), ret)
                        self.assertEqual(uc.reg_read(regs.UC_RISCV_REG_SP), stack + stack_size)
                        flip = bool((limit & 1) ^ (mode < 0))
                        delta = b - a if flip else a - b
                        expected = ((delta & 0xfedcba987654fefe) | 0x0101010101010101) * 7
                        if not flip:
                            expected ^= 0x5a5a5a5aa5a5a5a5
                        self.assertEqual(uc.reg_read(regs.UC_RISCV_REG_A0), expected & mask64)


if __name__ == "__main__":
    unittest.main()
