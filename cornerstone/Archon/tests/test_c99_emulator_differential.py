"""Compare generated full-state execution with Python Target."""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

from archon.c99_emulator import generate_emulator, generate_emulator_header
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
MASK64 = (1 << 64) - 1

MIXED_ARCH = """
architecture Mixed {
    endian = ENDIAN;
    address_width = 64;
    register_file Registers {
        count = 4;
        width = 64;
        register r[0..3];
        zero = 0;
    }
    register_class GPR { members = Registers.*; }
    state Cell: bits<64>;
    state Flag: bits<1>;
    stack Data: bits<64> { capacity = 2; }
    memory RAM { size = 16; }
    memory Other { size = 16; }
    memory Tiny { size = 1; }

    format Reg: bits<8> {
        field opcode: bits<2> @ [7:6];
        field lhs: bits<2> @ [5:4];
        field rhs: bits<2> @ [3:2];
        field base: bits<2> @ [1:0];
    }
    format Op: bits<8> { field opcode: bits<8> @ [7:0]; }

    instruction CYCLE {
        operands { inout lhs: GPR; inout rhs: GPR; in base: GPR; }
        encoding Reg { opcode = 1; lhs = lhs; rhs = rhs; base = base; }
        assembly "cycle {lhs}, {rhs}, {base}";
        semantics {
            sequence {
                parallel {
                    lhs := read_state(Cell);
                    Cell := load<bits<64>>(RAM, read(base));
                    rhs := read(rhs);
                    store<bits<64>>(RAM, read(base), read(rhs));
                    store<bits<8>>(RAM, read(base) +% EDGE, extract(read(lhs), 7, 0));
                    store<bits<8>>(Other, read(base) +% EDGE, extract(read(rhs), 15, 8));
                }
                Flag := extract(read(lhs), 63, 63);
                push(Data, read(lhs));
            }
        }
    }
    instruction IMPOSSIBLE_STORE {
        encoding Op { opcode = 2; }
        assembly "impossible.store";
        semantics {
            Cell := 0x1122334455667788;
            store<bits<64>>(Tiny, pop(Data), pop(Data));
        }
    }
    instruction IMPOSSIBLE_LOAD {
        encoding Op { opcode = 3; }
        assembly "impossible.load";
        semantics { Cell := load<bits<64>>(Tiny, pop(Data)); }
    }
    EXTRA
}
"""


def _literal(value):
    return f"UINT64_C(0x{value & MASK64:x})"


def _sparse(values):
    return ", ".join(f"[{i}] = {_literal(value)}" for i, value in enumerate(values) if value) or "0"


def _state_setup(db, prefix, initial, expected):
    """Emit guarded backing storage and comparisons, including pointer identity."""
    lines, checks = [f"struct {prefix}_emu_state state = {{0}};"], []

    def buffer(kind, local, member, length, before, after, compare_count):
        # Interior pointers into nonoverlapping storage are permitted. Canary
        # words test that updates don't walk past the declared backing region.
        lines.extend((
            f"{kind} {local}[{length} + 2] = {{0}};",
            f"const {kind} {local}_initial[{length}] = {{{_sparse(before)}}};",
            f"const {kind} {local}_expected[{length}] = {{{_sparse(after)}}};",
            f"memcpy({local} + 1, {local}_initial, sizeof {local}_initial);",
            f"{local}[0] = {local}[{length} + 1] = 123;",
            f"state.{member} = {local} + 1;",
        ))
        checks.extend((
            f"assert(state.{member} == {local} + 1);",
            f"assert({local}[0] == 123 && {local}[{length} + 1] == 123);",
            f"assert(memcmp({local} + 1, {local}_expected, "
            f"{compare_count} * sizeof *{local}) == 0);",
        ))

    if initial.registers:
        count = len(initial.registers)
        buffer("uint64_t", "regs", "registers", count, initial.registers, expected.registers, count)

    for name, value in initial.scalars.items():
        lines.append(f"state.scalar_{name} = {_literal(value)};")
        checks.append(f"assert(state.scalar_{name} == {_literal(expected.scalars[name])});")
    for row in db["state"]["stacks"]:
        name, capacity = row["name"], row["capacity"]
        before, after = initial.stacks[name], expected.stacks[name]
        # Inactive slots deliberately exceed the element type for narrow stacks.
        buffer("uint64_t", f"stk_{name}", f"stack_{name}.values", capacity,
               before + [MASK64] * min(1, capacity - len(before)), after, len(after))
        lines.append(f"state.stack_{name}.size = {len(before)};")
        checks.append(f"assert(state.stack_{name}.size == {len(after)});")
    for name, memory in initial.memories.items():
        buffer("uint8_t", f"mem_{name}", f"memory_{name}", len(memory), memory,
               expected.memories[name], len(memory))
    return lines, checks


class C99StateDifferential(unittest.TestCase):
    def compare(self, variants):
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or not shutil.which(cc[0]):
            self.skipTest("C99 compiler unavailable")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            includes, cases, units = [], [], []
            for prefix, db, vectors in variants:
                target = Target(db)
                (root / f"{prefix}.h").write_text(generate_emulator_header(db, prefix))
                (root / f"{prefix}.c").write_text(generate_emulator(db, prefix))
                includes.append(f'#include "{prefix}.h"')
                units.append(f"{prefix}.c")
                instructions = {inst["name"]: inst for inst in db["instructions"]}
                for mode, asm, overrides, failure in vectors:
                    status, message = failure or ("OK", None)
                    initial = target.new_state(**overrides)
                    expected = initial.copy()
                    consumed = 0
                    encoded = target.assemble_program("\n".join(asm))
                    for text in asm:
                        name, operands = target.assemble(text)
                        previous = expected.copy()
                        try:
                            updated = target.execute_state(name, operands, expected)
                        except ArchonError as exc:
                            self.assertIsNotNone(message, f"{prefix}: {text}: {exc}")
                            self.assertRegex(str(exc), message)
                            self.assertEqual(expected, previous)
                            break
                        self.assertEqual(expected, previous)
                        expected = updated
                        consumed += len(target.encode(name, operands))
                    else:
                        self.assertIsNone(message, f"{prefix}: {asm} unexpectedly succeeded")

                    lines, checks = _state_setup(db, prefix, initial, expected)
                    lines.append(f"/* {prefix}: {', '.join(asm)} */")
                    upper_status = f"{prefix.upper()}_EMU_{status}"
                    if mode == "named":
                        self.assertEqual(len(asm), 1)
                        values = [operands[o["name"]] for o in instructions[name]["operands"]]
                        arguments = "NULL"
                        if values:
                            arguments = "args"
                            lines.append(
                                "const uint64_t args[] = {"
                                + ", ".join(map(_literal, values)) + "};"
                            )
                        call = f'{prefix}_emu_execute_state("{name}", {arguments}, {len(values)}, &state)'
                    else:
                        data = ", ".join(f"0x{byte:02x}" for byte in encoded)
                        lines.append(f"const uint8_t data[] = {{{data}}};")
                        if mode == "step":
                            self.assertEqual(len(asm), 1)
                            call = f"{prefix}_emu_step_state(data, sizeof data, &state)"
                        else:
                            self.assertEqual(mode, "run")
                            lines.append("size_t consumed = 123;")
                            call = f"{prefix}_emu_run_state(data, sizeof data, &state, &consumed)"
                            checks.insert(0, f"assert(consumed == {consumed});")
                    cases.append("    {\n" + "\n".join(
                        f"        {line}"
                        for line in lines + [f"assert({call} == {upper_status});"] + checks
                    ) + "\n    }\n")

            (root / "test.c").write_text("\n".join(includes) + """
#include <assert.h>
#include <string.h>

int main(void) {
""" + "".join(cases) + "    return 0;\n}\n")
            subprocess.run(
                [*cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                 *units, "test.c", "-o", "test"],
                cwd=root, check=True,
            )
            subprocess.run([str(root / "test")], cwd=root, check=True)

    def test_mixed_state_rollback_bounds_and_aliases(self):
        variants = []
        for endian, edge in (("little", 0), ("big", 7)):
            with tempfile.TemporaryDirectory() as directory:
                source = Path(directory) / "mixed.arch"
                source.write_text(MIXED_ARCH.replace("ENDIAN", endian).replace("EDGE", str(edge))
                                  .replace("EXTRA", ""))
                db = compile_file(source)
            cell = 0xEA01020304050617
            value = 0xFEFDFCFBFAF9F817
            initial = {
                "registers": [99, 0x17, value, 8],
                "scalars": {"Cell": cell},
                "memories": {"RAM": bytes(range(16)), "Other": bytes(range(16, 32)), "Tiny": b"\x7f"},
            }
            cycle = "cycle r1, r2, r3"
            alias = "cycle r1, r1, r3"
            oob = ("MEMORY_OUT_OF_BOUNDS", "out of bounds")
            underflow = ("STACK_UNDERFLOW", "underflow")
            variants.append((endian, db, (
                ("step", [cycle], initial, None),
                ("named", [alias], initial, ("CONFLICTING_WRITES", "parallel writes to register")),
                ("named", [alias], {**initial, "scalars": {"Cell": 0x17}}, None),
                ("named", ["cycle r0, r0, r3"], initial, None),
                ("named", [cycle], {**initial, "registers": [99, 0x18, value, 8]},
                 ("CONFLICTING_WRITES", "parallel stores to memory")),
                ("named", [cycle], {**initial, "registers": [99, 0x17, value, 9]}, oob),
                ("named", [cycle], {**initial, "registers": [99, 0x17, value, MASK64]}, oob),
                ("run", [cycle, cycle], {**initial, "stacks": {"Data": [MASK64]}},
                 ("STACK_OVERFLOW", "overflow")),
                ("named", ["impossible.store"], {**initial, "stacks": {"Data": [1]}}, underflow),
                ("named", ["impossible.store"], {**initial, "stacks": {"Data": [0, 0]}}, oob),
                ("named", ["impossible.load"], initial, underflow),
                ("named", ["impossible.load"], {**initial, "stacks": {"Data": [0]}}, oob),
            )))
        self.compare(variants)

    def test_large_parallel_workspace_conflicts_and_block_reuse(self):
        common = """
                    store<bits<64>>(Other, 8, 0x8877665544332211);
                    store<bits<64>>(RAM, 4, 0x1122334455667788);
                    store<bits<64>>(Other, 0, 0x1122334455667788);
                    store<bits<64>>(RAM, 0, 0x5566778800000000);
                    store<bits<64>>(RAM, 8, 0x0000000011223344);
                    store<bits<8>>(Other, 2, trunc<8>(0x66));
        """
        extra = f"""
    instruction LARGE {{
        encoding Op {{ opcode = 4; }}
        assembly "large";
        semantics {{
            parallel {{ {common} }}
            parallel {{
                Cell := load<bits<64>>(RAM, 4);
                store<bits<8>>(RAM, 4, trunc<8>(0));
            }}
        }}
    }}
    instruction LARGE_FAIL {{
        encoding Op {{ opcode = 5; }}
        assembly "large.fail";
        semantics {{
            store<bits<8>>(Tiny, 0, trunc<8>(0xff));
            parallel {{
                {common}
                store<bits<8>>(Other, 15, trunc<8>(0xff));
                store<bits<64>>(RAM, 16, 0);
            }}
        }}
    }}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "large.arch"
            source.write_text(MIXED_ARCH.replace("ENDIAN", "little").replace("EDGE", "0")
                              .replace("EXTRA", extra))
            db = compile_file(source)
        conflict = ("CONFLICTING_WRITES", "conflicting parallel stores.*0xf")
        self.compare([("large", db, (
            ("named", ["large"], {}, None),
            ("named", ["large.fail"], {}, conflict),
            ("run", ["large", "large.fail"], {}, conflict),
        ))])

    def test_standard_specs_execute_existing_semantics(self):
        self.compare([
            ("pdp", compile_file(STDSPEC / "pdp11/root.arch"), (
                ("run", ["inc r0", "swab r0", "add r1, r0"],
                 {"registers": [0x7fff, 0xff00, 0, 0, 0, 0, 0, 0], "scalars": {"C": 1}},
                 None),
            )),
            ("wasm", compile_file(STDSPEC / "wasm/root.arch"), (
                ("run", ["drop", "drop"],
                 {"stacks": {"OperandStack": [3, MASK64], "OperandTypes": [2]}},
                 ("STACK_UNDERFLOW", "OperandTypes.*underflow")),
                ("step", ["i32.add"], {}, ("NO_SEMANTICS", "no executable semantics")),
            )),
            ("jvm", compile_file(STDSPEC / "jvm/root.arch"), (
                ("run", ["iconst_m1", "iconst_5"], {}, None),
            )),
            ("cil", compile_file(STDSPEC / "clr/root.arch"), (
                ("run", ["ldc.i4.m1", "dup", "pop", "pop", "pop"], {},
                 ("STACK_UNDERFLOW", "ValueKinds.*underflow")),
            )),
        ])
