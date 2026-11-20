"""Exercise generated C99 scalar, stack, and memory state transactionally."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from archon import database
from archon.c99_emulator import generate_emulator, generate_emulator_header
from archon.compiler import compile_file
from archon.runtime import Target


ROOT = Path(__file__).resolve().parents[1]


STATE_ARCH = r"""
architecture NativeState {
    endian = little;
    address_width = 8;

    state PC: uint<8>;
    state Flags: uint<8>;
    stack Data: uint<8> { capacity = 2; }
    memory RAM { size = 8; }

    format I: bits<32> {
        field opcode: bits<8> @ [31:24];
        field value: uint<8> @ [23:16];
        field other: uint<8> @ [15:8];
        field address: uint<8> @ [7:0];
    }

    instruction MUTATE {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 1; value = value; other = other; address = address; }
        assembly "mutate {value}, {other}, {address}";
        semantics {
            PC := value;
            Flags := other;
            push(Data, value);
            store<uint<8>>(RAM, address, other);
        }
    }

    instruction POP_LOAD {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 2; value = value; other = other; address = address; }
        assembly "pop_load {value}, {other}, {address}";
        semantics {
            let top = pop(Data);
            PC := load<uint<8>>(RAM, address);
            Flags := top;
        }
    }

    instruction FAIL_POP {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 3; value = value; other = other; address = address; }
        assembly "fail_pop {value}, {other}, {address}";
        semantics {
            PC := value;
            store<uint<8>>(RAM, address, other);
            let missing = pop(Data);
            Flags := missing;
        }
    }

    instruction FAIL_STORE {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 4; value = value; other = other; address = address; }
        assembly "fail_store {value}, {other}, {address}";
        semantics {
            PC := value;
            push(Data, other);
            store<uint<8>>(RAM, address, value);
        }
    }

    instruction PARALLEL {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 5; value = value; other = other; address = address; }
        assembly "parallel {value}, {other}, {address}";
        semantics {
            parallel {
                PC := value;
                PC := other;
                store<uint<8>>(RAM, address, value);
                store<uint<8>>(RAM, address, other);
            }
        }
    }

    instruction NOOP {
        operands {
            in value: imm<uint<8>>;
            in other: imm<uint<8>>;
            in address: imm<uint<8>>;
        }
        encoding I { opcode = 6; value = value; other = other; address = address; }
        assembly "noop {value}, {other}, {address}";
        semantics {}
    }
}
"""

WIDE_STATE_ARCH = r"""
architecture WideState {
    endian = ENDIAN;
    address_width = 16;
    type Word = bits<16>;
    state PC: Word;
    state Carry: bits<1>;
    stack Values: Word { capacity = 3; }
    memory RAM { size = 8; }

    format Op: bits<8> { field opcode: bits<8> @ [7:0]; }

    instruction MIX {
        encoding Op { opcode = 1; }
        assembly "mix";
        semantics {
            let rhs = pop(Values);
            let lhs = pop(Values);
            let word = load<bits<16>>(RAM, read_state(PC));
            push(Values, lhs +% rhs +% word);
            store<bits<16>>(RAM, read_state(PC), rhs);
            parallel {
                PC := read_state(PC) +% 2;
                Carry := extract(rhs, 0, 0);
                store<bits<16>>(RAM, 4, lhs);
            }
        }
    }
    instruction DUP_SECOND {
        encoding Op { opcode = 2; }
        assembly "dup.second";
        semantics { push(Values, peek(Values, 1)); }
    }
    instruction FAIL {
        encoding Op { opcode = 3; }
        assembly "fail";
        semantics {
            PC := read_state(PC) +% 2;
            let value = pop(Values);
            store<bits<16>>(RAM, 7, value);
        }
    }
    instruction CONFLICT {
        encoding Op { opcode = 4; }
        assembly "conflict";
        semantics {
            parallel {
                store<bits<16>>(RAM, 0, 0x0102);
                store<bits<16>>(RAM, 1, 0x0304);
            }
        }
    }
}
"""


def _octets(data):
    return ", ".join(f"0x{value:02x}" for value in data)


class C99ExtendedState(unittest.TestCase):
    def test_cli_describes_registerless_state_and_stateful_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "state.arch"
            archdb = root / "state.archdb"
            source.write_text(STATE_ARCH, encoding="ascii")
            database.save(compile_file(source), archdb)

            inspected = subprocess.run(
                [sys.executable, "-m", "archon", "inspect", archdb],
                cwd=ROOT,
                capture_output=True,
                check=True,
                text=True,
            ).stdout
            self.assertIn("Registers: (none)", inspected)
            self.assertIn(
                "Non-register state: 2 scalar(s), 1 stack(s), 1 memory space(s)",
                inspected,
            )

            help_text = subprocess.run(
                [sys.executable, "-m", "archon", "gen", "--help"],
                cwd=ROOT,
                capture_output=True,
                check=True,
                text=True,
            ).stdout
            self.assertIn("transactional architectural-state emulator", help_text)

    def test_state_api_effects_failures_and_run_prefix(self):
        compilers = [compiler for compiler in ("gcc", "clang") if shutil.which(compiler)]
        if not compilers:
            self.skipTest("GCC and Clang are unavailable")

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "state.arch"
            source.write_text(STATE_ARCH, encoding="ascii")
            db = compile_file(source)
            target = Target(db)
            generated = root / "native.c"
            header = root / "native.h"
            generated.write_text(generate_emulator(db, "ns"), encoding="ascii")
            header.write_text(generate_emulator_header(db, "ns"), encoding="ascii")

            good = target.assemble_program("mutate 3, 4, 2")
            failed = target.assemble_program("fail_store 8, 6, 8")
            pop_load = target.assemble_program("pop_load 0, 0, 2")
            truncated = good + failed[:1]
            harness = root / "test.c"
            harness.write_text(
                f"""#include "native.h"
#include "native.h"
#include <assert.h>
#include <string.h>

int main(void) {{
    const uint8_t good[] = {{{_octets(good)}}};
    const uint8_t run_code[] = {{{_octets(good + failed)}}};
    const uint8_t pop_load[] = {{{_octets(pop_load)}}};
    const uint8_t truncated[] = {{{_octets(truncated)}}};
    const uint64_t mutate[] = {{3, 4, 2}};
    const uint64_t mutate_two[] = {{5, 6, 3}};
    const uint64_t overflow[] = {{7, 8, 4}};
    const uint64_t fail_pop[] = {{9, 7, 1}};
    const uint64_t fail_store[] = {{8, 6, 8}};
    const uint64_t same[] = {{5, 5, 3}};
    const uint64_t conflict[] = {{5, 6, 3}};
    uint64_t stack[NS_EMU_STACK_Data_CAPACITY] = {{0}};
    uint8_t memory[NS_EMU_MEMORY_RAM_SIZE] = {{0}};
    struct ns_emu_state state = {{0}};
    size_t consumed = 99;

    assert(NS_EMU_REGISTER_COUNT == 0);
    assert(NS_EMU_SCALAR_PC_WIDTH == 8);
    assert(NS_EMU_STACK_Data_WIDTH == 8);
    assert(NS_EMU_STACK_Data_CAPACITY == 2);
    assert(NS_EMU_MEMORY_RAM_SIZE == 8);
    assert(ns_emu_register_count() == 0 && ns_emu_register_width() == 0);

    state.stack_Data.values = stack;
    state.memory_RAM = memory;
    state.scalar_PC = 1;
    state.scalar_Flags = 2;

    /* The legacy ABI remains useful for state-independent instructions. */
    assert(ns_emu_execute("NOOP", mutate, 3, NULL, 0) == NS_EMU_OK);
    assert(ns_emu_execute("MUTATE", mutate, 3, NULL, 0) == NS_EMU_STATE_REQUIRED);

    /* Scalar and memory writes before a failing pop are rolled back. */
    assert(ns_emu_execute_state("FAIL_POP", fail_pop, 3, &state)
            == NS_EMU_STACK_UNDERFLOW);
    assert(state.scalar_PC == 1 && state.scalar_Flags == 2);
    assert(state.stack_Data.size == 0 && memory[1] == 0);

    /* A stream commits its successful prefix, not its failing instruction. */
    assert(ns_emu_run_state(run_code, sizeof run_code, &state, &consumed)
            == NS_EMU_MEMORY_OUT_OF_BOUNDS);
    assert(consumed == sizeof good);
    assert(state.scalar_PC == 3 && state.scalar_Flags == 4);
    assert(state.stack_Data.size == 1 && stack[0] == 3 && memory[2] == 4);
    assert(memory[6] == 0);

    assert(ns_emu_step_state(pop_load, sizeof pop_load, &state) == NS_EMU_OK);
    assert(state.scalar_PC == 4 && state.scalar_Flags == 3);
    assert(state.stack_Data.size == 0);

    /* A push before an out-of-bounds store is also rolled back. */
    assert(ns_emu_execute_state("FAIL_STORE", fail_store, 3, &state)
            == NS_EMU_MEMORY_OUT_OF_BOUNDS);
    assert(state.scalar_PC == 4 && state.scalar_Flags == 3);
    assert(state.stack_Data.size == 0);

    assert(ns_emu_execute_state("MUTATE", mutate, 3, &state) == NS_EMU_OK);
    assert(ns_emu_execute_state("MUTATE", mutate_two, 3, &state) == NS_EMU_OK);
    assert(state.stack_Data.size == 2);
    assert(ns_emu_execute_state("MUTATE", overflow, 3, &state)
            == NS_EMU_STACK_OVERFLOW);
    assert(state.scalar_PC == 5 && state.scalar_Flags == 6);
    assert(state.stack_Data.size == 2 && memory[4] == 0);

    /* Equal parallel aliases coalesce; differing aliases roll back. */
    assert(ns_emu_execute_state("PARALLEL", same, 3, &state) == NS_EMU_OK);
    assert(state.scalar_PC == 5 && memory[3] == 5);
    assert(ns_emu_execute_state("PARALLEL", conflict, 3, &state)
            == NS_EMU_CONFLICTING_WRITES);
    assert(state.scalar_PC == 5 && memory[3] == 5);

    /* Truncation follows one committed instruction and reports that prefix. */
    state.stack_Data.size = 0;
    memset(stack, 0, sizeof stack);
    memset(memory, 0, sizeof memory);
    assert(ns_emu_run_state(truncated, sizeof truncated, &state, &consumed)
            == NS_EMU_TRUNCATED_INSTRUCTION);
    assert(consumed == sizeof good && state.scalar_PC == 3);
    assert(state.stack_Data.size == 1 && memory[2] == 4);

    /* Invalid caller state is rejected before any transaction starts. */
    state.scalar_PC = 256;
    assert(ns_emu_step_state(good, sizeof good, &state) == NS_EMU_INVALID_ARGUMENT);
    assert(state.scalar_PC == 256 && state.stack_Data.size == 1);
    state.scalar_PC = 3;
    stack[0] = 256;
    assert(ns_emu_step_state(good, sizeof good, &state) == NS_EMU_INVALID_ARGUMENT);
    assert(stack[0] == 256);
    stack[0] = 3;
    state.memory_RAM = NULL;
    assert(ns_emu_execute_state("NOOP", mutate, 3, &state)
            == NS_EMU_INVALID_ARGUMENT);
    state.memory_RAM = memory;

    /* Named execution and encoded stepping agree on successful effects. */
    state.stack_Data.size = 0;
    assert(ns_emu_execute_state("MUTATE", mutate, 3, &state) == NS_EMU_OK);
    state.stack_Data.size = 0;
    assert(ns_emu_step_state(good, sizeof good, &state) == NS_EMU_OK);
    assert(state.scalar_PC == 3 && state.scalar_Flags == 4);
    return 0;
}}
""",
                encoding="ascii",
            )

            for compiler in compilers:
                with self.subTest(compiler=compiler):
                    executable = root / f"test-{compiler}"
                    subprocess.run(
                        [
                            compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "native.c", "test.c", "-o", executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run([executable], cwd=root, check=True)

            allocation_harness = root / "allocation.c"
            allocation_harness.write_text(
                """#include <stddef.h>
void *always_fail(size_t);
#define malloc always_fail
#include "native.c"
#undef malloc
#include <assert.h>

void *always_fail(size_t size) {
    (void)size;
    return NULL;
}

int main(void) {
    const uint64_t mutate[] = {3, 4, 2};
    uint64_t stack[NS_EMU_STACK_Data_CAPACITY] = {11, 0};
    uint8_t memory[NS_EMU_MEMORY_RAM_SIZE] = {12};
    struct ns_emu_state state = {0};
    state.scalar_PC = 9;
    state.scalar_Flags = 10;
    state.stack_Data.values = stack;
    state.stack_Data.size = 1;
    state.memory_RAM = memory;
    assert(ns_emu_execute_state("MUTATE", mutate, 3, &state)
            == NS_EMU_ALLOCATION_FAILURE);
    assert(state.scalar_PC == 9 && state.scalar_Flags == 10);
    assert(state.stack_Data.size == 1 && stack[0] == 11);
    assert(memory[0] == 12);
    return 0;
}
""",
                encoding="ascii",
            )
            for compiler in compilers:
                with self.subTest(compiler=compiler, allocation_failure=True):
                    executable = root / f"allocation-{compiler}"
                    subprocess.run(
                        [
                            compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                            "allocation.c", "-o", executable,
                        ],
                        cwd=root,
                        check=True,
                    )
                    subprocess.run([executable], cwd=root, check=True)

    def test_multibyte_memory_endianness_peek_and_parallel_overlap(self):
        compilers = [compiler for compiler in ("gcc", "clang") if shutil.which(compiler)]
        if not compilers:
            self.skipTest("GCC and Clang are unavailable")
        for endian, result, expected_memory in (
            ("little", 543, (0x14, 0, 0, 0, 0x0A, 0, 0, 0)),
            ("big", 288, (0, 0x14, 0, 0, 0, 0x0A, 0, 0)),
        ):
            with self.subTest(endian=endian), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                source = root / "wide.arch"
                source.write_text(
                    WIDE_STATE_ARCH.replace("ENDIAN", endian), encoding="ascii",
                )
                db = compile_file(source)
                generated = root / "wide.c"
                header = root / "wide.h"
                generated.write_text(generate_emulator(db, "wide"), encoding="ascii")
                header.write_text(generate_emulator_header(db, "wide"), encoding="ascii")
                memory = ", ".join(map(str, expected_memory))
                (root / "test.c").write_text(
                    f"""#include "wide.h"
#include <assert.h>
#include <string.h>

int main(void) {{
    uint64_t values[WIDE_EMU_STACK_Values_CAPACITY] = {{10, 20, 0}};
    uint8_t ram[WIDE_EMU_MEMORY_RAM_SIZE] = {{1, 2, 0, 0, 0, 0, 0, 0}};
    const uint8_t expected[WIDE_EMU_MEMORY_RAM_SIZE] = {{{memory}}};
    struct wide_emu_state state = {{0}};
    state.scalar_PC = 0;
    state.stack_Values.values = values;
    state.stack_Values.size = 2;
    state.memory_RAM = ram;

    assert(wide_emu_execute_state("MIX", NULL, 0, &state) == WIDE_EMU_OK);
    assert(state.scalar_PC == 2 && state.scalar_Carry == 0);
    assert(state.stack_Values.size == 1 && values[0] == {result});
    assert(memcmp(ram, expected, sizeof ram) == 0);

    values[0] = 3;
    values[1] = 5;
    state.stack_Values.size = 2;
    assert(wide_emu_execute_state("DUP_SECOND", NULL, 0, &state) == WIDE_EMU_OK);
    assert(state.stack_Values.size == 3 && values[2] == 3);

    memset(ram, 0, sizeof ram);
    state.scalar_PC = 1;
    values[0] = 0xbeef;
    state.stack_Values.size = 1;
    assert(wide_emu_execute_state("FAIL", NULL, 0, &state)
            == WIDE_EMU_MEMORY_OUT_OF_BOUNDS);
    assert(state.scalar_PC == 1 && state.stack_Values.size == 1);
    assert(values[0] == 0xbeef && ram[0] == 0);

    assert(wide_emu_execute_state("CONFLICT", NULL, 0, &state)
            == WIDE_EMU_CONFLICTING_WRITES);
    for (size_t i = 0; i < sizeof ram; ++i)
        assert(ram[i] == 0);
    return 0;
}}
""",
                    encoding="ascii",
                )
                for compiler in compilers:
                    with self.subTest(endian=endian, compiler=compiler):
                        executable = root / f"wide-{compiler}"
                        subprocess.run(
                            [
                                compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                                "-pedantic", "wide.c", "test.c", "-o", executable,
                            ],
                            cwd=root,
                            check=True,
                        )
                        subprocess.run([executable], cwd=root, check=True)

if __name__ == "__main__":
    unittest.main()
