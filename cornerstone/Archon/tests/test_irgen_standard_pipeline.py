"""Exercise generated code and assembled bytes for every standard ArchDB."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from archon.irgen import generate_ir, write_generated_directory
from archon.irgen.core import compile_standard_architectures
from archon.irgen.macros import validate_stream
from archon.runtime import Target


def anf(width, primitive, rhs="IR_ARG_VALUE(right);"):
    return f"""\
IR_PROGRAM_BEGIN("standard-pipeline");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I{width});
IR_PARAM(left, "left", IR_TYPE_I{width});
IR_PARAM(right, "right", IR_TYPE_I{width});
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(result, IR_TYPE_I{width}, IR_PRIM_{primitive}, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
{rhs}
IR_LET_END();
IR_RETURN(result);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""


def gated_effects(target, *instructions):
    lines = [
        'ARCHON_IR_PROGRAM_BEGIN(1, "gated")',
        'ARCHON_IR_BLOCK(1, 0, "entry")',
        'ARCHON_IR_NODE(1, 1, ARCHON_IR_OP_EFFECT_START, '
        'ARCHON_IR_TYPE_EFFECT, 0, UINT64_C(0))',
    ]
    for i, instruction in enumerate(instructions, 2):
        lines.extend((
            f"ARCHON_IR_NODE({i}, 1, ARCHON_IR_OP_{target}_{instruction}, "
            "ARCHON_IR_TYPE_EFFECT, 0, UINT64_C(0))",
            f"ARCHON_IR_INPUT({i}, {i - 1}, 0)",
        ))
    return "\n".join(lines + ["ARCHON_IR_PROGRAM_END()", ""])


PDP11 = """\
ARCHON_IR_PROGRAM_BEGIN(1, "gated")
ARCHON_IR_BLOCK(1, 0, "entry")
ARCHON_IR_NODE(1, 1, ARCHON_IR_OP_EFFECT_START, ARCHON_IR_TYPE_EFFECT, 0, UINT64_C(0))
ARCHON_IR_NODE(2, 1, ARCHON_IR_OP_PDP11_SEC, ARCHON_IR_TYPE_EFFECT, 0, UINT64_C(0))
ARCHON_IR_INPUT(2, 1, 0)
ARCHON_IR_NODE(3, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 16, UINT64_C(0))
ARCHON_IR_NODE(4, 1, ARCHON_IR_OP_PDP11_MOV, ARCHON_IR_TYPE_UINT, 16, UINT64_C(0))
ARCHON_IR_INPUT(4, 3, 0)
ARCHON_IR_PROGRAM_END()
"""

STACK_CASES = {
    "Wasm32": (("DROP", "NOP", "DROP"), b"\x1a\x01\x1a"),
    "JVM": (("ICONST_M1", "ICONST_5", "LCONST_1"), b"\x02\x08\x0a"),
    "CIL": (("LDC_I4_M1", "DUP", "POP"), b"\x15\x25\x26"),
}

REGISTER_CASES = {
    "RISCV32": (32, "ADD", ("ADDI",), {"rs1": 0xFFFFFFFF}, "rd", 4),
    "RISCV64": (64, "SUB", ("SUB",), {"rs1": 2, "rs2": 5}, "rd", 2**64 - 3),
    "RISCV64IM": (64, "MUL", ("MUL",), {"rs1": 7, "rs2": 6}, "rd", 42),
    "AArch64": (64, "XOR", ("EOR",), {"rn": 0xAA, "rm": 0xFF}, "rd", 0x55),
    "MIPS32": (32, "ADD", ("ADDU",), {"rs": 0xFFFFFFFF, "rt": 3}, "rd", 2),
    "AMD64": (64, "COPY", ("MOV",), {"src": 0xFEDCBA9876543210}, "dst", 0xFEDCBA9876543210),
    "PDP11": (16, None, ("SEC", "MOV"), {"src": 0x8001}, "dst", 0x8001),
}

MISPLACED = {
    "gated": """\
ARCHON_IR_PROGRAM_BEGIN(1, "gated")
ARCHON_IR_BLOCK(1, 0, "entry")
ARCHON_IR_NODE(1, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 64, UINT64_C(0))
ARCHON_IR_NODE(2, 1, ARCHON_IR_OP_PARAMETER, ARCHON_IR_TYPE_UINT, 64, UINT64_C(0))
ARCHON_IR_NODE(3, 1, ARCHON_IR_OP_RISCV64_ADD, ARCHON_IR_TYPE_UINT, 64, UINT64_C(0))
ARCHON_IR_INPUT(3, 1, 0)
ARCHON_IR_INPUT(3, 2, 0)
ARCHON_IR_NODE(4, 1, ARCHON_IR_OP_RISCV64IM_ADD, ARCHON_IR_TYPE_UINT, 64, UINT64_C(0))
ARCHON_IR_INPUT(4, 3, 0)
ARCHON_IR_INPUT(4, 2, 0)
ARCHON_IR_PROGRAM_END()
""",
    "memory-ssa": """\
ARCHON_IR_DECLARE_BLOCK(0, "entry");
ARCHON_IR_ENTRY(0);
ARCHON_IR_BLOCK(0);
ARCHON_IR_PARAMETER(0, IR_TYPE_BITS, 64, "left");
ARCHON_IR_PARAMETER(1, IR_TYPE_BITS, 64, "right");
{
    const IR_value_id operands[2] = {0, 1};
ARCHON_IR_TARGET(2, IR_NO_VALUE, ARCHON_IR_OP_RISCV64_ADD,
                     IR_TYPE_BITS, 64, IR_NO_VALUE, 2, operands);
}
{
    const IR_value_id operands[2] = {2, 1};
    ARCHON_IR_TARGET(3, IR_NO_VALUE, ARCHON_IR_OP_RISCV64IM_ADD,
                     IR_TYPE_BITS, 64, IR_NO_VALUE, 2, operands);
}
ARCHON_IR_RETURN(3, IR_NO_VALUE);
""",
}


class StandardPipelineTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.databases = compile_standard_architectures()
        cls.by_name = {db["architecture"]: db for db in cls.databases}
        cls.compiler = shutil.which("gcc") or shutil.which("clang")
        if cls.compiler is None:
            raise unittest.SkipTest("GCC or Clang is required")

    def build(self, directory, representation, source, name, *,
              mixed=False, assembler="archon"):
        databases = self.databases if mixed else [self.by_name[name]]
        options = {"assembler": assembler, "oracle": False}
        if mixed:
            options["target"] = name
        write_generated_directory(
            generate_ir(representation, databases, options=options), directory,
        )
        (directory / "Program.i").write_text(source, encoding="ascii")
        result = subprocess.run(
            [self.compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
             '-DARCHON_IR_PROGRAM_FILE="Program.i"', "-DARCHON_ISEL_NO_MAIN",
             "-DARCHON_REGALLOC_NO_MAIN", "-I.", "IR.c", "Graphs.c", "Rewrite.c",
             "ISel.c", "Pipeline.c", "ISched.c", "Regalloc.c", "Emitter.c",
             "Driver.c", "-o", "driver"],
            cwd=directory, capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    @staticmethod
    def run_in(directory, *argv):
        return subprocess.run(argv, cwd=directory, capture_output=True, text=True)

    def assert_register_result(self, target, name, binary):
        _, _, instructions, values, output, expected = REGISTER_CASES[name]
        width = len(binary) // len(instructions)
        self.assertEqual(len(binary), width * len(instructions))
        decoded = [target.decode(binary[i:i + width]) for i in range(0, len(binary), width)]
        self.assertEqual(tuple(row[0] for row in decoded), instructions)
        operands = decoded[-1][1]
        inputs = target.new_state(scalars={"C": 0} if name == "PDP11" else None)
        for key, value in values.items():
            inputs.registers[operands[key]] = value
        if len(values) > 1:
            self.assertEqual(len({operands[key] for key in values}), len(values))
        if name == "AMD64":
            self.assertTrue(all(index < 8 and index != 4 for index in operands.values()))
        result = target.execute_program_state(binary, inputs)
        self.assertEqual(result.registers[operands[output]], expected)
        if name == "PDP11":
            self.assertEqual(result.scalars, {"N": 1, "Z": 0, "V": 0, "C": 1})

    def assert_stack_result(self, target, name, binary):
        self.assertEqual(binary, STACK_CASES[name][1])
        initial = None
        if name == "Wasm32":
            initial = {"OperandStack": [42, 66, 77], "OperandTypes": [1, 2, 1]}
            expected = {"OperandStack": [42], "OperandTypes": [1]}
        elif name == "JVM":
            expected = {"OperandStack": [0xFFFFFFFF, 5, 1], "OperandKinds": [1, 1, 2]}
        else:
            expected = {"EvaluationStack": [0xFFFFFFFF], "ValueKinds": [1]}
        state = target.execute_program_state(binary, target.new_state(stacks=initial))
        self.assertEqual(state.stacks, expected)
        self.assertEqual(state.registers, [])

    @staticmethod
    def source(name):
        if name == "PDP11":
            return "gated", PDP11
        if name in STACK_CASES:
            return "gated", gated_effects(name.upper(), *STACK_CASES[name][0])
        width, primitive, *_ = REGISTER_CASES[name]
        kwargs = {"rhs": {
            "RISCV32": "IR_ARG_INT(IR_TYPE_I32, 5);",
            "AMD64": "",
        }.get(name, "IR_ARG_VALUE(right);")}
        return "anf", anf(width, primitive, **kwargs)

    def test_every_stdspec_runs_individually_and_as_a_selected_target(self):
        self.assertEqual(self.by_name.keys(), REGISTER_CASES.keys() | STACK_CASES.keys())
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-std-pipeline-") as raw:
            for name in sorted(self.by_name):
                binary_by_mode = {}
                for mixed in (False, True):
                    with self.subTest(target=name, mixed=mixed):
                        directory = Path(raw) / f"{name}-{mixed}"
                        self.build(directory, *self.source(name), name, mixed=mixed)
                        completed = self.run_in(directory, "./driver", "--keep-artifacts")
                        self.assertEqual(completed.returncode, 0, completed.stderr)
                        for artifact, kind in (
                            ("MIR.i", "mir"), ("InstrMap.i", "instr_map"),
                            ("Scheduled.i", "scheduled"), ("RegMap.i", "reg_map"),
                            ("Regallocated.i", "regallocated"),
                        ):
                            validate_stream((directory / artifact).read_text(), kind)
                        assembled = self.run_in(
                            directory, "./Assembler.sh", "-o", "image.bin", "Program.archs",
                        )
                        self.assertEqual(assembled.returncode, 0, assembled.stderr)
                        binary = (directory / "image.bin").read_bytes()
                        binary_by_mode[mixed] = binary
                        target = Target(self.by_name[name])
                        if name in STACK_CASES:
                            self.assert_stack_result(target, name, binary)
                        else:
                            self.assert_register_result(target, name, binary)
                with self.subTest(target=name, mode="determinism"):
                    self.assertEqual(binary_by_mode.get(False), binary_by_mode.get(True))

    def test_unsupported_stack_arithmetic_is_not_emitted_as_executable(self):
        cases = {"Wasm32": "I32_ADD", "JVM": "IADD", "CIL": "ADD"}
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-stack-reject-") as raw:
            for name, opcode in cases.items():
                with self.subTest(target=name):
                    directory = Path(raw) / name
                    self.build(directory, "gated", gated_effects(name.upper(), opcode), name)
                    rejected = self.run_in(directory, "./driver", "--keep-artifacts")
                    self.assertEqual(rejected.returncode, 1)
                    self.assertIn(f"{opcode} is encoding-only on {name}", rejected.stderr)
                    self.assertFalse((directory / "InstrMap.i").exists())
                    self.assertFalse((directory / "Program.archs").exists())

                    (directory / "Program.i").unlink()
                    (directory / "Regallocated.i").write_text(
                        f'REGALLOC_EMISSION_BEGIN(1)\n'
                        f'REGALLOC_INSTRUCTION_BEGIN("{opcode}", 0)\n'
                        'REGALLOC_INSTRUCTION_END()\nREGALLOC_EMISSION_END()\n',
                        encoding="ascii",
                    )
                    restarted = self.run_in(directory, "./driver")
                    self.assertEqual(restarted.returncode, 1)
                    self.assertIn(f"{opcode} is encoding-only on {name}", restarted.stderr)
                    self.assertFalse((directory / "Program.archs").exists())

    def test_stack_profiles_reject_gas_and_generic_ssa(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-stack-profile-") as raw:
            for name in STACK_CASES:
                with self.subTest(target=name):
                    directory = Path(raw) / f"{name}-gated"
                    self.build(directory, *self.source(name), name)
                    rejected = self.run_in(directory, "./driver", "--dialect", "gas")
                    self.assertEqual(rejected.returncode, 1)
                    self.assertIn(f"target {name} has no validated built-in syntax", rejected.stderr)
                    self.assertFalse((directory / "Program.s").exists())
                    directory = Path(raw) / f"{name}-anf"
                    self.build(directory, "anf", anf(32, "ADD"), name)
                    rejected = self.run_in(directory, "./driver")
                    self.assertEqual(rejected.returncode, 1)
                    self.assertIn("SSA-to-stack lowering is unavailable", rejected.stderr)
                    self.assertFalse((directory / "Program.archs").exists())

    def test_amd64_restart_checks_encoding_register_ranges(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-restart-range-") as raw:
            directory = Path(raw) / "amd64"
            self.build(directory, *self.source("AMD64"), "AMD64")
            allocated = self.run_in(
                directory, "./driver", "--stop-after", "regalloc", "--keep-artifacts",
            )
            self.assertEqual(allocated.returncode, 0, allocated.stderr)
            artifact = directory / "Regallocated.i"
            invalid, changed = re.subn(
                r"(?m)^REGALLOC_OPERAND_REGISTER\(\d+\)$",
                "REGALLOC_OPERAND_REGISTER(8)", artifact.read_text(), count=1,
            )
            self.assertEqual(changed, 1)
            artifact.write_text(invalid, encoding="ascii")
            (directory / "Program.i").unlink()
            rejected = self.run_in(directory, "./driver")
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("register 8 is not available for dst", rejected.stderr)
            self.assertFalse((directory / "Program.archs").exists())

    def test_mixed_explicit_opcodes_keep_architecture_provenance(self):
        # Both profiles have ADD with identical widths/operands. Rejecting this
        # requires looking at the source opcode's architecture, not its name.
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-target-cross-") as raw:
            for representation, source in MISPLACED.items():
                with self.subTest(representation=representation):
                    directory = Path(raw) / representation
                    self.build(directory, representation, source, "RISCV64IM", mixed=True)
                    rejected = self.run_in(directory, "./driver", "--keep-artifacts")
                    self.assertEqual(rejected.returncode, 1)
                    self.assertIn("pipeline: isel stage:", rejected.stderr)
                    self.assertIn("belongs to a different target", rejected.stderr)
                    self.assertFalse((directory / "InstrMap.i").exists())
                    self.assertFalse((directory / "Program.archs").exists())

    def test_selected_profile_does_not_borrow_the_riscv_m_extension(self):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-std-feature-") as raw:
            directory = Path(raw) / "rv64i"
            self.build(directory, "anf", anf(64, "MUL"), "RISCV64", mixed=True)
            rejected = self.run_in(directory, "./driver", "--keep-artifacts")
            self.assertEqual(rejected.returncode, 1)
            self.assertIn("pipeline: isel stage:", rejected.stderr)
            self.assertIn("no legal instruction selection", rejected.stderr)
            self.assertFalse((directory / "Program.archs").exists())

    def test_unsigned_word_constant_can_use_a_sign_extended_riscv_immediate(self):
        source = anf(32, "ADD", "IR_ARG_INT(IR_TYPE_I32, UINT32_C(0xffffffff));")
        source = source.replace("IR_TYPE_I32", "IR_TYPE_U32")
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-rv32-immediate-") as raw:
            directory = Path(raw) / "rv32i"
            self.build(directory, "anf", source, "RISCV32")
            completed = self.run_in(directory, "./driver")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            assembled = self.run_in(
                directory, "./Assembler.sh", "-o", "image.bin", "Program.archs",
            )
            self.assertEqual(assembled.returncode, 0, assembled.stderr)
            target = Target(self.by_name["RISCV32"])
            binary = (directory / "image.bin").read_bytes()
            name, operands = target.decode(binary)
            self.assertEqual((name, operands["imm"]), ("ADDI", -1))
            state = target.new_state()
            state.registers[operands["rs1"]] = 42
            self.assertEqual(target.execute_program_state(binary, state).registers[operands["rd"]], 41)

    def test_input_used_late_is_live_at_fragment_entry(self):
        source = """\
IR_PROGRAM_BEGIN("late-input");
IR_FUNCTION_BEGIN(main, "main", IR_TYPE_I32);
IR_PARAM(left, "left", IR_TYPE_I32);
IR_PARAM(late, "late", IR_TYPE_I32);
IR_BLOCK_BEGIN(entry, "entry");
IR_LET_BEGIN(first, IR_TYPE_I32, IR_PRIM_ADD, IR_EFFECT_NONE);
IR_ARG_VALUE(left);
IR_ARG_INT(IR_TYPE_I32, 5);
IR_LET_END();
IR_LET_BEGIN(result, IR_TYPE_I32, IR_PRIM_SUB, IR_EFFECT_NONE);
IR_ARG_VALUE(first);
IR_ARG_VALUE(late);
IR_LET_END();
IR_RETURN(result);
IR_BLOCK_END();
IR_FUNCTION_END();
IR_PROGRAM_END();
"""
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-live-in-") as raw:
            directory = Path(raw) / "rv32i"
            self.build(directory, "anf", source, "RISCV32")
            completed = self.run_in(directory, "./driver")
            self.assertEqual(completed.returncode, 0, completed.stderr)
            assembled = self.run_in(
                directory, "./Assembler.sh", "-o", "image.bin", "Program.archs",
            )
            self.assertEqual(assembled.returncode, 0, assembled.stderr)
            target = Target(self.by_name["RISCV32"])
            binary = (directory / "image.bin").read_bytes()
            first_opcode, first = target.decode(binary[:4])
            second_opcode, second = target.decode(binary[4:])
            self.assertEqual((first_opcode, second_opcode), ("ADDI", "SUB"))
            self.assertEqual(second["rs1"], first["rd"])
            self.assertNotIn(second["rs2"], (first["rs1"], first["rd"]))
            state = target.new_state()
            state.registers[first["rs1"]] = 20
            state.registers[second["rs2"]] = 10
            self.assertEqual(target.execute_program_state(binary, state).registers[second["rd"]], 15)

    def test_real_cross_assemblers_accept_supported_gas_output(self):
        llvm_mc = shutil.which("llvm-mc")
        objcopy = shutil.which("llvm-objcopy")
        if not llvm_mc or not objcopy:
            self.skipTest("requires LLVM MC and llvm-objcopy")
        with tempfile.TemporaryDirectory(dir="/tmp/opencode", prefix="archon-std-gas-") as raw:
            for name in ("AArch64", "AMD64", "MIPS32", "RISCV32", "RISCV64", "RISCV64IM"):
                with self.subTest(target=name):
                    directory = Path(raw) / name
                    self.build(directory, *self.source(name), name, assembler="llvm")
                    completed = self.run_in(directory, "./driver")
                    self.assertEqual(completed.returncode, 0, completed.stderr)
                    assembled = self.run_in(directory, "./Assembler.sh", "-o", "image.o", "Program.s")
                    self.assertEqual(assembled.returncode, 0, assembled.stderr)
                    extracted = self.run_in(directory, objcopy, "--dump-section", ".text=image.bin", "image.o")
                    self.assertEqual(extracted.returncode, 0, extracted.stderr)
                    binary = (directory / "image.bin").read_bytes()
                    self.assert_register_result(Target(self.by_name[name]), name, binary)


if __name__ == "__main__":
    unittest.main()
