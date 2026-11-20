"""Compile one native emulator containing physical and virtual stdspec targets."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.c99_emulator import generate_emulator
from archon.compiler import compile_file
from archon.runtime import Target


ROOT = Path(__file__).resolve().parents[1]
STDSPEC = ROOT / "stdspec"


def _values(values):
    return ", ".join(f"UINT64_C(0x{value:x})" for value in values)


class StandardSpecEmulators(unittest.TestCase):
    def test_native_semantic_boundary(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")

        cases = (
            ("arm", "aarch64/root.arch", "add x1, x2, x3", {2: 7, 3: 9}, True),
            ("mips", "mips32/root.arch", "addu r1, r2, r3", {2: 7, 3: 9}, True),
            ("amd", "amd64/root.arch", "mov rax, rbx", {3: 9}, True),
            ("pdp", "pdp11/root.arch", "nop", {0: 0xFFFF}, True),
            ("wasm", "wasm/root.arch", "i32.add", {}, False),
            ("jvm", "jvm/root.arch", "iadd", {}, False),
            ("cil", "clr/root.arch", "add", {}, False),
        )
        generated = []
        checks = []
        for prefix, spec, assembly, assignments, _ in cases:
            db = compile_file(STDSPEC / spec)
            target = Target(db)
            name, operands = target.assemble(assembly)
            instruction = next(inst for inst in db["instructions"] if inst["name"] == name)
            positional = [operands[operand["name"]] for operand in instruction["operands"]]
            register_count = db["registers"]["count"] if db["registers"] is not None else 0
            initial = [0] * register_count
            for index, value in assignments.items():
                initial[index] = value
            effects = instruction["semantics"]["effects"] if instruction["semantics"] else {}
            needs_state = any(
                effects.get(key) for key in (
                    "state_reads", "state_writes", "stack_reads", "stack_writes",
                    "memory_reads", "memory_writes",
                )
            )
            executable = instruction["semantics"] is not None and not needs_state
            expected = target.execute(name, operands, initial) if executable else initial
            generated.append(generate_emulator(db, prefix))
            argument_name = f"{prefix}_args" if positional else "NULL"
            declaration = (
                f"    const uint64_t {argument_name}[] = {{{_values(positional)}}};\n"
                if positional else ""
            )
            if executable:
                status = f"{prefix.upper()}_EMU_OK"
            elif needs_state:
                status = f"{prefix.upper()}_EMU_STATE_REQUIRED"
            else:
                status = f"{prefix.upper()}_EMU_NO_SEMANTICS"
            if initial:
                declaration += (
                    f"    uint64_t {prefix}_regs[] = {{{_values(initial)}}};\n"
                    f"    const uint64_t {prefix}_expected[] = {{{_values(expected)}}};\n"
                )
                registers = f"{prefix}_regs"
                comparison = (
                    f"    assert(memcmp({prefix}_regs, {prefix}_expected, "
                    f"sizeof {prefix}_regs) == 0);\n"
                )
            else:
                registers = "NULL"
                comparison = ""
            checks.append(
                declaration
                + f'    assert({prefix}_emu_execute("{name}", {argument_name}, {len(positional)}, '
                  f"{registers}, {len(initial)}) == {status});\n"
                + comparison
            )

        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "stdspec.c"
            source.write_text(
                "\n".join(generated)
                + "\n#include <assert.h>\nint main(void) {\n"
                + "".join(checks)
                + "    return 0;\n}\n",
                encoding="ascii",
            )
            executable = directory / "stdspec"
            subprocess.run(
                [*command, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 source, "-o", executable],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_native_standard_spec_state(self):
        command = shlex.split(os.environ.get("CC", "cc"))
        if not command or not shutil.which(command[0]):
            self.skipTest("C99 compiler unavailable")

        generated = []
        for prefix, spec in (
            ("pdp", "pdp11/root.arch"),
            ("wasm", "wasm/root.arch"),
            ("jvm", "jvm/root.arch"),
            ("cil", "clr/root.arch"),
        ):
            generated.append(generate_emulator(compile_file(STDSPEC / spec), prefix))

        checks = r"""
    {
        const uint64_t dst[] = {0};
        uint64_t registers[PDP_EMU_REGISTER_COUNT] = {0};
        uint8_t memory[PDP_EMU_MEMORY_MainMemory_SIZE] = {0};
        struct pdp_emu_state state = {0};
        registers[0] = UINT64_C(0x7fff);
        state.registers = registers;
        state.scalar_C = 1;
        state.memory_MainMemory = memory;
        assert(pdp_emu_execute("INC", dst, 1, registers, PDP_EMU_REGISTER_COUNT)
                == PDP_EMU_STATE_REQUIRED);
        assert(registers[0] == UINT64_C(0x7fff));
        assert(pdp_emu_execute_state("INC", dst, 1, &state) == PDP_EMU_OK);
        assert(registers[0] == UINT64_C(0x8000));
        assert(state.scalar_N == 1 && state.scalar_Z == 0);
        assert(state.scalar_V == 1 && state.scalar_C == 1);
        assert(memory[0] == 0);
    }
    {
        uint64_t values[WASM_EMU_STACK_OperandStack_CAPACITY] = {7};
        uint64_t types[WASM_EMU_STACK_OperandTypes_CAPACITY] = {1};
        struct wasm_emu_state state = {0};
        state.stack_OperandStack.values = values;
        state.stack_OperandStack.size = 1;
        state.stack_OperandTypes.values = types;
        state.stack_OperandTypes.size = 1;
        assert(wasm_emu_execute_state("DROP", NULL, 0, &state) == WASM_EMU_OK);
        assert(state.stack_OperandStack.size == 0);
        assert(state.stack_OperandTypes.size == 0);

        state.stack_OperandTypes.size = 1;
        assert(wasm_emu_execute_state("DROP", NULL, 0, &state)
                == WASM_EMU_STACK_UNDERFLOW);
        assert(state.stack_OperandStack.size == 0);
        assert(state.stack_OperandTypes.size == 1);
    }
    {
        uint64_t values[JVM_EMU_STACK_OperandStack_CAPACITY] = {0};
        uint64_t kinds[JVM_EMU_STACK_OperandKinds_CAPACITY] = {0};
        struct jvm_emu_state state = {0};
        state.stack_OperandStack.values = values;
        state.stack_OperandKinds.values = kinds;
        assert(jvm_emu_execute_state("ICONST_M1", NULL, 0, &state) == JVM_EMU_OK);
        assert(state.stack_OperandStack.size == 1);
        assert(state.stack_OperandKinds.size == 1);
        assert(values[0] == UINT64_C(0xffffffff) && kinds[0] == 1);
    }
    {
        uint64_t values[CIL_EMU_STACK_EvaluationStack_CAPACITY] = {0};
        uint64_t kinds[CIL_EMU_STACK_ValueKinds_CAPACITY] = {0};
        struct cil_emu_state state = {0};
        state.stack_EvaluationStack.values = values;
        state.stack_ValueKinds.values = kinds;
        assert(cil_emu_execute_state("LDC_I4_M1", NULL, 0, &state) == CIL_EMU_OK);
        assert(cil_emu_execute_state("DUP", NULL, 0, &state) == CIL_EMU_OK);
        assert(state.stack_EvaluationStack.size == 2);
        assert(state.stack_ValueKinds.size == 2);
        assert(values[0] == UINT64_C(0xffffffff));
        assert(values[1] == UINT64_C(0xffffffff));
        assert(kinds[0] == 1 && kinds[1] == 1);
        assert(cil_emu_execute_state("POP", NULL, 0, &state) == CIL_EMU_OK);
        assert(state.stack_EvaluationStack.size == 1);
        assert(state.stack_ValueKinds.size == 1);
    }
"""
        with tempfile.TemporaryDirectory() as directory:
            directory = Path(directory)
            source = directory / "stdspec-state.c"
            source.write_text(
                "\n".join(generated)
                + "\n#include <assert.h>\nint main(void) {\n"
                + checks
                + "    return 0;\n}\n",
                encoding="ascii",
            )
            executable = directory / "stdspec-state"
            subprocess.run(
                [*command, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 source, "-o", executable],
                check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
