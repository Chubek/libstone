#!/usr/bin/env python3
"""Compile native generated APIs and CLI against real Libstone headers and code."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(exe, *args, expected=0):
    command = [str(exe), *map(str, args)]
    actual = subprocess.run(command, timeout=90, check=False).returncode
    if actual != expected:
        raise RuntimeError(f"{command!r}: expected exit {expected}, got {actual}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    parser.add_argument("--driver", type=Path)
    args = parser.parse_args()
    am8 = Path(__file__).resolve().parents[1]
    root = am8.parents[1]
    warnings = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    with tempfile.TemporaryDirectory(prefix="am8-w7-") as tmp:
        work = Path(tmp)
        driver = args.driver.resolve() if args.driver else work / "driver"
        if args.driver is None:
            run(args.cxx, "-std=c++20", *warnings, "-I", am8,
                "-I", root / "firestone/libdsl", am8 / "tests/W7.cpp", "-o", driver)
        run(driver, am8 / "examples/interop", work, root)

        xbind = work / "xbind.o"
        run(args.cc, "-std=c11", *warnings, "-c",
            root / "tourchstone/xbind/xbind.c", "-o", xbind)
        c_api = work / "c-api.c"
        c_api.write_text(
            '#include "NativeC.h"\n'
            'int main(void) { return has_target("lua") != 1 || '
            'has_target("not-a-target") != 0 || has_target(0) != 0; }\n',
            encoding="utf-8")
        run(args.cc, "-std=c11", *warnings, "-I", root, c_api, xbind, "-o", work / "c-api")
        run(work / "c-api")

        cpp_api = work / "cpp-api.cpp"
        cpp_api.write_text(
            '#include "NativePipeline.hpp"\n'
            'int main() { return native_pipeline("lua", "(a b c)") != 0 || '
            'am8_moonstone_sexpr_arity("()") != 0 || '
            'am8_moonstone_sexpr_arity(nullptr) != -1 || '
            'am8_moonstone_sexpr_arity("one two") != -1 || '
            'am8_moonstone_sexpr_arity(std::string(65, \'(\').c_str()) != -2 || '
            'am8_moonstone_sexpr_arity(std::string(4097, \' \').c_str()) != -2; }\n',
            encoding="utf-8")
        for name, source in [("cpp-api", cpp_api), ("cli", work / "NativePipeline.cpp"),
                             ("cli-bundled", work / "NativePipelineBundled.cpp")]:
            run(args.cxx, "-std=c++20", *warnings, "-I", root, source, xbind, "-o", work / name)
        run(work / "cpp-api")
        run(work / "cli", "lua", "(one two three)")
        run(work / "cli-bundled", "lua", "(one two three)")
        run(work / "cli", "not-a-target", "(one two three)", expected=1)
        run(work / "cli", "lua", "(broken", expected=2)
        run(work / "cli", "lua", "(one two)", expected=3)
        run(work / "cli", expected=64)


if __name__ == "__main__":
    main()
