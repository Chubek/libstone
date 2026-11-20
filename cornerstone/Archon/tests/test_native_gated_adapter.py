"""Exercise the generated native bridge using Gated SSA's runtime builder."""

from copy import deepcopy
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from archon.compiler import compile_file
from archon.irgen import build_context, generate_ir, pipeline_stages, write_generated_directory
from archon.irgen.gated_native import emit_gated_adapter


ROOT = Path(__file__).resolve().parents[1]
RV64IM = ROOT / "stdspec/riscv/rv64im.arch"
OPTIONS = {"profile": "native", "abi": "LP64", "oracle": False, "target": "RISCV64IM"}

CLIENT = r"""
#include "GatedBackend.h"

#include <assert.h>
#include <string.h>

#define WORD UINT64_C(0xfedcba9876543210)

static struct archon_ir_builder entry(struct archon_ir_program *program)
{
    struct archon_ir_builder builder;
    archon_ir_program_init(program);
    archon_ir_builder_init(&builder, program);
    assert(archon_ir_position_at_end(&builder, archon_ir_create_block(program, "entry"))
        == ARCHON_IR_OK);
    return builder;
}

static void compare_mir(void)
{
    struct archon_ir_program program;
    struct archon_ir_builder builder = entry(&program);
    struct archon_ir_type word = archon_ir_bits(64);
    struct archon_ir_value rhs = archon_ir_build_parameter(&builder, archon_ir_uint(64), 1);
    struct archon_ir_value constant = archon_ir_build_constant(&builder, word, WORD);
    struct archon_ir_value lhs = archon_ir_build_parameter(&builder, archon_ir_sint(64), 0);
    struct archon_ir_value sub_args[] = { lhs, rhs };
    struct archon_ir_value diff = archon_ir_build_node(
        &builder, ARCHON_IR_OP_RISCV64IM_SUB, word, sub_args, 2);
    struct archon_ir_value add_args[] = { diff, constant };
    struct archon_ir_value result = archon_ir_build_node(
        &builder, ARCHON_IR_OP_RISCV64IM_ADD, word, add_args, 2);
    struct archon_ir_value last = archon_ir_build_constant(&builder, word, 11);
    ArchonBackendArtifact from_gated = {0}, from_mir = {0};
    ArchonBackendArtifact raw_gated = {0}, raw_mir = {0};
    ArchonBackendDiagnostic diag;
    const ArchonMIRType types[] = {
        ARCHON_MIR_I64, ARCHON_MIR_I64, ARCHON_MIR_I64,
        ARCHON_MIR_I64, ARCHON_MIR_I64, ARCHON_MIR_I64
    };
    const ArchonMIRInst insts[] = {
        { .op = ARCHON_MIR_CONST, .dst = 2, .a = ARCHON_MIR_NONE,
          .b = ARCHON_MIR_NONE, .imm = WORD },
        { .op = ARCHON_MIR_SUB, .dst = 3, .a = 0, .b = 1 },
        { .op = ARCHON_MIR_ADD, .dst = 4, .a = 3, .b = 2 },
        { .op = ARCHON_MIR_CONST, .dst = 5, .a = ARCHON_MIR_NONE,
          .b = ARCHON_MIR_NONE, .imm = 11 }
    };
    const ArchonMIRBlock block = {
        .insts = insts, .inst_count = 4, .term = { .kind = ARCHON_MIR_RETURN, .value = 4 }
    };
    const ArchonMIRFunction function = {
        .name = "gated_scalar", .result = ARCHON_MIR_I64, .types = types,
        .value_count = 6, .param_count = 2, .blocks = &block, .block_count = 1
    };
    const ArchonMIRModule module = {
        .version = ARCHON_MIR_SCHEMA_VERSION, .functions = &function, .function_count = 1
    };

    assert(result.id && last.id > result.id);
    assert(archon_ir_seal_block(&program, builder.block) == ARCHON_IR_OK);
    assert(archon_gated_backend_compile(&program, "gated_scalar", result.id,
        NULL, &from_gated, &diag) == ARCHON_BACKEND_OK);
    assert(diag.status == ARCHON_BACKEND_OK);
    assert(archon_backend_compile(&module, NULL, &from_mir, NULL) == ARCHON_BACKEND_OK);
    assert(from_gated.size == from_mir.size);
    assert(memcmp(from_gated.data, from_mir.data, from_gated.size + 1) == 0);
    assert(strstr(from_gated.data, "gated_scalar:"));
    assert(strstr(from_gated.data, "\tsub "));
    assert(strstr(from_gated.data, "\tadd "));
    assert(archon_gated_backend_compile_ex(&program, "gated_scalar", result.id,
        NULL, ARCHON_BACKEND_MACHINE_CODE, &raw_gated, NULL) == ARCHON_BACKEND_OK);
    assert(archon_backend_compile_ex(&module, NULL, ARCHON_BACKEND_MACHINE_CODE,
        &raw_mir, NULL) == ARCHON_BACKEND_OK);
    assert(raw_gated.size && raw_gated.size == raw_mir.size);
    assert(memcmp(raw_gated.data, raw_mir.data, raw_mir.size) == 0);
    archon_backend_artifact_destroy(&raw_mir);
    archon_backend_artifact_destroy(&raw_gated);
    archon_backend_artifact_destroy(&from_mir);
    archon_backend_artifact_destroy(&from_gated);
    archon_ir_program_dispose(&program);
}

static void expect_failure(const struct archon_ir_program *program, uint32_t result,
    ArchonBackendStatus expected, size_t instruction, const char *message)
{
    ArchonBackendArtifact output = {0};
    ArchonBackendDiagnostic diag;
    assert(archon_gated_backend_compile(program, "gated_bad", result, NULL, &output, &diag)
        == expected);
    assert(diag.status == expected && diag.stage == ARCHON_BACKEND_VERIFY);
    assert(diag.instruction == instruction);
    assert(strstr(diag.message, message));
    assert(!output.data && !output.size);
}

static void reject_opcode(enum archon_ir_opcode opcode)
{
    struct archon_ir_program program;
    struct archon_ir_builder builder = entry(&program);
    struct archon_ir_type word = archon_ir_bits(64);
    struct archon_ir_value value = archon_ir_build_parameter(&builder, word, 0);
    struct archon_ir_value args[] = { value, value };
    assert(archon_ir_build_node(&builder, opcode, word, args, 2).id);
    assert(archon_ir_verify(&program, NULL, 0) == ARCHON_IR_OK);
    /* Selecting the parameter as result must not hide a dead unsupported node. */
    expect_failure(&program, value.id, ARCHON_BACKEND_UNSUPPORTED, 1, "exact native primitive");
    archon_ir_program_dispose(&program);
}

static void reject_ambiguous_or_malformed(void)
{
    struct archon_ir_program program;
    struct archon_ir_builder builder = entry(&program);
    struct archon_ir_type word = archon_ir_bits(64);
    struct archon_ir_value value = archon_ir_build_parameter(&builder, word, 0);
    struct archon_ir_value args[] = { value, value };
    struct archon_ir_value result = archon_ir_build_node(
        &builder, ARCHON_IR_OP_RISCV64IM_ADD, word, args, 1);
    assert(archon_ir_verify(&program, NULL, 0) == ARCHON_IR_OK);
    expect_failure(&program, 0, ARCHON_BACKEND_INVALID, SIZE_MAX, "result ID");
    expect_failure(&program, 3, ARCHON_BACKEND_INVALID, SIZE_MAX, "result ID");
    expect_failure(&program, result.id, ARCHON_BACKEND_INVALID, 1, "input count");

    program.inputs[0].value = result.id;
    expect_failure(&program, result.id, ARCHON_BACKEND_INVALID, SIZE_MAX, "defined before use");
    archon_ir_program_dispose(&program);

    builder = entry(&program);
    args[0] = args[1] = archon_ir_build_parameter(&builder, word, 0);
    result = archon_ir_build_node(&builder, ARCHON_IR_OP_RISCV64IM_ADD, word, args, 2);
    program.nodes[1].payload = 1;
    expect_failure(&program, result.id, ARCHON_BACKEND_INVALID, 1, "payload");
    archon_ir_program_dispose(&program);

    builder = entry(&program);
    value = archon_ir_build_parameter(&builder, word, 0);
    assert(archon_ir_build_parameter(&builder, word, 0).id);
    expect_failure(&program, value.id, ARCHON_BACKEND_INVALID, 1, "unique and dense");
    program.nodes[1].payload = UINT64_MAX;
    expect_failure(&program, value.id, ARCHON_BACKEND_INVALID, 1, "unique and dense");
    archon_ir_program_dispose(&program);

    builder = entry(&program);
    value = archon_ir_build_constant(&builder, word, 123);
    assert(archon_ir_create_block(&program, "no executable edge") == 2);
    assert(archon_ir_verify(&program, NULL, 0) == ARCHON_IR_OK);
    expect_failure(&program, value.id, ARCHON_BACKEND_UNSUPPORTED, SIZE_MAX, "control-flow");
    archon_ir_program_dispose(&program);

    builder = entry(&program);
    value = archon_ir_build_constant(&builder, archon_ir_uint(32), 123);
    expect_failure(&program, value.id, ARCHON_BACKEND_UNSUPPORTED, 0, "64-bit scalar");
    archon_ir_program_dispose(&program);

    builder = entry(&program);
    assert(archon_ir_build_effect_start(&builder).id);
    value = archon_ir_build_constant(&builder, word, 123);
    expect_failure(&program, value.id, ARCHON_BACKEND_UNSUPPORTED, 0, "not effects");
    archon_ir_program_dispose(&program);
}

static int cancelled(void *user)
{
    return *(int *)user;
}

static void backend_options(void)
{
    struct archon_ir_program program;
    struct archon_ir_builder builder = entry(&program);
    struct archon_ir_value result = archon_ir_build_parameter(&builder, archon_ir_bits(64), 0);
    ArchonBackendArtifact output = {0};
    ArchonBackendDiagnostic diag;
    int cancel = 1;
    ArchonBackendOptions options = { .cancelled = cancelled, .user = &cancel };

    assert(archon_gated_backend_compile(&program, "identity", result.id, &options, &output,
        &diag) == ARCHON_BACKEND_CANCELLED);
    assert(diag.status == ARCHON_BACKEND_CANCELLED && !output.data && !output.size);
    cancel = 0;
    options.max_output_bytes = 1;
    assert(archon_gated_backend_compile(&program, "identity", result.id, &options, &output,
        &diag) == ARCHON_BACKEND_LIMIT);
    assert(diag.stage == ARCHON_BACKEND_EMIT && !output.data && !output.size);
    options.max_output_bytes = 0;
    assert(archon_gated_backend_compile(&program, "identity", result.id, &options, &output,
        NULL) == ARCHON_BACKEND_OK);
    assert(strstr(output.data, "identity:"));
    assert(archon_gated_backend_compile(&program, "identity", result.id, &options, &output,
        &diag) == ARCHON_BACKEND_INVALID);
    assert(diag.stage == ARCHON_BACKEND_INPUT && strstr(output.data, "identity:"));
    archon_backend_artifact_destroy(&output);
    archon_ir_program_dispose(&program);
}

int main(void)
{
    compare_mir();
    reject_opcode(ARCHON_IR_OP_GENERIC);
    reject_opcode(ARCHON_IR_OP_OTHER_ADD);
    reject_opcode(ARCHON_IR_OP_RISCV64IM_ADDI);
    reject_opcode(ARCHON_IR_OP_RISCV64IM_DIV);
    reject_opcode(ARCHON_IR_OP_RISCV64IM_SLL);
    reject_ambiguous_or_malformed();
    backend_options();
    return 0;
}
"""


def other_db(db):
    """Build validated rows with misleading names and an enum-name collision."""
    other = deepcopy(db)
    other["architecture"] = "OTHER"
    add = next(row for row in other["instructions"] if row["name"] == "ADD")
    sub = next(row for row in other["instructions"] if row["name"] == "SUB")
    other["instructions"] = [
        dict(add, name="ADD", id="OTHER.ADD", semantics=None),
        dict(sub, name="MUL", id="OTHER.MUL"),
        dict(add, name="a-b", id="OTHER.a-b", semantics=None),
        dict(add, name="a_b", id="OTHER.a_b"),
    ]
    return other


class NativeGatedAdapterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = compile_file(RV64IM)
        cls.other = other_db(cls.db)

    def test_generated_opcode_cases_use_exact_semantics_and_inventory(self):
        context = build_context("gated", [self.other, self.db], options=OPTIONS)
        source = emit_gated_adapter(context)["GatedBackend.c"]
        self.assertIn("case ARCHON_IR_OP_RISCV64IM_ADD: *arity = 2; return ARCHON_MIR_ADD;", source)
        self.assertIn("case ARCHON_IR_OP_OTHER_A_B_2: *arity = 2; return ARCHON_MIR_ADD;", source)
        self.assertIn("case ARCHON_IR_OP_OTHER_MUL: *arity = 2; return ARCHON_MIR_SUB;", source)
        for rejected in ("OTHER_ADD", "OTHER_A_B:", "RISCV64IM_SLL", "RISCV64IM_ADDI"):
            self.assertNotIn("case ARCHON_IR_OP_" + rejected, source)
        self.assertNotIn("gated_native", {stage.__name__.rsplit(".", 1)[-1]
                                          for stage in pipeline_stages()})

    def test_cmake_link_and_runtime_builder(self):
        cc = shutil.which("cc") or shutil.which("clang")
        cmake = shutil.which("cmake")
        if not cc or not cmake:
            self.skipTest("a host C compiler and CMake are required")
        temporary = Path("/tmp/opencode")
        temporary.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="archon-gated-native-", dir=temporary) as work:
            root = Path(work)
            outputs = generate_ir("gated", [self.other, self.db], options=OPTIONS)
            write_generated_directory(outputs, root / "generated")
            (root / "client.c").write_text(CLIENT, encoding="ascii")
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.16)\n"
                "project(GatedNativeClient LANGUAGES C)\n"
                "set(CMAKE_C_STANDARD 99)\n"
                "set(CMAKE_C_STANDARD_REQUIRED ON)\n"
                "set(CMAKE_C_EXTENSIONS OFF)\n"
                "add_subdirectory(generated)\n"
                "add_executable(client client.c)\n"
                "target_link_libraries(client PRIVATE archon_gated_backend)\n",
                encoding="ascii",
            )
            self.run_process(root, cmake, "-S", ".", "-B", "build", f"-DCMAKE_C_COMPILER={cc}",
                             "-DCMAKE_C_FLAGS=-Wall -Wextra -Werror -pedantic")
            self.run_process(root, cmake, "--build", "build", "--target", "client")
            empty = root / "empty"
            empty.mkdir()
            self.run_process(empty, root / "build/client")
            self.assertEqual(list(empty.iterdir()), [])

    def run_process(self, cwd, *command):
        result = subprocess.run(command, cwd=cwd, capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
