"""Optional Linux AMD64 interoperability test (real Cephyr -> SandAS -> ELF).

Pass --cephyr, --assembler, --bank (the current amd64.dsymspec), --qobjld.
The host C compiler is used only for independent test harnesses.
"""
import argparse
import ctypes
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ["cephyr", "assembler", "bank", "qobjld"]:
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[2] / "cornerstone/Cephyr"
    fixtures = project / "tests/fixtures"
    cli = fixtures / "cli_regression"
    with tempfile.TemporaryDirectory(prefix="cephyr-amd64-att-") as directory:
        work = Path(directory)
        config = work / "Cephyr.toml"
        assembler = [str(args.assembler.resolve()), "--bank", str(args.bank.resolve()),
                     "--syntax", "att", "-f", "elf"]
        config.write_text('target="x86_64-linux-gnu"\nassembler_style="as"\n[tools]\nassembler='
                          + json.dumps(assembler) + "\nlinker=" + json.dumps(str(args.qobjld.resolve())))
        cc = [str(args.cephyr.resolve()), "--config", str(config)]
        def run(argv):
            subprocess.run(argv, cwd=work, check=True, timeout=180)
        for source, harness, flags in [
            (cli / "scalar_abi_cephyr.c", cli / "scalar_abi_system.c", []),
            (fixtures / "floating/native.c", fixtures / "floating/harness.c", ["-Dtest_main=main"]),
        ]:
            run(cc + ["-I", project / "stdlib/include", "-c", source, "-o", "native.o"])
            run([args.cc, "-no-pie", *flags, harness, "native.o", "-o", "check"])
            run(["./check"])
        run(cc + [cli / f"cephyr_cli_static_{part}.c" for part in ["main", "left", "right"]]
            + ["-o", "static"])
        run(["./static"])
        run(cc + ["-nostdlib", "-shared", "-fPIC", cli / "cephyr_cli_shared_pic.c", "-o", "pic.so"])
        lib = ctypes.CDLL(str(work / "pic.so"))
        assert lib.cephyr_cli_shared_read() == 37
        lib.cephyr_cli_shared_write(61)
        assert lib.cephyr_cli_shared_read() == 61
        # Upstream x87 suite generates C and compiles twice, compares with
        # an independent C reference, and exercises variadics & PIC calls.
        wrapper = work / "cephyr-test"
        wrapper.write_text("#!" + sys.executable + "\nimport os,sys\ncc=" + repr(cc) +
                           "\nos.execv(cc[0], cc+sys.argv[1:])\n")
        wrapper.chmod(0o755)
        run([sys.executable, project / "tests/test_cephyr_c11_amd64_long_double.py",
             wrapper, "--cc", args.cc])


if __name__ == "__main__":
    main()
