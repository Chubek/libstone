"""Relocate an installed package and build/run its external C consumer."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile


parser = argparse.ArgumentParser(description=__doc__)
for name in ("cmake", "cc", "cxx", "generator", "bindir", "package-dir"):
    parser.add_argument("--" + name, required=True)
for name in ("build-dir", "tests"):
    parser.add_argument("--" + name, required=True, type=Path)
for name in ("cflags", "cxxflags", "config"):
    parser.add_argument("--" + name, default="")
parser.add_argument("--tools", action="store_true")
args = parser.parse_args()
tests = args.tests.resolve()
build = args.build_dir.resolve()
config = ["--config", args.config] if args.config else []
if Path(args.bindir).is_absolute() or Path(args.package_dir).is_absolute():
    print("Relocation test requires relative bin and package directories.")
    raise SystemExit(77)


def run(command, *, cwd, env=None, stdin=None):
    proc = subprocess.run([str(arg) for arg in command], cwd=cwd, env=env,
                          input=stdin, capture_output=True, text=True)
    if proc.returncode:
        raise RuntimeError(f"{proc.args!r} failed ({proc.returncode}):\n"
                           f"{proc.stdout}\n{proc.stderr}")


with tempfile.TemporaryDirectory(prefix="asdl-install-", dir=build) as scratch:
    cwd = Path(scratch)
    original = cwd / "initial"
    prefix = cwd / "relocated"
    run([args.cmake, "--install", build, "--prefix", original, *config], cwd=cwd)
    original.rename(prefix)

    schema = tests / "fixtures/calculator.asdl"
    suffix = ".exe" if os.name == "nt" else ""
    bin_dir = prefix / args.bindir
    if args.tools:
        run([bin_dir / ("asdlc" + suffix), "--check", schema], cwd=cwd)
        run([bin_dir / ("asdl-run" + suffix), schema, "-"], cwd=cwd, stdin="""
            assert(type(asdl.lib.require("std.tree")) == "table")
        """)

    # Use C and C++ languages for the C entry point's static-library link.
    (cwd / "CMakeLists.txt").write_text("""
cmake_minimum_required(VERSION 3.20)
project(ASDLConsumer LANGUAGES C CXX)
find_package(ASDL CONFIG REQUIRED)
if(NOT EXISTS "${ASDL_STDLIB_DIR}/std/tree.lua")
  message(FATAL_ERROR "ASDL package contains no relocatable standard library")
endif()
add_executable(consumer abi_consumer.c)
target_compile_features(consumer PRIVATE c_std_11)
target_link_libraries(consumer PRIVATE ASDL::runtime)
enable_testing()
add_test(NAME consumer COMMAND consumer)
""", encoding="utf-8")
    (cwd / "abi_consumer.c").write_bytes((tests / "abi_consumer.c").read_bytes())
    child = cwd / "build"
    run([args.cmake, "-S", cwd, "-B", child, "-G", args.generator,
         f"-DCMAKE_C_COMPILER={args.cc}", f"-DCMAKE_CXX_COMPILER={args.cxx}",
         f"-DCMAKE_BUILD_TYPE={args.config}", f"-DASDL_DIR={prefix / args.package_dir}",
         f"-DCMAKE_C_FLAGS={args.cflags}", f"-DCMAKE_CXX_FLAGS={args.cxxflags}"], cwd=cwd)
    run([args.cmake, "--build", child, *config], cwd=cwd)
    env = dict(os.environ)
    env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
    ctest = Path(args.cmake).with_name("ctest" + suffix)
    test_config = ["-C", args.config] if args.config else []
    run([ctest, "--test-dir", child, "--output-on-failure", *test_config], cwd=cwd, env=env)
