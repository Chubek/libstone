"""Exercise C11 enumeration semantics and native code generation."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


SOURCE = r"""
#include <limits.h>

enum Value {
    Zero,
    Negative = -(2 + 3),
    Successor,
    Size = sizeof(char[3]) + _Alignof(char),
    Next,
    Letter = 'A' - '0',
    Alias = Next,
    Minimum = INT_MIN,
    AboveMinimum,
    Maximum = INT_MAX,
    Reset = UINT_MAX + 1U,
    Rounded = (int)3.75,
    ShortCircuit = 1 || 1 / 0,
    Choice = 0 ? 1 / 0 : Rounded + 2,
    μ = Choice,
};

_Static_assert(Zero == 0 && Negative == -5 && Successor == -4, "implicit values");
_Static_assert(Size == 4 && Next == 5 && Alias == 5 && Letter == 17, "typed ICEs");
_Static_assert(AboveMinimum == INT_MIN + 1 && Maximum == INT_MAX && Reset == 0, "int range");
_Static_assert(Rounded == 3 && ShortCircuit == 1 && \u03bc == Choice, "constant semantics");
_Static_assert(_Generic(Maximum, int: 1, default: 0), "enumeration constant has type int");

typedef enum Second { Other = 5 } Second;
_Static_assert(_Generic((enum Value)Zero, Second: 0, enum Value: 1),
               "different enumerations have distinct types");
_Static_assert(_Generic((Second)Other, int: 1, default: 0), "int-compatible enum");
typedef int Counted[Other];
typedef int Counted[5];
typedef int Computed[Next];
typedef int Computed[Next];

struct Block {
    enum Length { Length = 3 } marker;
    int Length;
    int data[Length];
};
static struct Block block = { .marker = Length, .Length = Next, .data = {[Length - 1] = 13} };
static enum Value global = Successor;
static int designated[Next] = {[Alias - 1] = 19};
static int *null_pointer = Zero;

static int dispatch(enum Value value) {
    switch (value) {
    case Negative: return 1;
    case Next: return 2;
    default: return 3;
    }
}

static int parameters(int first, enum { ArgumentSize = sizeof first } n, int values[ArgumentSize]) {
    return n + values[ArgumentSize - 1];
}

static int scope(void) {
    char local[6];
    typedef enum { Measured = sizeof local } Local;
    Local length = Measured;
    {
        enum Value { Next = Next + 1, Neighbor, Measured = 2 };
        char local[Measured];
        static int from_inner = Neighbor;
        _Static_assert(Next == 6 && Neighbor == 7 && sizeof local == 2, "shadowing");
        if (from_inner != 7) return 0;
    }
    {
        int Next = 9;
        enum { Seen = sizeof Next };
        if (Next != 9 || Seen != sizeof(int)) return 0;
    }
    if (sizeof(enum { Condition = 11 }) == sizeof(int)) {
        if (Condition != 11) return 0;
    }
    if ((sizeof(enum { Test = 12 }) ? Test : 0) != 12) return 0;
    for (int i = 0; i < 1; ++i, (void)sizeof(enum { Step = 14 })) {
        if (Step != 14) return 0;
    }
    return length == 6 && Next == 5;
}

int main(void) {
    int values[4] = {[Size - 1] = 24};
    if (dispatch(Negative) != 1 || dispatch(Alias) != 2 || dispatch(Zero) != 3) return 1;
    if (global != -4 || designated[4] != 19 || null_pointer) return 2;
    if (block.data[2] != 13 || block.Length != 5 || block.marker != 3) return 3;
    if (parameters(0, 3, values) != 27 || !scope()) return 4;
    return 0;
}
"""


NEGATIVE = [
    ("nonconstant", "const int n = 3; enum { A = n };", r"enumerator 'A'.*integer constant"),
    ("floating", "typedef enum { A = 2.5 } E;", r"enumerator 'A'.*integer constant"),
    ("forward", "enum { A = B, B = 1 };", r"enumerator 'A'.*integer constant"),
    ("self", "enum { A = A };", r"enumerator 'A'.*integer constant"),
    ("outside-int-positive", "enum { A = 2147483648U };", r"not representable as int"),
    ("outside-int-negative", "void f(void) { enum { A = -2147483649L }; }",
     r"not representable as int"),
    ("implicit-overflow", "enum E { A = 2147483647, B };", r"enumerator 'B'.*not representable as int"),
    ("arithmetic-overflow", "enum { A = 2147483647 + 1 };", r"signed overflow"),
    ("duplicate", "enum { A, A = 0 };", r"enumerator 'A' is already declared"),
    ("conflicting-object-before", "int A; enum { A };", r"enumerator 'A' is already declared"),
    ("conflicting-object-after", "enum { A }; int A;", r"name 'A' is already declared"),
    ("conflicting-parameter", "void f(int A) { enum { A }; }", r"enumerator 'A' is already declared"),
    ("conflicting-typedef", "typedef int A; enum { A };", r"enumerator 'A' is already declared"),
    ("duplicate-case", "enum { A = 1, B = 1 }; int f(int n) { switch (n) {"
     "case A: return 1; case B: return 2; } return 0; }", r"duplicate case value"),
    ("not-lvalue", "enum { A }; int *p(void) { return &A; }", r"lvalue"),
    ("out-of-block", "int f(void) { { enum { A = 1 }; } return A; }", r"undeclared|no valid declaration"),
    ("out-of-prototype", "void f(enum { A } x); int n = A;", r"integer constant expression"),
    ("incomplete", "enum E { A = sizeof(enum E) };", r"needs a prior definition"),
    ("different-types", "enum E { A }; enum F { B }; void f(enum E *p) { enum F *q = p; }",
     r"incompatible"),
]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--qemu")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-enums-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env["LC_ALL"] = "C"
        env["XDG_CACHE_HOME"] = str(work / "cache")
        fixture = work / "enums.c"
        fixture.write_text(SOURCE, encoding="utf-8")
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", "-ffreestanding", f"--target={triple}"]
            executable = work / triple
            subprocess.run([*command, fixture, "-o", executable], cwd=work, env=env, check=True)
            if host in hosts or emulator:
                runner = [] if host in hosts else [emulator]
                subprocess.run([*runner, executable], cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked, execution skipped (no emulator)")
            for name, source, diagnostic in NEGATIVE:
                bad = work / f"{name}.c"
                bad.write_text(source + "\n", encoding="ascii")
                result = subprocess.run([*command, "-fsyntax-only", bad], cwd=work, env=env,
                                        capture_output=True, text=True)
                if result.returncode <= 0 or not re.search(diagnostic, result.stderr):
                    raise AssertionError(f"{triple} {name}: expected {diagnostic!r}; "
                                         f"exit {result.returncode}\n{result.stderr}")


if __name__ == "__main__":
    main()
