#!/usr/bin/env python3
"""Compile and run packaged Firestone consumers outside the source checkout."""

import argparse
import os
import pathlib
import shlex
import shutil
import subprocess
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]


def run(*argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True)

def cli_probe(binary, cc, cxx, out, source_root):
    fixture = out / "Bundle.am8"
    fixture.write_text("""(library Bundle
  (use firestone:BigFloat)
  (use firestone libbignum/BigInt.hpp)
  (use firestone:bignum/BigInt)
  (fn int bundle_answer () (block (return 42))))
""", encoding="utf-8")
    command = [binary, "--api", "--deps", "--bundle",
               "--include-root", str(source_root), str(fixture)]
    report = subprocess.check_output(command, cwd=out, text=True)
    dependencies = [line.split("\t")[:4] for line in report.splitlines()]
    assert dependencies == [
        ["bundled", "C++", f"firestone:libbignum/{name}.hpp", f"firestone/libbignum/{name}.hpp"]
        for name in ("BigInt", "BigFloat")
    ], report
    header = out / "Bundle.hpp"
    result = header.read_bytes()
    # No explicit --include-root: same source and report with the CWD default.
    default = subprocess.check_output(
        [binary, "--api", "--deps", "--bundle", str(fixture)], cwd=source_root, text=True)
    assert default == report and header.read_bytes() == result
    assert str(source_root).encode() not in result
    assert b"#include <firestone/" not in result

    include = '#include "Bundle.hpp"\n#include "Bundle.hpp"\n'
    (out / "api.c").write_text(include + "int main(void) { return bundle_answer() != 42; }\n",
                               encoding="utf-8")
    (out / "api.cpp").write_text(include + """int main() {
    using firestone::bignum::BigInt;
    return BigInt{bundle_answer()} == BigInt{42} ? 0 : 1;
}
""", encoding="utf-8")
    flags = ["-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    run(*cc, "-std=c11", *flags, "api.c", "-o", "api_c", cwd=out)
    run(*cxx, "-std=c++17", *flags, "api.cpp", "-o", "api_cpp", cwd=out)
    run("./api_c", cwd=out)
    run("./api_cpp", cwd=out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--amalgam8", type=lambda path: str(pathlib.Path(path).resolve()))
    options = parser.parse_args()
    cxx = [options.cxx] if options.cxx else shlex.split(os.getenv("CXX", "c++"))
    cc = [options.cc] if options.cc else shlex.split(os.getenv("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="am8-bundle-") as name:
        work = pathlib.Path(name)
        run(*cxx, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror", str(HERE / "W6_catalog.cpp"),
            "-o", "generate", cwd=work)
        out = work / "out"
        out.mkdir()
        run("./generate", str(ROOT), str(out), cwd=work)
        files = ("bundled_float.h", "bundled_int.h", "bundled_complex.h")
        notice = (ROOT / "LICENSE").read_bytes()
        original = [(out / f).read_bytes() for f in files]
        for source in original:
            assert source.count(notice) == 1
            assert str(ROOT).encode() not in source

        # Use generated output alone; neither the SDK nor any libstone library
        # goes on the consumer compile/link line, and its C view stays valid.
        for order in (files, files[::-1]):
            includes = "".join(f'#include "{f}"\n' for f in (*order, *order))
            (out / "consumer.cpp").write_text(includes + """int main() {
    using namespace firestone::bignum;
    BigFloat f{};
    const Complex128 c{1.0, 2.0};
    return BigInt{6} * BigInt{7} == BigInt{42} && c.imag() == 2.0 ? 0 : 1;
}
""", encoding="utf-8")
            run(*cxx, "-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                "consumer.cpp", "-o", "consumer", cwd=out)
            run("./consumer", cwd=out)
        (out / "consumer.c").write_text(includes + "int main(void) { return 0; }\n", encoding="utf-8")
        run(*cc, "-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
            "consumer.c", "-o", "consumer_c", cwd=out)
        run("./consumer_c", cwd=out)

        override = work / "snapshot"
        (override / "firestone").mkdir(parents=True)
        shutil.copytree(ROOT / "firestone/libbignum", override / "firestone/libbignum")
        shutil.copyfile(ROOT / "LICENSE", override / "LICENSE")
        run("./generate", str(override), str(out), cwd=work)
        assert original == [(out / f).read_bytes() for f in files]
        if options.amalgam8:
            cli_probe(options.amalgam8, cc, cxx, out, override)

        def rejected(message):
            result = subprocess.run(["./generate", str(override), str(out)], cwd=work,
                                    capture_output=True, text=True)
            assert result.returncode == 1 and message in result.stderr, result.stderr

        (override / "LICENSE").write_bytes(notice.replace(b"MIT License", b"MIT license"))
        rejected("does not match the reviewed Firestone MIT license")
        (override / "LICENSE").write_bytes(notice)
        bigint = override / "firestone/libbignum/BigInt.hpp"
        bigint.write_bytes(bigint.read_bytes().replace(b"#pragma once", b"#pragma test", 1))
        rejected("changed since bundling was reviewed")
        bigint.unlink()
        bigint.symlink_to(ROOT / "firestone/libbignum/BigInt.hpp")
        rejected("cannot read approved bundle input")
    print("Packaged C11/C++17 consumers and fail-closed snapshot checks passed.")


if __name__ == "__main__":
    main()
