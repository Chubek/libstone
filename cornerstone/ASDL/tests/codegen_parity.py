#!/usr/bin/env python3
"""Differential and runtime checks for the native ASDL generators."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile


TESTS = Path(__file__).resolve().parent
PERL = TESTS.parent / "scripts" / "asdl.pl"
PERL_EXECUTABLE = os.environ.get("PERL", "perl")
FIXTURE = TESTS / "fixtures" / "calculator.asdl"


def run(*args, **kwargs):
    return subprocess.run(
        args, stdout=subprocess.PIPE, check=True, **kwargs
    ).stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("asdl", type=Path, help="path to the native asdl executable")
    args = parser.parse_args()
    asdl = args.asdl.resolve()
    cc = shlex.split(os.environ.get("CC", "cc"))
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    flags = ("-Wall", "-Wextra", "-Werror", "-pedantic", "-UNDEBUG")

    legacy = run(PERL_EXECUTABLE, PERL, "--check", "--schema=-", FIXTURE)
    native = run(asdl, "--check", "--schema=-", FIXTURE)
    assert json.loads(legacy) == json.loads(native)
    assert native == run(asdl, "--check", "--schema=-", FIXTURE)
    assert run(PERL_EXECUTABLE, PERL, FIXTURE).split(b"\n", 1)[1] == run(asdl, FIXTURE).split(b"\n", 1)[1]

    with tempfile.TemporaryDirectory(prefix="asdl-codegen-test-") as tmp:
        root = Path(tmp)
        for split in (False, True):
            case = root / ("split" if split else "header-only")
            include, src, runtime = (case / part for part in ("include", "src", "runtime"))
            for part in (include, src, runtime):
                part.mkdir(parents=True)
            source = src / "calc.c"
            run(
                asdl, "--header", include / "calc.h", "--visitor", include / "walk.h",
                "--listener", include / "events.h", "--builtins-header", runtime / "asdl.h",
                "--cpp-header", include / "calc.hpp", *(("--source", source) if split else ()),
                FIXTURE,
            )
            binary, log = case / "calc", case / "events.log"
            log.write_text("one\ntwo")
            run(
                *cc, "-std=c11", *flags, "-I", include, TESTS / "runtime.c",
                TESTS / "runtime_other.c", *((source,) if split else ()), "-o", binary,
            )
            run(binary, log)
            cpp = case / "cpp"
            run(*cxx, "-std=c++20", *flags, "-I", include, TESTS / "codegen_runtime.cpp", "-o", cpp)
            run(cpp)

        collision = root / "collision.asdl"
        collision.write_text("module Conflict { expr = Number(int value); number = (expr value); }\n")
        bad = subprocess.run(
            (asdl, "--check", "--guard=conflict_expr_node", collision),
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        assert bad.returncode != 0 and b"conflict_expr_node" in bad.stderr and not bad.stdout

        # Quotes, backslashes and high bytes in schema origin paths.
        escaped = root / 'input-é"\\.asdl'
        escaped.write_bytes(FIXTURE.read_bytes())
        data = run(asdl, "--check", "--schema=-", escaped)
        assert json.loads(data) == json.loads(run(PERL_EXECUTABLE, PERL, "--check", "--schema=-", escaped))

        macros = root / "macros.asdl"
        macros.write_text("module Macros { expr = EOF(int class) | EOF_(int access); }\n")
        header, cpp = root / "macros.h", root / "macros.hpp"
        run(asdl, "--header", header, "--cpp-header", cpp, macros)
        c_source = root / "macros.c"
        c_source.write_text('#include "macros.h"\n')
        run(*cc, "-std=c11", *flags, "-fsyntax-only", c_source)
        source = root / "macros.cpp"
        source.write_text(
            '#include "macros.hpp"\n'
            'static_assert(macros_ast::reflection::types[0].constructors[0].name == "EOF");\n'
        )
        run(*cxx, "-std=c++20", *flags, "-fsyntax-only", source)

    print("PASS: differential schema, deterministic output, strict C11 + C++20 consumers")


if __name__ == "__main__":
    main()
