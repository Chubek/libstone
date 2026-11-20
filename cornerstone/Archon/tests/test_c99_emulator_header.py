"""Check the standalone emulator ABI and generation-time validation."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.c99_emulator import generate_emulator, generate_emulator_header
from archon.compiler import compile_file
from archon.diagnostics import ArchonError, Location
from archon.semantics import compile_semantics


TOY8 = Path(__file__).resolve().parents[1] / "examples" / "toy8.arch"


class C99EmulatorHeader(unittest.TestCase):
    def test_public_header_links_separately_and_empty_target_handles_input(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for prefix, options in (("toy", {}), ("empty", {"features": []})):
                db = compile_file(TOY8, **options)
                (root / f"{prefix}.h").write_text(generate_emulator_header(db, prefix), encoding="ascii")
                (root / f"{prefix}.c").write_text(generate_emulator(db, prefix), encoding="ascii")

            (root / "main.c").write_text(
                """#include "toy.h"
#include "empty.h"
#include "toy.h"
#include <assert.h>

int main(void) {
    uint64_t regs[8] = {99};
    struct toy_emu_state state = {0};
    const uint64_t operands[] = {1, UINT64_MAX};
    uint8_t data[4] = {0};
    size_t size = 0, consumed = 123;

    assert(toy_emu_register_count() == 8 && toy_emu_register_width() == 8);
    assert(TOY_EMU_REGISTER_COUNT == 8 && TOY_EMU_REGISTER_WIDTH == 8);
    state.registers = regs;
    assert(toy_encode("MOVI", operands, 2, data, sizeof data, &size) == TOY_OK);
    assert(size == 2);
    assert(toy_emu_step(NULL, 0, regs, 8) == TOY_EMU_TRUNCATED_INSTRUCTION);
    assert(toy_emu_step(NULL, size, regs, 8) == TOY_EMU_INVALID_ARGUMENT);
    assert(toy_emu_step(data, sizeof data, regs, 8) == TOY_EMU_INVALID_ARGUMENT);
    assert(toy_emu_run(NULL, size, regs, 8, &consumed) == TOY_EMU_INVALID_ARGUMENT);
    assert(consumed == 0 && regs[0] == 99);
    assert(toy_emu_step(data, size, regs, 8) == TOY_EMU_OK);
    assert(regs[0] == 0 && regs[1] == 255);
    regs[1] = 0;
    assert(toy_emu_step_state(data, size, &state) == TOY_EMU_OK);
    assert(regs[0] == 0 && regs[1] == 255);

    regs[0] = 77;
    assert(empty_emu_execute("MOVI", operands, 2, regs, 8) == EMPTY_EMU_UNKNOWN_INSTRUCTION);
    assert(empty_emu_step(data, size, regs, 8) == EMPTY_EMU_UNKNOWN_INSTRUCTION);
    assert(empty_emu_run(data, size, regs, 8, &consumed) == EMPTY_EMU_UNKNOWN_INSTRUCTION);
    assert(consumed == 0 && regs[0] == 77);
    assert(empty_emu_run(NULL, 0, regs, 8, &consumed) == EMPTY_EMU_OK);
    assert(consumed == 0 && regs[0] == 77);
    return 0;
}
""",
                encoding="ascii",
            )
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 "toy.c", "empty.c", "main.c", "-o", "main"],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "main")], check=True)

    def test_requires_valid_semir_and_rejects_wide_intermediates(self):
        db = compile_file(TOY8)
        inst = next(inst for inst in db["instructions"] if inst["name"] == "MOVI")
        inst["semantics"]["effects"]["writes"] = []
        for generate in (generate_emulator, generate_emulator_header):
            with self.subTest(generate=generate), self.assertRaisesRegex(ArchonError, "effects do not match"):
                generate(db)

        inst["semantics"] = compile_semantics(
            "dst := trunc<8>(zext<65>(value));",
            Location("wide.arch", 1, 1),
            inst["operands"],
            8,
            {},
        )
        for generate in (generate_emulator, generate_emulator_header):
            with self.subTest(generate=generate), self.assertRaisesRegex(ArchonError, "up to 64 bits"):
                generate(db)

    def test_rejects_state_values_wider_than_the_c_abi(self):
        for category, row, message in (
            (
                "scalars",
                {"name": "Wide", "type": {"kind": "bits", "width": 65}},
                "scalar state 'Wide'.*at most 64 bits",
            ),
            (
                "stacks",
                {
                    "name": "Wide",
                    "element_type": {"kind": "bits", "width": 65},
                    "capacity": 1,
                },
                "stack 'Wide'.*at most 64 bits",
            ),
        ):
            db = compile_file(TOY8, features=[])
            db["state"][category].append(row)
            for generate in (generate_emulator, generate_emulator_header):
                with self.subTest(category=category, generate=generate), self.assertRaisesRegex(
                    ArchonError, message,
                ):
                    generate(db)

    def test_registerless_semantic_word_width_is_checked_even_without_expressions(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "wide.arch"
            source.write_text("""
architecture Wide {
    endian = little;
    address_width = 65;
    format Op: bits<8> { field opcode: bits<8> @ [7:0]; }
    instruction NOP {
        encoding Op { opcode = 0; }
        assembly "nop";
        semantics {}
    }
}
""", encoding="ascii")
            db = compile_file(source)
        for generate in (generate_emulator, generate_emulator_header):
            with self.subTest(generate=generate), self.assertRaisesRegex(
                ArchonError, "semantic widths of up to 64 bits in 'NOP'",
            ):
                generate(db)


if __name__ == "__main__":
    unittest.main()
