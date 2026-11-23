"""Optional Linux end-to-end test using independently built native qobjld.

Pass --build-dir (Sandstone), --qobjld and the two QEMU user-mode binaries.
This script builds fresh banks, assembles both objects with SandAS, inspects
the relocations with llvm-readelf, and executes each static link.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

from test_pipeline import assemble, build_bank, find_binary


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--build-dir", required=True)
    cli.add_argument("--qobjld", required=True)
    cli.add_argument("--qemu-aarch64", default="qemu-aarch64")
    cli.add_argument("--qemu-riscv64", default="qemu-riscv64")
    args = cli.parse_args()
    compiler = find_binary(args.build_dir, "sandstone-spec-compiler")
    assembler = find_binary(args.build_dir, "sandstone-as")
    source_dir = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp)
        # Cross targets exercise a qobjld-supported ABS64 relocation. Load
        # the literal function pointer then jump through it; no linker-side
        # instruction synthesis or process entry/exit emulation.
        for arch, caller, callee, reloc, runner in [
            ("amd64", "call external", ".byte 0xbf,37,0,0,0,0xb8,60,0,0,0\nsyscall",
             "R_X86_64_PLT32", []),
            ("aarch64", ".long 0x58000050,0xd61f0200\n.quad external",
             "movz x0,37\nmovz x8,93\nsvc 0", "R_AARCH64_ABS64", [args.qemu_aarch64]),
            ("rv64im", ".long 0x297,0x0102b283,0x28067,0x13\n.quad external",
             "addi x10,x0,37\naddi x17,x0,93\necall", "R_RISCV_64", [args.qemu_riscv64]),
        ]:
            bank = str(work / (arch + ".symas"))
            build_bank(str(source_dir), compiler,
                       str(source_dir / "compiler/stdspec" / (arch + ".dsymspec")), bank)
            objects = []
            for label, body in [("_start", caller), ("external", callee)]:
                obj = str(work / (label + ".o"))
                assemble(assembler, bank, f".text\n.global {label}\n{label}:\n{body}\n", obj, fmt="elf")
                objects.append(obj)
            info = subprocess.check_output(["llvm-readelf", "-hSWsr", objects[0]], text=True)
            assert reloc in info and "external" in info, info
            exe = str(work / arch)
            subprocess.run([args.qobjld, "-e", "_start", *objects, "-o", exe], check=True)
            status = subprocess.run(runner + [exe], timeout=10).returncode
            assert status == 37, (arch, status)
            print(f"{arch}: {reloc}, exit {status}")


if __name__ == "__main__":
    main()
