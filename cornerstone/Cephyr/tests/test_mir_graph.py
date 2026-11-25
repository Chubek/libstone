#!/usr/bin/env python3
"""Check the opt-in graph pipeline and the Archon wire/target boundary."""

import argparse
from pathlib import Path
import platform
import re
import subprocess
import tempfile


SOURCE = """
#ifndef __STDC_NO_ATOMICS__
#include <stdatomic.h>
static atomic_uint calls;
#endif

static volatile unsigned long observable;

static unsigned long identities(unsigned long x) {
    unsigned long a = x & ~0UL;
    unsigned long b = a | 0UL;
    observable = b;
#ifndef __STDC_NO_ATOMICS__
    atomic_fetch_add_explicit(&calls, 1, memory_order_relaxed);
#endif
    return (b ^ 0UL) * 1UL;
}

static unsigned long use_vla(unsigned n) {
    unsigned long result;
    {
        volatile unsigned long storage[n];
        storage[n - 1] = n;
        result = storage[n - 1];
    }
    return result;
}

static unsigned long invoke(unsigned long (*f)(unsigned long), unsigned long x) {
    return f(x);
}

int main(void) {
    unsigned long last = 0;
    for (unsigned long i = 1; i < 5; ++i)
        last = invoke(identities, i == 2 ? 19UL : i);
#ifndef __STDC_NO_ATOMICS__
    if (atomic_load(&calls) != 4)
        return 2;
#endif
    return last != 4UL || observable != 4UL || use_vla(9) != 9UL;
}
"""


def run(argv):
    subprocess.run(argv, check=True, capture_output=True, text=True, timeout=30)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args = parser.parse_args()
    targets = [
        ("x86_64-linux-gnu", "x86_64", None),
        ("aarch64-linux-gnu", "aarch64", args.qemu_aarch64),
        ("riscv64-linux-gnu", "riscv64", args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-mir-graph-") as tmp:
        root = Path(tmp)
        src = root / "test.c"
        src.write_text(SOURCE, encoding="ascii")
        config = root / "Cephyr.toml"
        config.write_text("[blackstone]\nenabled = false\n", encoding="ascii")
        for target, host, runner in targets:
            prefix = [str(args.cephyr.resolve()), f"--target={target}", "--no-factir",
                      f"--config={config}"]
            before = root / "before.mir"
            after = root / "after.mir"
            run([*prefix, "--no-mir-opt", "--emit-mir", str(src), "-o", str(before)])
            run([*prefix, "--mir-opt", "--emit-mir", str(src), "-o", str(after)])
            base, optimized = before.read_text(), after.read_text()
            assert base.startswith("archon-mir 1\n") and optimized.startswith("archon-mir 1\n")
            assert len(re.findall(r"^(?:and|or|xor|mul) ", optimized, re.M)) < len(
                re.findall(r"^(?:and|or|xor|mul) ", base, re.M)
            ), (target, base, optimized)
            for op in ("load", "store", "call_indirect", "cbr", "stack_alloc", "stack_free"):
                assert re.search(rf"^{op} ", optimized, re.M), (target, op)
            out = root / "compiled"
            if platform.machine() == host or runner:
                run([*prefix, "--mir-opt", str(src), "-o", str(out)])
                run([str(out)] if platform.machine() == host else [runner, str(out)])
            else:
                run([*prefix, "--mir-opt", "-c", str(src), "-o", str(out)])


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        raise SystemExit(f"{error}\n{error.stdout}\n{error.stderr}") from error
