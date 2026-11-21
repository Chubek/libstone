"""Exercise C11 alignment specifiers and layout constants on native targets."""

import argparse
import os
from pathlib import Path
import platform
import re
import subprocess
import tempfile


POSITIVE = r"""
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#if __alignas_is_defined != 1 || __alignof_is_defined != 1
#error "stdalign.h must make both C11 macros available"
#endif

struct Inner {
    char tag;
    alignas(4) alignas(16) unsigned char flag;
    char tail;
};
union Payload { int word; alignas(struct Inner) char bytes[3]; };
struct Outer { char prefix; struct Inner rows[2]; union Payload payload; };
struct Flex { int len; alignas(16) char bytes[]; };

_Static_assert(alignof(struct Inner) == 16, "member increases record alignment");
_Static_assert(alignof(max_align_t) == 16, "fundamental alignment maximum");
_Static_assert(alignof(struct Inner[2]) == 16, "array has element alignment");
_Static_assert(sizeof(struct Inner) == 32, "struct tail padding");
_Static_assert(alignof(union Payload) == 16 && sizeof(union Payload) == 16,
               "union alignment and tail padding");
_Static_assert(offsetof(struct Outer, rows[1].flag) == 64, "nested offset");
_Static_assert(offsetof(struct Outer, payload.bytes[2]) == 82, "union offset");
_Static_assert(offsetof(struct Flex, bytes) == 16, "flexible member offset");
_Static_assert(_Generic(offsetof(struct Inner, flag), size_t: 1, default: 0),
               "offsetof returns size_t");

typedef struct Inner Aligned;
extern alignas(16) unsigned char global_value;
alignas(Aligned) alignas(max_align_t) unsigned char global_value = 7;
extern unsigned char global_value;
unsigned char tentative;
alignas(16) unsigned char tentative;
static alignas(16) unsigned char zeroed[3];
static const size_t nested = offsetof(struct Outer, rows[1].flag);

static int aligned(const void *p, uintptr_t mask) {
    return ((uintptr_t)p & mask) == 0;
}

int main(void) {
    char sentinel = 3;
    static alignas(16) unsigned char persistent = 9;
    alignas(offsetof(struct Inner, flag)) unsigned char array[] = {1, 2, 3};
    alignas(0) long natural = 12;
    alignas(1) alignas(16) long strictest = 13;
    alignas(int (*)(void)) char pointer_aligned = 4;
    struct Inner inner = {.flag = 10};
    unsigned char scope[16];
    {
        alignas(sizeof scope) char scope = 5, peer = 6;
        if (!aligned(&scope, 15) || !aligned(&peer, 15) || scope + peer != 11)
            return 1;
    }
    if (!aligned(&global_value, 15) || !aligned(&tentative, 15) ||
        !aligned(zeroed, 15) || !aligned(&persistent, 15) || !aligned(array, 15))
        return 2;
    if (!aligned(&natural, 7) || !aligned(&strictest, 15) || !aligned(&pointer_aligned, 7) ||
        !aligned(&inner, 15) || !aligned(&inner.flag, 15))
        return 3;
    if (sizeof global_value != 1 || sizeof array != 3 || sizeof scope != 16 ||
        sizeof natural != 8 || sizeof pointer_aligned != 1)
        return 4;
    if (sentinel != 3 || global_value != 7 || persistent != 9 || tentative ||
        zeroed[2] || array[2] != 3 || inner.flag != 10 ||
        natural != 12 || strictest != 13 || pointer_aligned != 4 || nested != 64)
        return 5;
    return 0;
}
"""

NEGATIVE = [
    ("weak", "_Alignas(1) long x;", r"cannot weaken the natural alignment"),
    ("weak-field", "struct S { _Alignas(4) long x; };", r"cannot weaken"),
    ("weak-second", "_Alignas(4) char x, *p;", r"cannot weaken"),
    ("weak-late-type", "struct S; extern _Alignas(1) struct S x;"
     "struct S { long x; };", r"cannot weaken"),
    ("negative", "_Alignas(-8) char x;", r"requires a supported alignment"),
    ("non-power", "_Alignas(6) char x;", r"requires a supported alignment"),
    ("extended", "_Alignas(32) char x;", r"requires a supported alignment"),
    ("nonconstant", "void f(int n) { _Alignas(n) char x; }", r"integer constant expression"),
    ("noninteger", "_Alignas(8.0) char x;", r"integer constant expression"),
    ("typedef", "typedef _Alignas(8) char T;", r"not allowed on a typedef"),
    ("parameter", "void f(_Alignas(8) char x);", r"not allowed on a parameter"),
    ("function", "_Alignas(8) char f(void);", r"not allowed on a function"),
    ("register", "void f(void) { register _Alignas(8) char x; }", r"not allowed on a register"),
    ("bit-field", "struct S { _Alignas(4) unsigned n:1; };", r"not allowed on a bit-field"),
    ("tag-only", "_Alignas(8) struct S;", r"requires an object declarator"),
    ("type-name", "_Static_assert(sizeof(_Alignas(8) int), \"\");", r"expected expression|type name"),
    ("incomplete-type", "struct S; _Alignas(struct S) char x;", r"complete object type"),
    ("alignof-function", "_Static_assert(_Alignof(int(void)), \"\");", r"complete object type"),
    ("alignof-expression", "int x; _Static_assert(_Alignof(x), \"\");", r"expected declaration type"),
    ("redecl-alignment", "_Alignas(8) extern char x; _Alignas(16) char x = 0;",
     r"conflicting _Alignas declarations"),
    ("redecl-missing", "_Alignas(8) extern char x; char x = 0;", r"definition must also specify"),
    ("offset-incomplete", "struct S; unsigned long x = __builtin_offsetof(struct S, n);",
     r"requires a complete struct or union"),
    ("unknown-field", "struct S { int n; }; unsigned long x = __builtin_offsetof(struct S, a);",
     r"no such record member"),
    ("offset-subscript", "struct S { int a[2]; };"
     "unsigned long x = __builtin_offsetof(struct S, a[2]);", r"outside the array bound"),
    ("offset-dynamic", "struct S { int a[2]; };"
     "unsigned long f(int i) { return __builtin_offsetof(struct S, a[i]); }",
     r"integer constant expression"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu")
    parser.add_argument("--qemu-aarch64")
    args = parser.parse_args()
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-alignment-") as directory:
        work = Path(directory)
        env = os.environ.copy()
        env.pop("CEPHYR_CONFIG", None)
        env["LC_ALL"] = "C"
        env["XDG_CACHE_HOME"] = str(work / "cache")
        source = work / "alignment.c"
        source.write_text(POSITIVE, encoding="ascii")
        for triple, hosts, emulator in targets:
            command = [args.cephyr.resolve(), "-std=c11", f"--target={triple}"]
            executable = work / triple
            subprocess.run([*command, "-ffreestanding", "-c", source, "-o", work / f"{triple}.o"],
                           cwd=work, env=env, check=True)
            subprocess.run([*command, source, "-o", executable], cwd=work, env=env, check=True)
            if host in hosts:
                subprocess.run([executable], cwd=work, env=env, check=True)
            elif emulator:
                subprocess.run([emulator, executable], cwd=work, env=env, check=True)
            else:
                print(f"{triple}: linked, execution skipped (no emulator)")
            for name, text, diagnostic in NEGATIVE:
                bad = work / f"{name}.c"
                bad.write_text(text + "\n", encoding="ascii")
                output = work / f"{name}-{triple}.s"
                result = subprocess.run([*command, "-ffreestanding", "-S", bad, "-o", output],
                                        cwd=work, env=env, capture_output=True, text=True)
                if result.returncode <= 0 or output.exists() or not re.search(diagnostic, result.stderr):
                    raise AssertionError(f"{triple} {name}: expected {diagnostic!r}; "
                                         f"exit {result.returncode}\n{result.stderr}")


if __name__ == "__main__":
    main()
