"""Native links must work with no linker (or any other tool) on PATH."""
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ld, cc, api, c_api = map(os.path.abspath, sys.argv[1:])
clang = shutil.which("clang")
ar = shutil.which("ar")


def run(*args, env=None, expected=0):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, env=env)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


with tempfile.TemporaryDirectory(prefix="qld-native-") as tmp:
    root = pathlib.Path(tmp)
    # Also set the old recursion guard: a native link must not enter runProcess.
    isolated = {**os.environ, "PATH": str(root / "no-tools"),
                "QLD_TOOLCHAIN_BACKEND_ACTIVE": "1"}
    src = root / "start.s"
    src.write_text(".global _start\n.text\n_start: call answer\n"
                   "mov %eax, %edi\nmov $60, %eax\nsyscall\n")
    start, obj = root / "start.o", root / "answer.o"
    run(cc, "-c", src, "-o", start)
    src.write_text(".global answer\n.text\nanswer: mov $42, %eax\nret\n")
    run(cc, "-c", src, "-o", obj)
    archive = root / "libanswer.a"
    run(ar, "rcs", archive, obj)
    image = root / "image"
    for args in [
        [start, obj],
        ["--static", start, "-L", root, "-lanswer"],
        ["--format=elf", start, "-L", root, "-l:libanswer.a"],
        ["--format=elf", "--static", archive, "-e", "answer"],
    ]:
        run(ld, *args, "-o", image, env=isolated)
        assert image.read_bytes()[:4] == b"\x7fELF"
        if "answer" not in args:
            run(image, env=isolated, expected=42)
    rsp = root / "native.rsp"
    rsp.write_text(f'--static "{start}" "{obj}" -o "{image}"')
    run(ld, "@" + str(rsp), env=isolated)
    run(image, env=isolated, expected=42)
    partial = root / "partial.o"
    run(ld, "--format=elf", "-r", start, obj, "-o", partial, env=isolated)
    run(ld, partial, "-o", image, env=isolated)
    run(image, env=isolated, expected=42)
    run(api, "elf", start, obj, env=isolated)
    run(c_api, "_start", image, start, obj, env=isolated)
    run(image, env=isolated, expected=42)

    def fail(*args, contains):
        image.write_bytes(b"preserved")
        p = subprocess.run([ld, *map(str, args), "-o", str(image)],
                           capture_output=True, text=True, env=isolated)
        assert p.returncode and contains in p.stderr, (args, p.stderr)
        assert image.read_bytes() == b"preserved"

    fail("--shared", obj, contains="native")
    fail("--pie", start, obj, contains="native")
    fail("--imaginary-native-option", obj, contains="option")
    fail("--format=elf", "-m", "elf_i386", start, obj, contains="mismatch")
    fail("--static", start, obj, "-Map", image, contains="collide")
    fail(start, obj, obj, contains="multiple definition")
    src.write_text(".global _start\n.text\n_start: mov $1, %eax\nxor %ebx,%ebx\nint $0x80\n")
    obj32, archive32 = root / "start32.o", root / "libstart32.a"
    run(cc, "-m32", "-c", src, "-o", obj32)
    run(ar, "rcs", archive32, obj32)
    run(ld, "--format=elf", "--static", archive32, "-o", image, env=isolated)
    assert image.read_bytes()[4] == 1 and struct.unpack_from("<H", image.read_bytes(), 18)[0] == 3
    fail(start, obj32, contains="mixing")
    src.write_text('.global tls\n.section .tdata,"awT",@progbits\ntls: .long 1\n')
    tls = root / "tls.o"
    run(cc, "-c", src, "-o", tls)
    fail(start, obj, tls, contains="TLS")
    if not clang:
        print("native ELF tests passed; PE/Mach-O fixtures unavailable (clang not installed)")
        sys.exit(0)
    for fmt, target, entry, answer in [
        ("pe", "x86_64-pc-windows-msvc", "entry", "answer"),
        ("macho", "x86_64-apple-macos11", "_entry", "_answer"),
    ]:
        assembly = root / (fmt + ".s")
        assembly.write_text(f".text\n.globl {entry}\n{entry}: call {answer}\nret\n")
        run(clang, "-target", target, "-c", assembly, "-o", root / (fmt + "-main.o"))
        assembly.write_text(f".text\n.globl {answer}\n{answer}: mov $42, %eax\nret\n")
        run(clang, "-target", target, "-c", assembly, "-o", root / (fmt + "-answer.o"))
        main = root / (fmt + "-main.o")
        provider = root / (fmt + "-answer.o")
        for flags in [[], ["--format=" + fmt, "--static"]]:
            run(ld, *flags, main, provider, "-e", entry, "-o", image, env=isolated)
        run(api, fmt, main, provider, env=isolated)
        run(c_api, entry, image, main, provider, env=isolated)
        archive = root / ("lib" + fmt + ".a")
        run(ar, "rcs", archive, provider)
        run(ld, main, archive, "-e", entry, "-o", image, env=isolated)
        first = image.read_bytes()
        run(sys.executable, pathlib.Path(__file__).with_name("native_image.py"), image)
        run(ld, main, archive, "-e", entry, "-o", image, env=isolated)
        assert first == image.read_bytes(), "native output is nondeterministic"
        fail("--format=" + fmt, "--shared", provider, contains="native")
        fail("--format=" + fmt, "-r", main, contains="native")
        fail("--format=" + fmt, "-e", entry, main, contains="undefined")
        if fmt == "pe":
            data = image.read_bytes() if image.read_bytes() != b"preserved" else first
            peoff = struct.unpack_from("<I", data, 60)[0]
            assert data[peoff:peoff + 4] == b"PE\0\0"
            optional = peoff + 24
            assert struct.unpack_from("<I", data, optional + 16)[0]
            assert not struct.unpack_from("<H", data, optional + 70)[0] & 0x40
        else:
            assert struct.unpack_from("<IIII", first) == (0xfeedfacf, 0x1000007, 3, 2)
            assert not struct.unpack_from("<I", first, 24)[0] & 4
    fail("--format=pe", obj, contains="format")

    if clang:
        # Calls, RIP-relative data, absolute pointers, BSS and lazy archives.
        source = root / "native.c"
        source.write_text("int data = 40; int zero; int *pointer = &data;\n"
                          "int answer(void) { return *pointer + zero + 2; }\n")
        main = root / "entry.c"
        main.write_text("extern int answer(void); int entry(void) { return answer(); }\n")
        for fmt, target, ext in [("pe", "x86_64-pc-windows-msvc", "obj"),
                                 ("macho", "x86_64-apple-macos11", "o")]:
            provider, entry = root / ("provider." + ext), root / ("entry." + ext)
            run(clang, "-target", target, "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                "-c", source, "-o", provider)
            run(clang, "-target", target, "-fno-stack-protector", "-fno-asynchronous-unwind-tables",
                "-c", main, "-o", entry)
            lib = root / ("libnative-" + fmt + ".a")
            run(ar, "rcs", lib, provider)
            symbol = "entry" if fmt == "pe" else "_entry"
            run(ld, "--format=" + fmt, "--static", "-e", symbol, entry, lib,
                "-o", image, env=isolated)
            first = image.read_bytes()
            run(sys.executable, pathlib.Path(__file__).with_name("native_image.py"), image)
            run(ld, "-e", symbol, entry, lib, "-o", image, env=isolated)
            assert image.read_bytes() == first, "native output is not deterministic"
            if fmt == "pe":
                pe = struct.unpack_from("<I", first, 0x3c)[0]
                assert first[pe:pe + 4] == b"PE\0\0"
                assert struct.unpack_from("<H", first, pe + 4)[0] == 0x8664
                opt = pe + 24
                assert struct.unpack_from("<H", first, opt)[0] == 0x20b
                assert struct.unpack_from("<I", first, opt + 16)[0] != 0
                # No imports; fixed-base native images must not claim ASLR.
                assert not struct.unpack_from("<H", first, opt + 70)[0] & 0x40
                assert first[opt + 112 + 8:opt + 112 + 16] == bytes(8)
            else:
                assert struct.unpack_from("<IIII", first) == (0xfeedfacf, 0x1000007, 3, 2)
                assert not struct.unpack_from("<I", first, 24)[0] & 4  # no MH_DYLDLINK
            run(api, fmt, entry, provider, env=isolated)
            run(c_api, symbol, image, entry, provider, env=isolated)
            fail("--format=" + fmt, "--shared", provider, contains="native")
            fail("--format=" + fmt, "-e", symbol, entry, contains="undefined")
        # Mach-O SIGNED_1/SIGNED_4 relocations include the instruction's trailing
        # immediate in their embedded addend. Do not apply that bias twice.
        for type_name in ("unsigned char", "unsigned short", "int"):
            source.write_text(f"{type_name} value; int entry(void) {{ value = 42; return value; }}\n")
            run(clang, "-target", "x86_64-apple-macos11", "-fno-asynchronous-unwind-tables",
                "-c", source, "-o", obj)
            run(ld, obj, "-e", "_entry", "-o", image, env=isolated)
            run(sys.executable, pathlib.Path(__file__).with_name("native_image.py"), image)
        source.write_text("static int data = 40; static int *pointer = &data; static int zero;\n"
                          "int entry(void) { return *pointer + zero + 2; }\n")
        for fmt, target, entry in [("pe", "x86_64-pc-windows-msvc", "entry"),
                                   ("macho", "x86_64-apple-macos11", "_entry")]:
            run(clang, "-target", target, "-fno-asynchronous-unwind-tables",
                "-c", source, "-o", obj)
            run(ld, obj, "-e", entry, "-o", image, env=isolated)
            run(sys.executable, pathlib.Path(__file__).with_name("native_image.py"), image)

        source.write_text("int value = 42; int *pointer = &value; int entry(void) { return *pointer; }\n")
        run(clang, "-target", "i686-pc-windows-msvc", "-c", source, "-o", obj)
        run(ld, obj, "-e", "_entry", "-o", image, env=isolated)
        data = image.read_bytes()
        pe = struct.unpack_from("<I", data, 60)[0]
        assert struct.unpack_from("<H", data, pe + 4)[0] == 0x14c
        assert struct.unpack_from("<H", data, pe + 24)[0] == 0x10b
        source.write_text("char too_big[268435456]; int entry(void) { return 42; }\n")
        run(clang, "-target", "x86_64-pc-windows-msvc", "-c", source, "-o", obj)
        fail("--format=pe", obj, "-e", "entry", contains="limit")
        source.write_text("__declspec(dllexport) int entry(void) { return 42; }\n")
        run(clang, "-target", "x86_64-pc-windows-msvc", "-c", source, "-o", obj)
        fail("--format=pe", obj, "-e", "entry", contains="directives")
        source.write_text('inline int helper() { return 42; }\n'
                          'extern "C" int entry() { return helper(); }\n')
        run(clang, "-target", "x86_64-pc-windows-msvc", "-x", "c++", "-c", source, "-o", obj)
        fail("--format=pe", obj, "-e", "entry", contains="COMDAT")
        src.write_text(".text\n.globl _entry\n_entry: movq _value@GOTPCREL(%rip), %rax\n"
                       "mov (%rax), %eax\nret\n.data\n.globl _value\n_value: .long 42\n")
        run(clang, "-target", "x86_64-apple-macos11", "-c", src, "-o", obj)
        fail("--format=macho", obj, "-e", "_entry", contains="unsupported native Mach-O relocation")
    print("native ELF/PE/Mach-O links passed without executable toolchain access")
