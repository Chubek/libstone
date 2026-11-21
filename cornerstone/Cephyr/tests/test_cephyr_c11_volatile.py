"""Check native scalar volatile semantics and the emitted access sequence."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


SEQUENCE = r"""
void access_sequence(volatile unsigned char *p) {
    *p;
    *p = 7;
    (void)*p;
    *p = 9;
    (void)sizeof *p;
    (void)_Generic(*p, unsigned char: 0, default: 1);
    (void)&*p;
}
"""

PROGRAM = r"""
struct Port { unsigned short status; unsigned char counters[2]; };
static volatile struct Port port;
static const volatile int read_only = 12;
static volatile signed char narrow = -3;

typedef int (*callback)(int);
static int plus_one(int arg) { return arg + 1; }
static int update(volatile int v) { return ++v; }

int main(void) {
    volatile int value = 2;
    volatile int *volatile pointer = &value;
    volatile callback call = plus_one;
    volatile struct Port *p = &port;
    if (update(value) != 3 || value != 2 || read_only != 12 || narrow != -3)
        return 1;
    if ((++*pointer) != 3 || (*pointer)++ != 3 || *pointer != 4 || call(4) != 5)
        return 2;
    p->status = 65535;
    if ((port.status += 2) != 1 || p->status != 1)
        return 3;
    int i = 0;
    p->counters[0] = 8;
    if ((p->counters[i++] += 1) != 9 || i != 1 || port.counters[0] != 9)
        return 4;
    if ((value = 5, value *= 3, value) != 15)
        return 5;
    if (0 && *(volatile int *)0)
        return 6;
    if (sizeof *(volatile int *)0 != sizeof(int))
        return 7;
    access_sequence(p->counters);
    if (p->counters[0] != 9 || p->counters[1] != 0)
        return 8;
#ifdef __CEPHYR_HAS_SCALAR_FP
    volatile float real = 1.5f;
    if ((real += 0.5f) != 2.0f || real != 2.0f)
        return 9;
#endif
    return 0;
}
"""


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
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-volatile-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env.update(LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        sequence, source, negative = (work / f"{name}.c"
                                      for name in ("sequence", "volatile", "const-volatile"))
        sequence.write_text(SEQUENCE, encoding="ascii")
        source.write_text(SEQUENCE + PROGRAM, encoding="ascii")
        negative.write_text(
            "void f(const volatile int *p) { *p = 0; }\n", encoding="ascii")
        qml = ("--qml-pass=canonicalize", "--qml-pass=strength_reduce")
        for triple, hosts, emulator, load, store in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            for mode, passes in (("unoptimized", ()), ("qml", qml)):
                asm = work / f"{triple}-{mode}.s"
                subprocess.run([*command, *passes, "-S", sequence, "-o", asm],
                               cwd=work, env=env, check=True)
                # This file has only one function. Its only byte instructions
                # should be the four evaluated accesses, not the discarded
                # operands of sizeof, _Generic or &. A runtime check alone
                # cannot observe the two discarded reads.
                accesses = re.findall(rf"(?m)^\s*({load}|{store})\s",
                                      asm.read_text(encoding="ascii"))
                if accesses != [load, store, load, store]:
                    raise AssertionError(f"{triple} {mode}: access sequence {accesses}")
            executable = work / triple
            subprocess.run([*command, *qml, source, "-o", executable],
                           cwd=work, env=env, check=True)
            if host in hosts or emulator:
                run = [executable] if host in hosts else [emulator, executable]
                subprocess.run(run, cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked, execution skipped (no emulator)")
            result = subprocess.run([*command, "-fsyntax-only", negative],
                                    cwd=work, env=env, capture_output=True, text=True)
            if result.returncode <= 0 or "modifiable scalar lvalue" not in result.stderr:
                raise AssertionError(f"{triple}: expected const assignment diagnostic\n{result.stderr}")


if __name__ == "__main__":
    main()
