"""Exercise the generated codec with no Python runtime at execution time."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.c99 import generate_codec
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"


class C99Codec(unittest.TestCase):
    def compiler(self):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        return cc

    def compile_and_run(self, cc, root):
        subprocess.run(
            [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic", "test.c", "-o", "test"],
            cwd=root,
            check=True,
        )
        subprocess.run([str(root / "test")], check=True)

    def test_invalid_prefix(self):
        db = compile_file(STDSPEC / "riscv" / "rv32i.arch")
        for prefix in ("", "not-valid", "_private", "a\n#error"):
            with self.subTest(prefix=prefix), self.assertRaises(ArchonError):
                generate_codec(db, prefix)

    def test_standalone_codec(self):
        cc = self.compiler()
        rv32 = compile_file(STDSPEC / "riscv" / "rv32i.arch")
        mips = compile_file(STDSPEC / "mips32" / "root.arch")
        mips["endian"] = "big"  # Exercise the codec's other byte order with a known word.
        empty = compile_file(STDSPEC / "riscv" / "rv32i.arch", features=[])
        word = (0x012A4021).to_bytes(4, mips["endian"])
        name, operands = Target(mips).decode(word)
        mips_inst = next(inst for inst in mips["instructions"] if inst["name"] == name)
        numbered = ", ".join(str(operands[op["name"]]) for op in mips_inst["operands"])
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            for db, prefix in ((rv32, "rv"), (mips, "mips"), (empty, "empty")):
                (root / f"{prefix}.c").write_text(generate_codec(db, prefix), encoding="ascii")
            (root / "test.c").write_text(
                f"""#include "rv.c"
#include "mips.c"
#include "empty.c"
#include <assert.h>

int main(void) {{
    uint8_t out[4] = {{0}};
    const uint8_t expected[] = {{0x93, 0x00, 0xf0, 0xff}};
    const uint8_t wrong[] = {{0xff, 0xff, 0xff, 0xff}};
    const uint64_t args[] = {{1, 0, UINT64_MAX}};
    const uint64_t bad_args[] = {{1, 0, 4095}};
    const uint64_t mips_args[] = {{{numbered}}};
    const uint8_t mips_expected[] = {{{", ".join(map(str, word))}}};
    size_t size = 0, count = 0;
    uint64_t decoded[3] = {{0, 0, 0}};
    const char *inst = NULL;

    assert(rv_encode("ADDI", args, 3, out, 3, &size) == RV_BUFFER_TOO_SMALL);
    assert(out[0] == 0 && size == 0);
    assert(rv_encode("ADDI", bad_args, 3, out, 4, &size) == RV_OPERAND_RANGE);
    assert(out[0] == 0 && size == 0);
    assert(rv_encode("ADDI", args, 3, out, 4, &size) == RV_OK);
    assert(size == 4 && memcmp(out, expected, 4) == 0);
    assert(rv_decode(wrong, 4, &inst, decoded, 3, &count) == RV_UNKNOWN_INSTRUCTION);
    assert(inst == NULL && count == 0 && decoded[0] == 0);
    assert(rv_decode(out, 4, &inst, decoded, 1, &count) == RV_BUFFER_TOO_SMALL);
    assert(rv_decode(out, 4, &inst, decoded, 3, &count) == RV_OK);
    assert(count == 3 && strcmp(inst, "ADDI") == 0);
    for (size_t i = 0; i < 3; ++i)
        assert(decoded[i] == args[i]);

    assert(mips_encode("{name}", mips_args, 3, out, 4, &size) == MIPS_OK);
    assert(size == 4 && memcmp(out, mips_expected, 4) == 0);
    assert(mips_decode(out, 4, &inst, decoded, 3, &count) == MIPS_OK);
    assert(count == 3 && strcmp(inst, "{name}") == 0);
    for (size_t i = 0; i < 3; ++i)
        assert(decoded[i] == mips_args[i]);
    assert(empty_decode(out, 4, &inst, decoded, 3, &count) == EMPTY_UNKNOWN_INSTRUCTION);
    return 0;
}}
""",
                encoding="ascii",
            )
            self.compile_and_run(cc, root)

    def test_wasm_and_amd64_instruction_widths(self):
        cc = self.compiler()
        for spec, asm, expected in (
            ("wasm", "i32.add", b"\x6a"),
            ("amd64", "mov rax, rbx", b"\x48\x89\xd8"),
        ):
            with self.subTest(spec=spec), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                db = compile_file(STDSPEC / spec / "root.arch")
                name, values = Target(db).assemble(asm)
                instruction = next(inst for inst in db["instructions"] if inst["name"] == name)
                args = [values[op["name"]] for op in instruction["operands"]]
                data_c = ", ".join(map(str, expected))
                args_c = ", ".join(map(str, args)) or "0"
                code = generate_codec(db) + f"""
#include <assert.h>
int main(void) {{
    const uint8_t data[] = {{{data_c}}};
    const uint64_t args[] = {{{args_c}}};
    uint64_t decoded[{max(1, len(args))}] = {{0}};
    uint8_t encoded[sizeof data] = {{0}};
    const char *name = NULL;
    size_t count = 0, size = 0;
    assert(archon_encode("{name}", args, {len(args)}, encoded, sizeof encoded, &size) == ARCHON_OK);
    assert(size == sizeof data && memcmp(encoded, data, size) == 0);
    assert(archon_decode(data, sizeof data, &name, decoded, {len(args)}, &count) == ARCHON_OK);
    assert(count == {len(args)} && strcmp(name, "{name}") == 0);
    assert(memcmp(decoded, args, count * sizeof *args) == 0);
    return 0;
}}
"""
                (root / "test.c").write_text(code, encoding="ascii")
                self.compile_and_run(cc, root)


if __name__ == "__main__":
    unittest.main()
