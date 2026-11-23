"""Cross-inspect SandAS objects; optionally link the same fixtures with qobjld/LLD."""
import argparse
import os
from pathlib import Path
import platform
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from test_pipeline import find_binary


def run(*args, env=None):
    return subprocess.check_output(args, env=env, text=True)


def check_arm_image(image, coff):
    """Follow the linked BL, ADRP+ADD, then pointer+addend in virtual memory."""
    data = image.read_bytes()
    def get(fmt, offset):
        return struct.unpack_from("<"+fmt, data, offset)
    segments = []
    if coff:
        pe, = get("I", 60)
        optional = pe+24
        base, = get("Q", optional+24)
        entry = base+get("I", optional+16)[0]
        table = optional+get("H", pe+20)[0]
        for i in range(get("H", pe+6)[0]):
            _, addr, size, off = get("IIII", table+40*i+8)
            segments.append((base+addr, size, off))
    else:
        cmd = 32
        for _ in range(get("I", 16)[0]):
            kind, length = get("II", cmd)
            if kind == 0x19:
                addr, _, off, size = get("QQQQ", cmd+24)
                segments.append((addr, size, off))
            if kind == 0x80000028:
                entryoff, = get("Q", cmd+8)
            cmd += length
        entry = next(addr+entryoff-off for addr,size,off in segments if off <= entryoff < off+size)
    def read(addr, fmt="I"):
        off = next(off+addr-start for start,size,off in segments if start <= addr < start+size)
        return get(fmt, off)[0]
    def signed(v, n):
        return (v ^ (1 << (n-1))) - (1 << (n-1))
    pc = entry+8
    call = read(pc)
    assert call >> 26 == 0b100101
    target = pc + 4*signed(call & 0x3ffffff, 26)
    page, add = read(target), read(target+4)
    assert page & 0x9f00001f == 0x90000009 and add & 0xffc003ff == 0x91000129
    ptr = (target & ~4095) + (signed(((page >> 29) & 3) | (((page >> 5) & 0x7ffff)<<2), 21)<<12)
    ptr += (add >> 10) & 0xfff
    assert read(read(ptr, "Q")) == 42
    assert read(ptr+8, "Q") == target-8


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--build-dir", required=True)
    cli.add_argument("--qobjld", help="test native AMD64 static links")
    cli.add_argument("--lld", action="store_true", help="test independent cross-linkers")
    args = cli.parse_args()
    if not all(shutil.which(x) for x in ("llvm-readobj", "llvm-nm")):
        print("SKIP: object inspection needs llvm-readobj and llvm-nm")
        return 77
    sand = Path(__file__).resolve().parents[1]
    assembler = find_binary(args.build_dir, "sandstone-as")
    compiler = find_binary(args.build_dir, "sandstone-spec-compiler")
    isolated = dict(os.environ, PATH="", QLD_TOOLCHAIN_BACKEND_ACTIVE="1")
    with tempfile.TemporaryDirectory(prefix="sandas-object-") as tmp:
        root = Path(tmp)
        for arch in ("amd64", "aarch64"):
            arm = arch == "aarch64"
            for fmt, profile in (("coff", "windows"), ("macho", "darwin")):
                coff = fmt == "coff"
                bank = root / f"{arch}-{fmt}.symas"
                run(sys.executable, sand / "scripts/spec-compiler.py", "--compiler", compiler,
                    "--object-profile", profile, "--validate", "-o", bank,
                    sand / f"compiler/stdspec/{arch}.dsymspec")
                asm = [assembler, "--bank", bank, "--syntax", "default" if arm else "att"]
                entry = "_entry" if not coff else "entry"
                def assemble(label, source):
                    src, out = root / (label+".s"), root / (label+".o")
                    src.write_text(source)
                    run(*asm, "-f", fmt, "-o", out, src, env=isolated)
                    return out

                # Return the second word via an absolute pointer with addend +4.
                caller = f".text\n.globl {entry}\n{entry}:\n"
                caller += ("sub sp,sp,#16\nstr x30,[sp]\nbl external_function\n"
                           "ldr x30,[sp]\nadd sp,sp,#16\nret x30\n" if arm else
                           "subq $40,%rsp\ncall external_function\naddq $40,%rsp\nret\n")
                caller += ".bss\n.balign 64\nreserve:\n.space 65536\n"
                caller += ".data\n.balign 32\n.globl ptr\nptr:\n.quad local_data+4,external_function-8\n"
                caller += ".rodata\nlocal_data:\n.long 13,42\n"
                main_obj = assemble("main", caller)
                body = ("adrp x9, ptr\nadd x9,x9,:lo12:ptr\nldr x9,[x9]\nldr w0,[x9]\nret x30\n"
                        if arm else "movq ptr(%rip),%rax\nmovl (%rax),%eax\nret\n")
                provider = assemble("provider", ".text\n.global external_function\nexternal_function:\n"+body)
                info = run("llvm-readobj", "--file-headers", "--sections", "--section-data",
                           "--relocations", main_obj)
                for expected in [
                        ("IMAGE_FILE_MACHINE_ARM64" if arm else "IMAGE_FILE_MACHINE_AMD64")
                        if coff else ("Arm64" if arm else "X86-64"),
                        ("IMAGE_REL_ARM64_BRANCH26" if arm else "IMAGE_REL_AMD64_REL32")
                        if coff else ("ARM64_RELOC_BRANCH26" if arm else "X86_64_RELOC_BRANCH"),
                        "ADDR64" if coff else "RELOC_UNSIGNED",
                        "IMAGE_SCN_ALIGN_64BYTES" if coff else "Alignment: 6",
                        "IMAGE_SCN_CNT_UNINITIALIZED_DATA" if coff else "ZeroFill",
                        "04000000 00000000", "F8FFFFFF FFFFFFFF", "0D000000 2A000000",
                        "RawDataSize: 65536" if coff else "Size: 0x10000"]:
                    assert expected in info, (expected, info)
                assert main_obj.stat().st_size < 2048
                symbols = run("llvm-nm", main_obj)
                for kind, name in (("T", entry), ("U", "external_function"), ("D", "ptr"),
                                   ("[rs]", "local_data"), ("[bs]", "reserve")):
                    assert re.search(rf"\b{kind} {name}$", symbols, re.M), symbols
                def reject(mode, source, diagnostic):
                    src, out = root / "bad.s", root / "existing.o"
                    src.write_text(source)
                    out.write_bytes(b"original")
                    p = subprocess.run([*asm,"-f",mode,"-o",out,src], capture_output=True,text=True)
                    assert p.returncode and diagnostic in p.stderr and out.read_bytes()==b"original", p
                reject("macho" if coff else "coff", ".text\nnop\n", "target/ABI")
                reject(fmt, ".data\n.byte external\n", "unsupported native relocation")
                reject(fmt, ".data\n.local missing\n.quad missing\n", "undefined local")
                image = root / "image"
                if args.lld:
                    if coff:
                        run("lld-link", "/entry:"+entry, "/subsystem:console", "/nodefaultlib",
                            "/out:"+str(image), main_obj, provider)
                    else:
                        run("ld64.lld", "-arch", "arm64" if arm else "x86_64", "-platform_version",
                            "macos", "11", "11", "-e", entry, "-o", image, main_obj, provider)
                    if arm:
                        check_arm_image(image, coff)
                if args.qobjld and not arm:
                    run(args.qobjld, "--static", "-e", entry, main_obj, provider, "-o", image, env=isolated)
                    if sys.platform.startswith("linux") and platform.machine()=="x86_64":
                        run(sys.executable, sand.parent / "yellowstone/ld/tests/native_image.py", image)
                print(arch, fmt, "OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
