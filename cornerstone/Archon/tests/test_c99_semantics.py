"""Compare standalone C99 SemIR instruction bodies with the Python interpreter."""

import itertools
import os
from pathlib import Path
import random
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.c99_semantics import lower_instruction
from archon.diagnostics import ArchonError, Location
from archon.semantics import compile_semantics, execute_semantics


_FULL_MASK = (1 << 64) - 1


def _reg(name, direction):
    return {"name": name, "kind": "reg", "width": 3, "signed": False, "direction": direction}


def _imm(name, width, signed):
    return {"name": name, "kind": "imm", "width": width, "signed": signed, "direction": "in"}


def _instruction(source, operands, width):
    return {
        "name": "TEST",
        "operands": operands,
        "semantics": compile_semantics(source, Location("test.arch", 1, 1), operands, width, {}),
    }


def _c_array(values):
    elements = ", ".join(f"UINT64_C(0x{value & _FULL_MASK:x})" for value in values)
    return f"(const uint64_t[]){{{elements}}}" if elements else "NULL"


class C99Semantics(unittest.TestCase):
    def compile_and_compare(self, variants):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        definitions, calls = [], []
        for index, (inst, width, zero, cases) in enumerate(variants):
            definitions.append(lower_instruction(inst, "test", index, width, zero))
            names = [op["name"] for op in inst["operands"]]
            for values, registers in cases:
                args = dict(zip(names, values))
                try:
                    expected = execute_semantics(inst["semantics"], args, registers, width, zero)
                except ArchonError as exc:
                    self.assertIn("conflicting parallel writes", str(exc))
                    expected = None
                scratch = list(registers)
                if zero is not None:
                    scratch[zero] = 0
                calls.append(
                    f"    check(test_emu_sem_{index}, {_c_array(values)}, {_c_array(scratch)},"
                    f" {len(scratch)}, {_c_array(expected or [])});"
                )
        code = """
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum test_emu_status { TEST_EMU_OK, TEST_EMU_CONFLICTING_WRITES };
""" + "\n".join(definitions) + """
static void check(enum test_emu_status (*exec)(const uint64_t *, uint64_t *),
        const uint64_t *args, const uint64_t *initial, size_t count, const uint64_t *expected) {
    uint64_t scratch[8];
    enum test_emu_status status;
    assert(count <= sizeof scratch / sizeof *scratch);
    memcpy(scratch, initial, count * sizeof *scratch);
    status = exec(args, scratch);
    if (expected) {
        assert(status == TEST_EMU_OK);
        assert(memcmp(scratch, expected, count * sizeof *scratch) == 0);
    } else {
        assert(status == TEST_EMU_CONFLICTING_WRITES);
    }
}

int main(void) {
""" + "\n".join(calls) + "\n    return 0;\n}\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "test.c").write_text(code, encoding="ascii")
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "test.c", "-o", "test"],
                cwd=root,
                check=True,
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)

    def test_arithmetic_and_bit_operations_against_python(self):
        rng = random.Random(13)
        variants = []
        for width in (1, 2, 7, 8, 31, 32, 63, 64):
            mask = (1 << width) - 1
            small = min(width, 4)
            rotate = (f"concat(extract(regs, 0, 0), extract(regs, {width - 1}, 1))"
                      if width > 1 else "extract(regs, 0, 0)")
            inst = _instruction(
                f"""
                let regs = read(src);
                let v0 = ~((regs +% 1) *% (regs -% 1));
                let a = sext<{width}>(displacement);
                let b = zext<{width}>(displacement);
                logical := ((v0 & regs) << amount) | ((v0 ^ regs) >> amount);
                signed_shifted := a >> amount;
                unsigned_shifted := b >> amount;
                rotated := {rotate};
                shrunk := zext<{width}>(trunc<1>(extract(regs, {width - 1}, 0)));
                """,
                [_reg("src", "in"), _reg("logical", "out"),
                 _reg("signed_shifted", "out"), _reg("unsigned_shifted", "out"),
                 _reg("rotated", "out"), _reg("shrunk", "out"), _imm("amount", 64, False),
                 _imm("displacement", small, True)],
                width,
            )
            cases = []
            for amount, offset in itertools.product(
                sorted({0, 1, width - 1, width, width + 1, _FULL_MASK}),
                sorted({-(1 << (small - 1)), -1, 0, (1 << (small - 1)) - 1}),
            ):
                for original in (0, 1 << (width - 1), mask, rng.getrandbits(width)):
                    cases.append(([0, 1, 2, 3, 4, 5, amount, offset], [original, mask, 0, mask, 0, mask]))
            variants.append((inst, width, None, cases))
        self.compile_and_compare(variants)

    def test_parallel_aliasing_sequences_and_lexical_scope(self):
        operands = [_reg("left", "inout"), _reg("right", "inout"), _reg("dst", "out")]
        variants = []
        for source in (
            """
            let used = 1;
            sequence {
                let v0 = read(left);
                left := v0 +% used;
                parallel {
                    let slot = read(right);
                    right := read(left);
                    left := slot;
                    dst := read(left) +% read(right);
                }
            }
            sequence {
                let v0 = read(left);
                dst := v0 +% read(right);
            }
            """,
            "parallel { left := 0xff +% 2; right := 1; left := 1; }",
            "parallel { left := 0xff; left := 1; }",
        ):
            for zero in (None, 0, 2):
                cases = [(args, [99, 2, 255]) for args in itertools.product(range(3), repeat=3)]
                variants.append((_instruction(source, operands, 8), 8, zero, cases))
        self.compile_and_compare(variants)

    def test_unused_bindings_and_signed_constants(self):
        inst = _instruction("dst := sext<64>(imm);", [_reg("dst", "out"), _imm("imm", 1, True)], 64)
        # The ArchDB wire format permits negative signed literals, although
        # the frontend does not generate them.
        expr = inst["semantics"]["statements"][0]["value"]["value"]
        expr["op"], expr["value"] = "constant", -1
        del expr["name"]
        self.compile_and_compare([
            (_instruction("", [], 8), 8, None, [([], [3])]),
            (_instruction("parallel { let tmp = ~1; } sequence { let tmp = 3; }", [], 8),
             8, 0, [([], [3])]),
            (inst, 64, None, [([0, 0], [3])]),
        ])

    def test_reject_wide_registers_and_intermediate_values(self):
        for source, width, message in (
            ("", 65, "register width 65 exceeds 64 bits"),
            ("let v = trunc<64>(zext<65>(1));", 64, "SemIR 'zext' width 65 exceeds 64 bits"),
            ("let v = concat(1, 1);", 64, "SemIR 'concat' width 128 exceeds 64 bits"),
        ):
            with self.subTest(source=source, width=width), self.assertRaisesRegex(ArchonError, message):
                lower_instruction(_instruction(source, [], width), "test", 0, width, None)


if __name__ == "__main__":
    unittest.main()
