"""Check volatile aggregate values and access order on the native targets."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


PROBE = r"""
struct Cell { volatile unsigned short half; unsigned char byte; };
struct Packet { unsigned char head; struct Cell cells[2]; unsigned int end; };
volatile struct Packet probe_src, probe_dst;
struct Packet probe_members;

void probe(void) {
    probe_src;
    (void)sizeof probe_src;
    (void)_Generic(probe_src, struct Packet: 0, default: 1);
    (void)&probe_src;
    probe_dst = probe_src;
    probe_src = probe_src;
    (void)probe_members;
}

void init_probe(void) {
    volatile struct Packet local = {.cells[1].half = 5, .head = 1};
}
"""

PROGRAM = r"""
#include <stddef.h>
#include <stdlib.h>

union U { unsigned long word; unsigned char bytes[8]; };
union Register { volatile unsigned char byte; unsigned int word; };
struct ReadOnly { const volatile unsigned short value; };
static const volatile struct Packet read_only = {.end = 91};
static union Register initialized = {.byte = 20};

int main(void) {
    volatile struct Packet source = {1, {{2, 3}, {4, 5}}, 6};
    struct Packet ordinary = source;
    volatile struct Packet copied = ordinary;
    if (ordinary.head != 1 || copied.cells[0].half != 2 || copied.cells[1].byte != 5)
        return 1;
    if ((source = copied = source).end != 6 || source.cells[0].byte != 3)
        return 2;
    struct Packet chosen = 0 ? *(volatile struct Packet *)0 : copied;
    if (chosen.cells[1].half != 4 || read_only.end != 91 || read_only.cells[1].byte)
        return 3;
    volatile struct Packet designated = {.cells[1].half = 15, .head = 7};
    if (designated.head != 7 || designated.cells[1].half != 15 || designated.end ||
        designated.cells[0].half || designated.cells[0].byte || designated.cells[1].byte)
        return 4;
    volatile unsigned char *padding = (volatile unsigned char *)&designated.cells[0];
    struct ReadOnly original = {31}, copy = original;
    if (padding[3] != 0 || copy.value != 31)
        return 10;
    volatile char string[5] = "a\0b";
    volatile char inferred[] = "ab";
    volatile unsigned short array[] = {[23] = 29, [1] = 12};
    if (sizeof inferred != 3 || sizeof array != 48 || string[0] != 'a' || string[1] ||
        string[2] != 'b' || string[3] || string[4] || inferred[2] ||
        array[0] || array[1] != 12 || array[22] || array[23] != 29)
        return 5;
    volatile union U left = {.word = 0x1020304050607080UL}, right = left;
    if ((right = left).word != 0x1020304050607080UL || right.bytes[7] != 0x10)
        return 6;
    union Register selected = {.byte = 12};
    if (selected.byte != 12 || initialized.byte != 20)
        return 7;
    struct Flex { unsigned long size; unsigned char mark; unsigned char extra[]; };
    volatile struct Flex *from = malloc(sizeof *from + 16);
    volatile struct Flex *to = malloc(sizeof *to + 16);
    if (!from || !to)
        return 8;
    from->size = 47;
    from->mark = 9;
    from->extra[0] = 10;
    from->extra[10] = 11;
    to->extra[0] = 19; /* Also in header padding on all the LP64 targets. */
    to->extra[10] = 20;
    *to = *from;
    if (to->size != 47 || to->mark != 9 || to->extra[0] != 19 || to->extra[10] != 20)
        return 9;
    return (void)sizeof *(volatile struct Packet *)0, 0;
}
"""


def memory_accesses(mir, function):
    """Track constant address provenance, ignoring accesses to value snapshots."""
    body = re.search(rf"(?m)^function {function}\b[^\n]*\n(.*?)(?=^(?:function|import|global|end)\b)",
                     mir, re.DOTALL)
    if not body:
        raise AssertionError(f"missing {function} in MIR:\n{mir}")
    constants, addresses, accesses = {}, {}, []
    for line in body[1].splitlines():
        instruction = line.split()
        if not instruction:
            continue
        op = instruction[0]
        if op in ("const", "global_addr", "frame_addr", "ptr_add") or op.startswith("load"):
            _, result, *args = instruction
            if op == "const":
                constants[result] = int(args[0], 0)
            elif op == "global_addr":
                addresses[result] = (args[0], 0)
            elif op == "frame_addr":
                addresses[result] = (f"frame-{args[0]}", 0)
            elif op == "ptr_add" and args[0] in addresses and args[1] in constants:
                origin, offset = addresses[args[0]]
                addresses[result] = (origin, offset + constants[args[1]])
            elif op.startswith("load") and args[0] in addresses:
                origin, offset = addresses[args[0]]
                accesses.append((origin, op, offset + int(args[-1])))
        elif instruction[0].startswith("store") and instruction[1] in addresses:
            op, address, _, disp = instruction
            origin, offset = addresses[address]
            accesses.append((origin, op, offset + int(disp)))
    return accesses


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None, "movzbq", "movb"),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64, "ldrb", "strb"),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu, "lbu", "sb"),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-volatile-aggregates-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        sequence, source, negative = (work / f"{name}.c" for name in ("probe", "run", "negative"))
        sequence.write_text(PROBE, encoding="ascii")
        source.write_text(PROBE + PROGRAM, encoding="ascii")
        qml = ("--qml-pass=canonicalize", "--qml-pass=strength_reduce")
        layout = [(8, 0), (16, 2), (8, 4), (16, 6), (8, 8), (32, 12)]

        def access(symbol, write=False):
            return [(symbol, f"store{width}" if write else f"load_u{width}", offset)
                    for width, offset in layout]

        expected = (access("probe_src") * 2 + access("probe_dst", True)
                    + access("probe_src") + access("probe_src", True) + access("probe_members"))
        for triple, hosts, emulator, byte_load, byte_store in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            for passes in ((), qml):
                mir = work / "probe.mir"
                subprocess.run([*command, *passes, "--emit-mir", sequence, "-o", mir],
                               cwd=work, env=env, check=True)
                text = mir.read_text(encoding="ascii")
                actual = [op for op in memory_accesses(text, "probe") if op[0].startswith("probe_")]
                if actual != expected:
                    raise AssertionError(f"{triple} {passes}: expected {expected}, got {actual}")
                initialized = [(op, offset) for _, op, offset in memory_accesses(text, "init_probe")]
                if initialized != [("store16", 6), ("store8", 0), ("store16", 2),
                                   ("store8", 4), ("store8", 5), ("store8", 8),
                                   ("store32", 12)]:
                    raise AssertionError(f"{triple} {passes}: unexpected initialization {initialized}")
                asm = work / "probe.s"
                subprocess.run([*command, *passes, "-S", sequence, "-o", asm],
                               cwd=work, env=env, check=True)
                native = re.findall(rf"(?m)^\s*({byte_load}|{byte_store})\s",
                                    asm.read_text(encoding="ascii"))
                # The only other byte stores initialize three members and
                # the padding of a wholly omitted cell. Every value source
                # is snapshotted in memory.
                if native != [byte_load, byte_store] * 18 + [byte_store] * 4:
                    raise AssertionError(f"{triple} {passes}: unexpected native accesses {native}")
                exe = work / triple
                subprocess.run([*command, *passes, source, "-o", exe],
                               cwd=work, env=env, check=True)
                if host in hosts or emulator:
                    subprocess.run([exe] if host in hosts else [emulator, exe],
                                   cwd=work, env=env, check=True)
            for code, diagnostic in [
                ("struct S { int x; }; void f(void) { volatile struct S s = {.x=1, .x=2}; }",
                 "overlapping designators"),
                ("union U { int x; long y; };"
                 "void f(void) { volatile union U u = {.x=1, .y=2}; }",
                 "overlapping designators"),
                ("union U { volatile unsigned x; unsigned long y; };"
                 "void f(union U *p) { (void)*p; }", "union with volatile members"),
                ("struct S { const volatile int x; }; void f(struct S *p) { *p = *p; }",
                 "const-qualified"),
            ]:
                negative.write_text(code, encoding="ascii")
                result = subprocess.run([*command, "-fsyntax-only", negative], cwd=work,
                                        env=env, capture_output=True, text=True)
                if result.returncode <= 0 or diagnostic not in result.stderr:
                    raise AssertionError(f"{triple}: expected {diagnostic}\n{result.stderr}")


if __name__ == "__main__":
    main()
