"""Exercise bounded register operands independently of the AMD64 encodings."""

from copy import deepcopy
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon import database
from archon.c99 import generate_codec
from archon.c99_emulator import generate_emulator, generate_emulator_header
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.irgen import emitter, isel
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
SOURCE = """\
architecture Windowed {
    address_width = 8;
    endian = little;
    register_file Registers {
        count = 16;
        width = 8;
        register r[0..15];
        alias low = r[1];
        alias first = r[2];
        alias last = r[5];
        alias high = r[6];
    }
    register_class Window { members = Registers.[2..5]; }
    register_class Pair { members = Registers.[1..2]; }
    register_class All { members = Registers.*; }
    format Narrow: bits<16> {
        field opcode: bits<10> @ [15:6];
        field dst: bits<3> @ [5:3];
        field src: bits<3> @ [2:0];
    }
    format Wide: bits<16> {
        field opcode: bits<8> @ [15:8];
        field dst: bits<4> @ [7:4];
        field imm: uint<4> @ [3:0];
    }
    instruction SCOPED {
        operands { out dst: Window; in src: Pair; }
        encoding Narrow { opcode = 1; dst = dst; src = src; }
        assembly "scoped {dst}, {src}";
        semantics { write(dst, read(src)); }
    }
    instruction WIDE {
        operands { inout dst: All; in imm: imm<uint<4>>; }
        encoding Wide { opcode = 0xff; dst = dst; imm = imm; }
        assembly "wide {dst}, {imm}";
        semantics { write(dst, read(dst) +% zext<8>(imm)); }
    }
}
"""


def _compile(source):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "windowed.arch"
        path.write_text(source, encoding="utf-8")
        return compile_file(path)


class RegisterClassRangesTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = _compile(SOURCE)

    def test_round_trip_and_raw_field_indices(self):
        text = database.dumps(self.db)
        loaded = database.loads(text)
        self.assertEqual(database.dumps(loaded), text)
        scoped, wide = loaded["instructions"]
        self.assertEqual([operand["register_range"] for operand in scoped["operands"]],
                         [[2, 5], [1, 2]])
        self.assertNotIn("register_range", wide["operands"][0])
        target = Target(loaded)
        for dst in range(8):
            for src in range(8):
                operands = {"dst": dst, "src": src}
                encoded = bytes((0x40 | (dst << 3) | src, 0))
                with self.subTest(operands=operands):
                    if 2 <= dst <= 5 and 1 <= src <= 2:
                        self.assertEqual(target.decode(encoded), ("SCOPED", operands))
                        self.assertEqual(target.encode("SCOPED", operands), encoded)
                        continue
                    with self.assertRaisesRegex(ArchonError, "register class range"):
                        target.decode(encoded)
                    with self.assertRaisesRegex(ArchonError, "register class range"):
                        target.encode("SCOPED", operands)
        self.assertEqual(target.assemble("scoped first, low"), ("SCOPED", {"dst": 2, "src": 1}))
        self.assertEqual(target.assemble("scoped last, first"), ("SCOPED", {"dst": 5, "src": 2}))
        state = target.new_state(registers=list(range(16)))
        for text in ("scoped low, r2", "scoped high, r2", "scoped r2, last"):
            with self.subTest(text=text), self.assertRaisesRegex(ArchonError, "register class range"):
                target.assemble(text)
        with self.assertRaisesRegex(ArchonError, "register class range"):
            target.execute_state("SCOPED", {"dst": 6, "src": 1}, state)
        self.assertEqual(state.registers, list(range(16)))
        self.assertEqual(target.encode(*target.assemble("wide r15, 3")), bytes.fromhex("f3ff"))

    def test_source_rejects_invalid_bounds_and_fields(self):
        for scope, message in (
            ("[-1..5]", r"ordered range within 0\.\.15"),
            ("[5..2]", r"ordered range within 0\.\.15"),
            ("[2..16]", r"ordered range within 0\.\.15"),
            ("[2..8]", r"cannot encode register operand 'dst'"),
            ("[8..15]", r"cannot encode register operand 'dst'"),
            ("*", r"cannot encode register operand 'dst'"),
        ):
            with self.subTest(scope=scope), self.assertRaisesRegex(ArchonError, message):
                _compile(SOURCE.replace("[2..5]", scope))

    def test_mutated_archdb_bounds_fail_at_each_public_boundary(self):
        for value, message in (
            (None, r"must be a JSON array"),
            ("2..5", r"must be a JSON array"),
            ([2], r"must have two inclusive"),
            ([1, 2, 3], r"must have two inclusive"),
            ([True, 5], r"must be a JSON integer"),
            ([2, False], r"must be a JSON integer"),
            ([-1, 5], r"must index one of the 16 registers"),
            ([2, 16], r"must index one of the 16 registers"),
            ([6, 2], r"must be an ordered range"),
            ([2, 8], r"does not fit the operand's encoding width"),
        ):
            with self.subTest(value=value):
                db = deepcopy(self.db)
                db["instructions"][0]["operands"][0]["register_range"] = value
                self.assert_bad_db(db, message)
        db = deepcopy(self.db)
        del db["instructions"][0]["operands"][0]["register_range"]
        self.assert_bad_db(db, r"cannot encode the register file")
        db = deepcopy(self.db)
        db["instructions"][1]["operands"][1]["register_range"] = [0, 3]
        self.assert_bad_db(db, r"only valid on register operands")
        db = deepcopy(self.db)
        field = next(row for row in db["instructions"][0]["fields"] if row["operand"] == "dst")
        field["width"] = 2
        self.assert_bad_db(db, r"width differs from its")

    def assert_bad_db(self, db, message):
        for load in (database.dumps, lambda value: database.loads(json.dumps(value)),
                     Target, generate_codec, generate_emulator):
            with self.assertRaisesRegex(ArchonError, message):
                load(db)

    @staticmethod
    def _write_files(root, files):
        for name, text in files.items():
            (root / name).write_text(text, encoding="utf-8")

    def _run_c(self, files, *sources):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        tmp = Path("/tmp/opencode")
        with tempfile.TemporaryDirectory(dir=tmp if tmp.is_dir() else None) as directory:
            root = Path(directory)
            self._write_files(root, files)
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 *sources, "-o", "checks"],
                cwd=root, check=True, timeout=30,
            )
            subprocess.run([str(root / "checks")], cwd=root, check=True, timeout=10)
            self.assertFalse((root / "unexpected.s").exists())

    def test_c99_scoped_codec_and_emulator(self):
        dbs = (
            (self.db, "window"),
            (compile_file(STDSPEC / "amd64/root.arch"), "amd"),
            (compile_file(STDSPEC / "aarch64/root.arch"), "arm"),
        )
        files = {"checks.c": C_CODEC}
        for db, prefix in dbs:
            db = database.loads(database.dumps(db))
            files[f"{prefix}.c"] = generate_emulator(db, prefix=prefix)
            files[f"{prefix}.h"] = generate_emulator_header(db, prefix=prefix)
        self._run_c(files, "checks.c", "window.c", "amd.c", "arm.c")

    def test_legacy_selection_and_emission_cannot_use_bounded_forms(self):
        # All of AMD64's instruction forms are bounded. Mark a dialect
        # available to test rejection of a fabricated allocation even when
        # there is no missing-dialect check to stop it.
        amd = compile_file(STDSPEC / "amd64/root.arch")
        amd["assembly_dialects"] = {"gas": {"supported": True}}
        options = {"oracle": False}
        files = emitter.emit({"database": amd, "options": options})
        files.update(isel.emit({"architectures": [amd, self.db], "options": options}))
        self.assertNotIn('"SCOPED"', files["ISelPattern_Windowed.i"])
        self.assertIn('"WIDE"', files["ISelPattern_Windowed.i"])
        files["checks.c"] = C_LEGACY
        files["Regallocated.i"] = """\
REGALLOC_EMISSION_BEGIN(1)
REGALLOC_INSTRUCTION_BEGIN("MOV", 2)
REGALLOC_OPERAND_REGISTER(8)
REGALLOC_OPERAND_REGISTER(0)
REGALLOC_INSTRUCTION_END()
REGALLOC_EMISSION_END()
"""
        self._run_c(files, "-DARCHON_ISEL_NO_MAIN", "checks.c", "Emitter.c", "ISel.c")


C_CODEC = r"""
#include <assert.h>
#include <string.h>
#include "window.h"
#include "amd.h"
#include "arm.h"

int main(void)
{
    uint64_t values[2] = {5, 2}, regs[WINDOW_EMU_REGISTER_COUNT] = {0};
    uint64_t decoded[2] = {99, 99};
    uint8_t bytes[3] = {0xAA, 0xAA, 0xAA};
    size_t size = 42, count = 42;
    const char *name = "untouched";
    const uint8_t below[] = {0x49, 0}; /* dst 1, src 1 */
    const uint8_t above[] = {0x72, 0}; /* dst 6, src 2 */
    struct window_emu_state window = {0};

    values[0] = 1;
    assert(window_encode("SCOPED", values, 2, bytes, 3, &size) == WINDOW_OPERAND_RANGE);
    values[0] = 6;
    assert(window_encode("SCOPED", values, 2, bytes, 3, &size) == WINDOW_OPERAND_RANGE);
    assert(bytes[0] == 0xAA && bytes[1] == 0xAA && size == 42);
    assert(window_decode(below, 2, &name, decoded, 2, &count) == WINDOW_OPERAND_RANGE);
    assert(window_decode(above, 2, &name, decoded, 2, &count) == WINDOW_OPERAND_RANGE);
    assert(decoded[0] == 99 && decoded[1] == 99 && count == 42 && !strcmp(name, "untouched"));
    window.registers = regs;
    regs[2] = 37;
    regs[6] = 41;
    assert(window_emu_execute("SCOPED", values, 2, regs, 16) == WINDOW_EMU_OPERAND_RANGE);
    assert(window_emu_execute_state("SCOPED", values, 2, &window) == WINDOW_EMU_OPERAND_RANGE);
    assert(window_emu_step_state(above, 2, &window) == WINDOW_EMU_OPERAND_RANGE);
    assert(regs[6] == 41);

    values[0] = 5;
    assert(window_encode("SCOPED", values, 2, bytes, 3, &size) == WINDOW_OK);
    assert(size == 2 && bytes[0] == 0x6A && bytes[1] == 0);
    assert(window_decode(bytes, size, &name, decoded, 2, &count) == WINDOW_OK);
    assert(count == 2 && decoded[0] == 5 && decoded[1] == 2 && !strcmp(name, "SCOPED"));
    assert(window_emu_step_state(bytes, size, &window) == WINDOW_EMU_OK && regs[5] == 37);
    values[0] = 15;
    assert(window_encode("WIDE", values, 2, bytes, 3, &size) == WINDOW_OK);
    assert(bytes[0] == 0xF2 && bytes[1] == 0xFF);

    {
        uint64_t amd_regs[AMD_EMU_REGISTER_COUNT] = {0};
        const uint8_t extended[3] = {0x49, 0x89, 0xD8};
        assert(AMD_EMU_REGISTER_COUNT == 16);
        values[0] = 8; values[1] = 3;
        assert(amd_encode("MOV", values, 2, bytes, 3, &size) == AMD_OPERAND_RANGE);
        assert(amd_emu_execute("MOV", values, 2, amd_regs, 16) == AMD_EMU_OPERAND_RANGE);
        assert(amd_decode(extended, 3, &name, decoded, 2, &count) == AMD_UNKNOWN_INSTRUCTION);
        values[0] = 0;
        assert(amd_encode("MOV", values, 2, bytes, 3, &size) == AMD_OK);
        assert(size == 3 && bytes[0] == 0x48 && bytes[1] == 0x89 && bytes[2] == 0xD8);
    }
    {
        uint64_t arm_regs[ARM_EMU_REGISTER_COUNT] = {0};
        const uint64_t args[3] = {31, 1, 2};
        struct arm_emu_state arm = {0};
        arm.registers = arm_regs;
        arm.scalar_SP = UINT64_C(0xFEDCBA9876543210);
        arm_regs[1] = 5; arm_regs[2] = 7; arm_regs[31] = 31;
        assert(ARM_EMU_SCALAR_SP_WIDTH == 64);
        assert(arm_emu_execute_state("ADD", args, 3, &arm) == ARM_EMU_OK);
        assert(arm.scalar_SP == UINT64_C(0xFEDCBA9876543210) && arm_regs[31] == 0);
    }
}
"""


C_LEGACY = r"""
#include <assert.h>
#include "Emitter.h"
#include "ISel.h"

int main(void)
{
    Emitter_Options options = {0};
    Emitter_Diagnostic diagnostic;
    ArchonISelOperand operand = {ARCHON_ISEL_OPERAND_VALUE, 64, 0, 1};
    ArchonISelOperation operation = {"copy", NULL, 64, 0, 0, &operand, 1};
    ArchonISelChoice choice;
    size_t index;

    assert(archon_isel_pattern_count("AMD64") == 0);
    assert(archon_isel_select_operation("AMD64", &operation, &choice, NULL, 0)
        == ARCHON_ISEL_NO_MATCH);
    operation.operation = NULL;
    operation.explicit_target = "MOV";
    assert(archon_isel_select_operation("AMD64", &operation, &choice, NULL, 0)
        == ARCHON_ISEL_NO_MATCH);

    assert(!Emitter_lookup_instruction("MOV", &index));
    options.dialect = EMITTER_DIALECT_GAS;
    options.output_path = "unexpected.s";
    assert(Emitter_emit(&options, &diagnostic) == EMITTER_STATUS_UNKNOWN_INSTRUCTION);
}
"""


if __name__ == "__main__":
    unittest.main()
