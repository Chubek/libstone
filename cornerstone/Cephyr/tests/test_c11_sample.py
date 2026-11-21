"""Compile and, where possible, execute a sample of ISO C11 environment requirements."""

import argparse
import os
from pathlib import Path
import platform
import resource
import signal
import subprocess
import tempfile


SCALAR_PARAMS = ", ".join(f"int p{i}" for i in range(127))


def sample_source() -> str:
    # These identifier pairs differ at the last position counted by §5.2.4.1.
    ext = "e" * 30
    local = "l" * 62
    macro = "M" * 62
    parameters = ", ".join(f"p{i}" for i in range(127))
    arguments = ", ".join(["0"] * 126 + ["17"])
    call_arguments = ", ".join(str(i + 1) for i in range(127))
    logical_line = "#define LONG_LINE" + " " * (4095 - len("#define LONG_LINE 1")) + "1"
    return "\n".join([
        "#include <assert.h>",
        "#include <iso646.h>",
        "#include <stdio.h>",
        "#include <stdlib.h>",
        "#include <wctype.h>",
        '#include "level0.h"',
        "#if __STDC__ != 1 || __STDC_VERSION__ != 201112L || __STDC_HOSTED__ != 1",
        "#error wrong hosted C11 translation mode",
        "#endif",
        "#if '\\xff' != 255",
        "#error wrong preprocessor byte value",
        "#endif",
        "#ifdef __CHAR_UNSIGNED__",
        "_Static_assert('\\xff' == 255, \"unsigned execution char\");",
        "#else",
        "_Static_assert('\\xff' == -1, \"signed execution char\");",
        "#endif",
        *["#if 1"] * 63,
        f"#define LAST({parameters}) p126",
        *["#endif"] * 63,
        logical_line,
        *[f"#define COUNT_{i} {i}" for i in range(4095)],
        f"#define {macro}a 5",
        f"#define {macro}b 6",
        *[f"extern int external_{i};" for i in range(4095)],
        f"extern int {ext}a, {ext}b;",
        f"int tail({SCALAR_PARAMS});",
        "int unevaluated(int);",
        '_Static_assert(sizeof unevaluated(0) == sizeof(int), "unevaluated call");',
        'static const char long_string[] = "' + "a" * 3000 + '"',
        '                                  "' + "b" * 1095 + '";',
        "static char big_object[65535];",
        '_Static_assert(sizeof(long_string) == 4096, "concatenated literal");',
        '_Static_assert(sizeof(big_object) == 65535, "object size");',
        "static int ************deep_pointer;",
        "static int " + "(" * 63 + "seen" + ")" * 63 + ";",
        'static void first(void) { if (seen != 2 || puts("atexit: LIFO") == EOF) _Exit(9); }',
        "static void second(void) { seen = 2; }",
        "static int check_stream(void) {",
        "  FILE *f = tmpfile();",
        '  if (not f or fputs("ab", f) == EOF) return 1;',
        "  rewind(f);",
        "  if (fgetc(f) not_eq 'a' or ungetc('c', f) != 'c' or fgetc(f) != 'c' or fgetc(f) != 'b') return 1;",
        "  return fclose(f);",
        "}",
        "static int operators(void) {",
        "  unsigned x = 15;",
        "  x and_eq 3;",
        "  x or_eq 4;",
        "  x xor_eq 1;",
        "  return (x == 6) and ((x bitand 2) bitor (x xor 6)) == 2 and (x bitand compl 6u) == 0;",
        "}",
        "static int wide_classification(void) {",
        '  wctype_t letter = wctype("alpha");',
        '  wctrans_t upper = wctrans("toupper");',
        "  return iswctype('z', letter) && !iswctype(WEOF, letter) &&",
        "         towctrans('z', upper) == 'Z' && towctrans(WEOF, upper) == WEOF;",
        "}",
        "int main(int argc, char *argv[]) {",
        '_Static_assert(sizeof __func__ == sizeof "main", "function identifier is an array");',
        *[f"  int local_{i};" for i in range(511)],
        f"  int {local}a = {macro}a, {local}b = {macro}b;",
        "  if (argc != 2 || !argv[0] || argv[2] || argv[1][0] != 'x') return 1;",
        f"  if ({local}a != 5 || {local}b != 6 || {ext}a != 7 || {ext}b != 8) return 2;",
        f"  if (LAST({arguments}) != 17 || COUNT_4094 != 4094) return 3;",
        "  if (INCLUDE_DEPTH != 15 || LONG_LINE != 1 || deep_pointer ||",
        "      sizeof(long_string) != 4096 || sizeof(big_object) != 65535) return 4;",
        "  if (long_string[2999] != 'a' || long_string[3000] != 'b' ||",
        "      long_string[4094] != 'b' || long_string[4095] != '\\0') return 5;",
        "  big_object[65534] = 'y';",
        "  " + "{" * 126,
        "  if (" + "(" * 63 + "big_object[65534]" + ")" * 63 + " != 'y') return 6;",
        "  " + "}" * 126,
        "  if (atexit(first) || atexit(second)) return 7;",
        f"  if (tail({call_arguments}) != 128) return 8;",
        "  if (check_stream() || !operators()) return 10;",
        "  if (!wide_classification()) return 11;",
        "  if (argv[1][1] == '!') assert(argc == 3);",
        "}",  # Falling off main returns zero and performs normal termination (§5.1.2.2.3).
        "",
    ])


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--qemu-aarch64")
    parser.add_argument("--qemu")
    args = parser.parse_args()
    cephyr = args.cephyr.resolve()
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = os.environ.copy()
    env.pop("CEPHYR_CONFIG", None)
    env["LC_ALL"] = "C"
    host = platform.machine().lower()
    targets = [
        ("x86_64-linux-gnu", {"x86_64", "amd64"}, None),
        ("aarch64-linux-gnu", {"aarch64", "arm64"}, args.qemu_aarch64),
        ("riscv64-linux-gnu", {"riscv64"}, args.qemu),
    ]
    with tempfile.TemporaryDirectory(prefix="cephyr-c11-sample-") as directory:
        work = Path(directory)
        env["XDG_CACHE_HOME"] = str(work / "cache")
        for i in range(15):
            (work / f"level{i}.h").write_text(
                f'#include "level{i + 1}.h"\n' if i < 14 else "#define INCLUDE_DEPTH 15\n",
                encoding="ascii",
            )
        (work / "main.c").write_text(sample_source(), encoding="ascii")
        ext = "e" * 30
        (work / "definitions.c").write_text(
            f"int {ext}a = 7, {ext}b = 8;\n"
            f"int tail({SCALAR_PARAMS}) {{ return p0 + p126; }}\n",
            encoding="ascii",
        )
        for triple, hosts, emulator in targets:
            command = [cephyr, "-std=c11", f"--target={triple}"]
            subprocess.run(
                [*command, "-ffreestanding", "-fsyntax-only", "-"],
                input=(
                    b"#include <iso646.h>\n"
                    b"#include <float.h>\n"
                    b"#if __STDC_HOSTED__ != 0 or __STDC_NO_VLA__ != 1\n"
                    b"#error wrong freestanding C11 translation mode\n"
                    b"#endif\n"
                    b"_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53,"
                    b" \"LP64 floating-point formats\");\n"
                    b"void main(void) { }\n"
                ),
                cwd=work, env=env, check=True,
            )
            subprocess.run([*command, "-c", "definitions.c", "-o", "definitions.o"],
                           cwd=work, env=env, check=True)
            executable = work / triple
            subprocess.run([*command, "main.c", "definitions.o", "-o", executable],
                           cwd=work, env=env, check=True)
            runtime_smoke = Path(__file__).resolve().parents[1] / "stdlib/tests/time-multibyte-cephyr.c"
            runtime_program = work / f"{triple}-runtime"
            subprocess.run([*command, runtime_smoke, "-o", runtime_program],
                           cwd=work, env=env, check=True)
            if host in hosts:
                runner = []
            elif emulator:
                runner = [emulator]
            else:
                print(f"{triple}: linked; execution skipped (no matching host or emulator)")
                continue
            subprocess.run([*runner, runtime_program], cwd=work, env=env, check=True)
            run = [*runner, executable]
            result = subprocess.run([*run, "x"], cwd=work, env=env,
                                    check=True, stdout=subprocess.PIPE)
            if result.stdout != b"atexit: LIFO\n":
                raise RuntimeError(f"{triple}: wrong termination output: {result.stdout!r}")
            failed = subprocess.run([*run, "x!"], cwd=work, env=env, capture_output=True)
            if (
                failed.returncode not in (-signal.SIGABRT, 128 + signal.SIGABRT)
                or b": main: assertion failed: argc == 3\n" not in failed.stderr
                or failed.stdout
            ):
                raise RuntimeError(
                    f"{triple}: wrong assertion failure: status={failed.returncode}, "
                    f"stdout={failed.stdout!r}, stderr={failed.stderr!r}"
                )


if __name__ == "__main__":
    main()
