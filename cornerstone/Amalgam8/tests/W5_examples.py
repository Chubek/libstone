"""Verify the declarative examples and the native embedding host end to end.

Run: python3 cornerstone/Amalgam8/tests/W5_examples.py /path/to/amalgam8
Uses CC/CXX when specified (defaults to cc/c++) and a temporary directory.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent.parent
REPO = HERE.parent.parent
CC = shlex.split(os.getenv("CC", "cc"))
CXX = shlex.split(os.getenv("CXX", "c++"))


def run(*args, cwd, expected=0):
    proc = subprocess.run(args, cwd=cwd, text=True, capture_output=True)
    if proc.returncode != expected:
        raise AssertionError(
            f"{args}: expected {expected}, got {proc.returncode}\n"
            f"stdout: {proc.stdout}\nstderr: {proc.stderr}"
        )
    return proc.stdout, proc.stderr


def arithmetic(compiler, temp):
    work = temp / "language"
    shutil.copytree(HERE / "examples/language", work)
    run(compiler, "--api", "Example.am8", cwd=work)
    run(compiler, "--cli", "Example.am8", cwd=work)
    (work / "api.c").write_text(
        '''#include "Example.hpp"
#include <stdlib.h>
int main(int argc, char **argv) {
    long long result = 1234567;
    const char *error = "previous error";
    const long long x = argc == 3 ? strtoll(argv[2], NULL, 10) : 0;
    const int status = arithmetic_interpret(argv[1], x, &result, &error);
    if (status) {
        if (result != 1234567 || !error) return 99;
        fprintf(stderr, "error: %s\\n", error);
    } else {
        if (error) return 99;
        printf("%lld\\n", result);
    }
    return status;
}
'''
    )
    (work / "direct.c").write_text(
        '''#include "Example.hpp"
int main(void) {
    long long result = 0;
    if (arithmetic_eval(2) != 44 || arithmetic_eval(-2) != -36) return 1;
    if (arithmetic_interpret("(literal 13)", 0, &result, NULL) || result != 13) return 2;
    if (!arithmetic_interpret(NULL, 0, &result, NULL)) return 3;
    if (!arithmetic_interpret("(literal 4)", 0, NULL, NULL)) return 4;
    return 0;
}
'''
    )
    run(*CC, "-std=c11", "-pedantic-errors", "api.c", "-o", "api", cwd=work)
    run(*CC, "-std=c11", "-pedantic-errors", "direct.c", "-o", "direct", cwd=work)
    run(*CXX, "-std=c++20", "-pedantic-errors", "Example.cpp", "-o", "cli", cwd=work)
    run("./direct", cwd=work)

    program = "(twice (plus (literal 2) (times (variable x) (literal 10))))"
    for args, status, out, err in (
        ((program, "2"), 0, "44\n", ""),
        ((program, "-2"), 0, "-36\n", ""),
        (("(plus (literal 3) (literal 9))",), 0, "12\n", ""),
        (("\n(times (literal -10)\t(variable x)) ", "5"), 0, "-50\n", ""),
        (("(literal -1000000)",), 0, "-1000000\n", ""),
        (("(variable x)",), 0, "0\n", ""),
        (("(variable y)",), 1, "", "error: unknown variable\n"),
        (("(variable x)", "1000001"), 1, "", "error: x must be between -1000000 and 1000000\n"),
        (("(minus (literal 2) (literal 1))",), 1, "", "error: unknown form\n"),
        (("(literal abc)",), 1, "", "error: expected an integer between -1000000 and 1000000\n"),
        (("(times (literal 2))",), 1, "", "error: expected '('\n"),
        (("(literal 3",), 1, "", "error: expected ')'\n"),
        (("(literal 3) (literal 4)",), 1, "", "error: unexpected trailing input\n"),
        (("(times (literal 1000000) (literal 1000000))",), 1, "", "error: arithmetic result out of range\n"),
        (("(twice " * 32 + "(literal 1)" + ")" * 32,), 1, "", "error: expression nesting limit exceeded\n"),
        ((" " * 1025,), 1, "", "error: program size limit exceeded\n"),
    ):
        api = run("./api", *args, cwd=work, expected=status)
        cli = run("./cli", *args, cwd=work, expected=status)
        assert api == cli == (out, err), (args, api, cli)

    assert run("./cli", cwd=work, expected=2) == ("", "usage: arithmetic PROGRAM [X]\n")

    source = (work / "Example.am8").read_text()
    for name, invalid, marker in (
        ("Undefined", "(A.times (A.variable unknown_name) (A.literal 10))", "unknown_name"),
        ("Malformed", "(A.times (A.variable x))", "times"),
    ):
        path = work / f"{name}.am8"
        path.write_text(source.replace("(A.times (A.variable x) (A.literal 10))", invalid))
        api = run(compiler, "--api", path.name, cwd=work, expected=1)
        cli = run(compiler, "--cli", path.name, cwd=work, expected=1)
        assert api == cli and marker in api[1] and path.name in api[1], (api, cli)
        assert not path.with_suffix(".hpp").exists() and not path.with_suffix(".cpp").exists()

    base_source = source.replace('  (import "ArithmeticExtra.am8")\n', "").replace(
        "(twice (A.plus (A.literal 2) (A.times (A.variable x) (A.literal 10))))",
        "(A.plus (A.literal 2) (A.times (A.variable x) (A.literal 10)))",
    ).replace("arithmetic_base_vocabulary, arithmetic_extra_vocabulary, NULL",
              "arithmetic_base_vocabulary, NULL")
    (work / "BaseOnly.am8").write_text(base_source)
    run(compiler, "--cli", "BaseOnly.am8", cwd=work)
    run(*CXX, "-std=c++20", "-pedantic-errors", "BaseOnly.cpp", "-o", "base", cwd=work)
    assert run("./base", "(plus (literal 1) (literal 2))", cwd=work) == ("3\n", "")
    assert run("./base", "(twice (literal 2))", cwd=work, expected=1) == ("", "error: unknown form\n")


def native(compiler, work):
    work.mkdir()
    example = HERE / "examples/native"
    shutil.copy(example / "Example.am8", work)
    run(
        *CXX, "-std=c++20", "-I", str(HERE), "-I", str(REPO / "firestone/libdsl"),
        str(example / "host.cpp"), "-o", "host", cwd=work,
    )
    header, _ = run("./host", "Example.am8", cwd=work)
    (work / "Example.hpp").write_text(header)
    (work / "consume.c").write_text(
        '#include "Example.hpp"\n'
        'int main(void) { return native_answer() != 42; }\n'
    )
    run(*CC, "-std=c11", "-pedantic-errors", "consume.c", "-o", "consume", cwd=work)
    run("./consume", cwd=work)
    assert "host-sum" in run(compiler, "--api", "Example.am8", cwd=work, expected=1)[1]

    for name, body, marker in (
        ("Inactive", "(host-sum 20 22)", "host-sum"),
        ("Arity", "(native host-sum) (host-sum 20)", "host-sum"),
        ("BadValue", "(native host-sum) (host-sum 20 nope)", "expected an integer"),
    ):
        filename = f"{name}.am8"
        (work / filename).write_text(f"(library {name} {body})\n")
        out, err = run("./host", filename, cwd=work, expected=1)
        assert not out and filename in err and marker in err, (name, out, err)

    (work / "UnactivatedModule.am8").write_text("(module UnactivatedModule (host-sum 20 22))\n")
    (work / "RootOnly.am8").write_text(
        '(library RootOnly (import "UnactivatedModule.am8") (native host-sum))\n'
    )
    out, err = run("./host", "RootOnly.am8", cwd=work, expected=1)
    assert not out and "UnactivatedModule.am8" in err and "host-sum" in err, (out, err)


if __name__ == "__main__":
    compiler = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="amalgam8-w5-") as directory:
        temp = Path(directory)
        arithmetic(compiler, temp)
        native(compiler, temp / "native")
