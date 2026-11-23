"""Compile, link, and run C11 linkage and inline declarations across TUs."""

import argparse
import os
from pathlib import Path
import platform
import shutil
import tempfile

from test_cephyr_floating import EMULATIONS, START, run


SOURCE = r"""
extern int late_bound[];
static int private_object = 3;
static int completed_private[1] = {1};
static int completed_private[];
int tentative[];
extern int tentative[];
extern int initialized_external = 19;
static inline int private_function(int n) { return n + private_object; }
static int private_function(int);

inline int shared_inline(int n) {
    static const int contribution[1] = {20};
    return n + contribution[0];
}
inline int promoted_inline(int n) {
    static int count = 2;
    return n + private_function(count++);
}
extern int promoted_inline(int);

inline int plain_declaration_promotes(int n);
int plain_declaration_promotes(int n);
inline int plain_declaration_promotes(int n) { return n + private_object; }
extern inline int extern_inline(int n) { return n + private_object; }

int (*inline_address(void))(int) { return &shared_inline; }
int compare_address(void);

/* Neither declaration alone supplies both the table and result bounds. */
extern int (*(*table[])(int (*)[]))[5];
extern int (*(*table[3])(int (*)[2]))[];
_Static_assert(sizeof table == 3 * sizeof(*table), "composite table");
_Static_assert(sizeof *table[0](0) == 5 * sizeof(int), "composite return");
extern int (*(*wide)())[5];
extern int (*(*proto)(int))[];
_Static_assert(sizeof *(1 ? wide : proto)(1) == 5 * sizeof(int),
               "merge prototype and result of conditional function pointer");

static int \u03b1 = 17;
static int f\u03b1(int n) { return n + α; }
static int f_u03b1(int n) { return n - 1; }
int \u00e9 = 11;
int e\u0301 = 13; /* distinct code-point sequences, no normalization */
int ceph\u03c6(int n) { return fα(n) + f_u03b1(n); }
extern int \U0001D6C2;
extern int remote\u03b2(int);
static int *remote_address = &𝛂;
static int (*callback)(int) = &remoteβ;

int test_main(void) {
    extern int α;
    int fα(int);
    if (α != 17 || fα(1) != 18 || *remote_address != 31 ||
        callback(1) != 75 || remoteβ(2) != 77) return 11;
    {
        extern int (*incomplete)[];
        extern int (*complete)[3];
        _Static_assert(sizeof **(1 ? &incomplete : &complete) == 3 * sizeof(int),
                       "conditional composite of nested pointers");
    }
    extern int remote;
    extern int remote;
    extern int shared_inline(int); /* not a file-scope declaration */
    int imported(int);
    extern int imported(int);
    static int *saved_remote = &remote;
    int (*remote_function)(int) = &imported;
    int shadow = 6;
    {
        extern int shadow;
        shadow += 2;
    }
    if (shadow != 6 || completed_private[0] != 1) return 1;
    {
        extern int private_object;
        int private_function(int);
        ++private_object;
        if (private_function(4) != 8) return 2;
    }
    {
        extern int late_bound[3];
        extern int late_bound[];
        _Static_assert(sizeof(late_bound) == 3 * sizeof(int), "block composite");
        if (late_bound[2] != 8) return 3;
    }
    {
        extern int nested_only[2];
        extern int (*pointer_to_array)[3];
        extern int (*pointer_to_array)[];
        _Static_assert(sizeof(*pointer_to_array) == 3 * sizeof(int), "nested composite");
        if (nested_only[1] != 42 || (*pointer_to_array)[1] != 7) return 4;
    }
    if (++*saved_remote != 12 || remote_function(remote) != 13) return 5;
    if (imported(4) != 5) return 6;
    if (shared_inline(7) != 47 || inline_address()(8) != 48) return 7;
    if (promoted_inline(1) != 7 || promoted_inline(1) != 8) return 8;
    if (plain_declaration_promotes(7) != 11 || extern_inline(8) != 12) return 9;
    return compare_address() ? 0 : 10;
}
int late_bound[3] = {6, 7, 8};
int (*pointer_to_array)[3] = &late_bound;
"""

DEFINITIONS = r"""
static int α = 19;
static int fα(int n) { return α - n; }
extern int é, e\u0301;
int cephφ(int);
int 𝛂 = 31;
int remoteβ(int n) { return cephφ(n) + fα(0) + é + é + 14; }
int remote = 11;
int shadow = 9;
int nested_only[2] = {0, 42};
int imported(int n) { return n + 1; }
int shared_inline(int n) { return n + 40; }
int (*inline_address(void))(int);
extern int tentative[1];
extern int initialized_external;
int compare_address(void) {
    return &shared_inline == inline_address() && tentative[0] == 0 &&
           initialized_external == 19;
}
"""

NEGATIVE = {
    "nested-array-conflict": ("extern int (*(*table[])(int (*)[2]))[5];"
                              "extern int (*(*table[3])(int (*)[]))[];"
                              "extern int (*(*table[])(int (*)[4]))[];", "conflicting types"),
    "nested-prototype-conflict": ("int visit(int (*)(), double (*)[3]);"
                                  "int visit(int (*)(int), double (*)[]);"
                                  "int visit(int (*)(long), double (*)[]);", "conflicting types"),
    "nested-composite-call": ("extern int (*visit)(int (*)[2], int (*)[]);"
                              "extern int (*visit)(int (*)[], int (*)[4]);"
                              "int f(void) { int a[3], b[4]; return visit(&a, &b); }",
                              "incompatible object-pointer conversion"),
    "block-initializer": ("void f(void) { extern int x = 1; }", "cannot have an initializer"),
    "block-static-function": ("void f(void) { static int g(void); }", "invalid storage class"),
    "block-conflict": ("void f(void) { int x; extern int x; }", "redeclaration"),
    "block-incompatible": ("void f(void) { extern int x; extern long x; }", "conflicting types"),
    "function-incompatible": ("void f(void) { int g(int); int g(long); }", "conflicting types"),
    "hidden-conflict": ("static int x; void f(void) { int x; { extern int x; } }",
                        "conflicting linkage"),
    "hidden-typedef-conflict": ("static int x; void f(void) { typedef int x; { extern int x; } }",
                                "conflicting linkage"),
    "file-conflict": ("void f(void) { extern int x; } static int x;", "conflicting linkage"),
    "incomplete-internal-tentative": ("static int x[]; static int x[2];",
                                     "internal linkage has incomplete array type"),
    "late-incompatible": ("void f(void) { extern int x[2]; } int x[] = {1, 2, 3};",
                          "conflicting types"),
    "outside-block": ("int f(void) { { extern int x; } return x; }", "undeclared"),
    "outside-function": ("void f(void) { extern int g(void); } int h(void) { return g(); }",
                         "not a declared function"),
    "outside-file": ("void f(void) { extern int x; } int *p = &x;", "supported symbol address"),
    "no-inherited-incomplete-bound": ("void f(void) { extern int x[2]; } extern int x[];"
                                     "int g(void) { return sizeof(x); }", "incomplete"),
    "inline-missing": ("inline int f(void);", "has no definition in this translation unit"),
    "inline-static": ("inline int f(void) { static int x; return 0; }", "modifiable static object"),
    "inline-static-array": ("inline int f(void) { static int x[2]; return 0; }",
                            "modifiable static object"),
    "inline-internal-object": ("static const int x = 1; inline int f(void) { return x; }",
                               "identifier with internal linkage"),
    "inline-internal-function": ("static int g(void) { return 0; }"
                                 "inline int f(void) { int g(void); return g(); }",
                                 "identifier with internal linkage"),
    "inline-unevaluated": ("static int x; inline int f(void) { return sizeof(x); }",
                           "identifier with internal linkage"),
    "hosted-inline": ("inline int main(void) { return 0; }", "hosted 'main'"),
    "hosted-noreturn": ("_Noreturn int main(void);", "hosted 'main'"),
    "duplicate-definition": ("inline int f(void) { return 0; }"
                             "extern inline int f(void) { return 1; }", "redefinition"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--qemu-aarch64", default=shutil.which("qemu-aarch64") or
                        shutil.which("qemu-aarch64-static"))
    parser.add_argument("--qemu-riscv64", default=shutil.which("qemu-riscv64") or
                        shutil.which("qemu-riscv64-static"))
    args = parser.parse_args()
    host = platform.machine().lower()
    cephyr = (str(args.cephyr.resolve()), "-std=c11")
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-linkage-") as directory:
        work = Path(directory)
        env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
        env.pop("CEPHYR_CONFIG", None)

        def check(*command, expected=None):
            return run(*command, work=work, env=env, expected=expected)

        source, definitions = work / "use.c", work / "definitions.c"
        source.write_text(SOURCE, encoding="utf-8")
        definitions.write_text(DEFINITIONS, encoding="utf-8")
        starts = {**START, "riscv64": ("call test_main", "li a7, 93", "ecall")}
        emulations = {**EMULATIONS, "riscv64": "elf64lriscv"}
        emulators = {"aarch64": args.qemu_aarch64, "riscv64": args.qemu_riscv64}
        for arch, start_code in starts.items():
            triple = f"{arch}-linux-gnu"
            target = f"--target={triple}"
            native, provider, entry, exe = (work / f"{arch}.{suffix}" for suffix in
                                            ("use.o", "definitions.o", "start.o", "elf"))
            check(*cephyr, target, "-fPIC", "-c", source, "-o", native)
            check(*cephyr, target, "-c", definitions, "-o", provider)
            start = work / "start.s"
            start.write_text("\n".join((".text", ".globl _start", "_start:", *start_code,
                                       '.section .note.GNU-stack,"",%progbits', "")), encoding="ascii")
            abi = ("-march=rv64im", "-mabi=lp64") if arch == "riscv64" else ()
            check(args.clang, target, *abi, "-c", start, "-o", entry)
            link = (args.lld, "-m", emulations[arch], "-static", entry, native)
            check(*link, "-o", exe, expected="undefined symbol: shared_inline")
            # Both TUs export and import UTF-8 names. Replacing the provider
            # with independent C code tests the actual ELF spelling both ways.
            foreign = work / f"{arch}.clang.o"
            check(args.clang, target, *abi, "-std=c11", "-ffreestanding", "-fPIC", "-c",
                  definitions, "-o", foreign)
            for obj in (provider, foreign):
                check(*link, obj, "-o", exe)
                if host == arch or (host, arch) in (("amd64", "x86_64"), ("arm64", "aarch64")):
                    check(exe)
                elif emulators.get(arch):
                    check(emulators[arch], exe)
                else:
                    print(f"{triple} ({obj.name}): linked; execution skipped (no emulator)")

        for name, (text, diagnostic) in NEGATIVE.items():
            source, output = work / f"{name}.c", work / f"{name}.s"
            source.write_text(text + "\n", encoding="ascii")
            check(*cephyr, "--target=x86_64-linux-gnu", "-S", source, "-o", output,
                  expected=diagnostic)
            if output.exists():
                raise AssertionError(f"{name}: unexpectedly published {output}")


if __name__ == "__main__":
    main()
