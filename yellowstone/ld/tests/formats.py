"""Cross-link real COFF/Mach-O fixtures; inspect with independent LLVM tools.

Native execution is additionally checked on the corresponding host OS. Cross
inspection is deliberately not reported as Windows/dyld runtime validation.
"""
import ctypes
import os
import pathlib
import platform
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

ld, api, c_api, pe_enabled, macho_enabled = sys.argv[1:]
ld, api, c_api = map(os.path.abspath, (ld, api, c_api))
pe_enabled = pe_enabled == "ON"
macho_enabled = macho_enabled == "ON"
tools = {name: shutil.which(name) for name in
         ("clang", "llvm-ar", "llvm-readobj", "llvm-objdump", "llvm-nm", "lld-link", "ld64.lld")}
if any(not tools[t] for t in ("clang", "llvm-ar", "llvm-readobj", "llvm-objdump", "llvm-nm")):
    print("SKIP: cross-format tests require clang and LLVM inspection/archive tools")
    sys.exit(77)
if (pe_enabled and not tools["lld-link"]) or (macho_enabled and not tools["ld64.lld"]):
    print("SKIP: enabled cross-format tests require lld-link / ld64.lld")
    sys.exit(77)


def run(*args, expected=0, **kwargs):
    if args[0] == ld:
        args = (ld, "--dynamic", *args[1:])  # Explicit compatibility bridge.
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, **kwargs)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p.stdout


def info(path, *flags):
    return run(tools["llvm-readobj"], *flags, path)


def pe_headers(path):
    data = path.read_bytes()
    assert data[:2] == b"MZ"
    off = struct.unpack_from("<I", data, 60)[0]
    assert data[off:off + 4] == b"PE\0\0"
    return data, off


with tempfile.TemporaryDirectory(prefix="qld-formats-") as tmp:
    root = pathlib.Path(tmp)
    os.environ["TMPDIR"] = tmp

    def compile(name, code, target, *flags):
        src = root / (name + ".c")
        obj = root / (name + ".o")
        src.write_text(code)
        run(tools["clang"], "-target", target, "-ffreestanding", "-fno-stack-protector",
            "-c", src, "-o", obj, *flags)
        return obj

    def fail(fmt, *args, contains, output=None, extra=None):
        output = output or root / ("failure." + fmt)
        output.write_bytes(b"preserve-existing-image")
        if extra:
            extra.write_bytes(b"preserve-existing-auxiliary")
        p = subprocess.run([ld, "--dynamic", "--format=" + fmt, *map(str, args), "-o", str(output)],
                           capture_output=True, text=True)
        assert p.returncode and contains.lower() in p.stderr.lower(), (args, p.returncode, p.stderr)
        assert output.read_bytes() == b"preserve-existing-image"
        if extra:
            assert extra.read_bytes() == b"preserve-existing-auxiliary"

    win = "x86_64-pc-windows-msvc"
    mac = "x86_64-apple-macos11"
    provider = compile("provider", """
__declspec(dllexport) int datum = 41;
__declspec(dllexport) int answer(void) { return datum + 1; }
__declspec(dllexport) int (*answer_ptr)(void) = answer;
""", win, "-g", "-gcodeview")
    mach_obj = compile("mach-provider", "int datum = 41; int answer(void) { return datum + 1; }\n", mac)
    if not pe_enabled:
        fail("pe", provider, "/dll", "/noentry", contains="PE toolchain backend disabled")
    elif not tools["lld-link"]:
        raise RuntimeError("PE backend enabled but lld-link unavailable")
    else:
        # Native export directives, base relocations, stable DLL name and import library.
        dll = root / "provider name.dll"
        implib = root / "provider name.lib"
        linkmap = root / "provider.map"
        run(ld, provider, "--shared", "/noentry", "-o", dll, "-Map", linkmap)
        exports = info(dll, "--coff-exports")
        assert all(x in exports for x in ("answer", "datum", "answer_ptr"))
        assert "DIR64" in info(dll, "--coff-basereloc")
        assert implib.exists() and "answer" in linkmap.read_text()
        run(tools["llvm-ar"], "t", implib)
        if platform.system() == "Windows":
            assert ctypes.CDLL(str(dll)).answer() == 42

        # Import thunk and data import resolution from a real short-import library.
        consumer = compile("consumer", """
__declspec(dllimport) int answer(void);
__declspec(dllimport) int datum;
int entry(void) { return answer() + datum; }
""", win)
        exe = root / "consumer.exe"
        run(ld, consumer, implib, "/entry:entry", "/subsystem:console", "/nodefaultlib", "-o", exe)
        imports = info(exe, "--coff-imports")
        assert "provider name.dll" in imports and "answer" in imports and "datum" in imports
        assert "qobjld-" not in imports
        fail("pe", consumer, implib, "/entry:entry", "/subsystem:console", "--static",
             contains="contains DLL imports")

        # Static archive extraction, unused members stay unextracted.
        used = compile("used", "int add(int a, int b) { return a + b; }\n", win)
        unused = compile("unused", "extern int missing(void); int unused(void) { return missing(); }\n", win)
        static_main = compile("static-main", "extern int add(int,int); int entry(void){ return add(19,23); }\n", win)
        archive = root / "libmath.a"
        run(tools["llvm-ar"], "rcs", archive, used, unused)
        static_exe = root / "static.exe"
        run(ld, "--static", static_main, "-L", root, "-lmath", "/entry:entry", "/subsystem:console",
            "/nodefaultlib", "-o", static_exe)
        assert "Import {" not in info(static_exe, "--coff-imports")
        assert "callq" in run(tools["llvm-objdump"], "-d", static_exe)
        # Archive-only magic detection (no explicit --format).
        run(ld, archive, "/entry:add", "/subsystem:console", "/nodefaultlib", "-o", root / "archive.exe")
        if platform.system() == "Windows":
            run(static_exe, expected=42)

        # Definition-file ordinal exports and a forwarded export.
        definition = root / "provider.def"
        definition.write_text("EXPORTS\n ordinal_answer=answer @19 NONAME\n forwarded=KERNEL32.Sleep\n")
        run(ld, "--format=pe", provider, "/dll", "/noentry", "/def:" + str(definition),
            "/implib:" + str(root / "explicit.lib"), "-o", root / "ordinal.dll")
        ordinal = info(root / "ordinal.dll", "--coff-exports")
        assert "Ordinal: 19" in ordinal and "KERNEL32.Sleep" in ordinal

        # PDB and map destinations stay outside the private workspace.
        pdb = root / "symbols.pdb"
        run(ld, provider, "/dll", "/noentry", "/debug", "/pdb:" + str(pdb),
            "/out:" + str(dll), "/map:" + str(linkmap))
        assert pdb.read_bytes().startswith(b"Microsoft C/C++ MSF")
        debug = info(dll, "--coff-debug-directory")
        assert str(pdb) in debug

        # COMDAT duplicate selection and 32-bit / ARM64 targets.
        ca = compile("comdat-a", "__declspec(selectany) int common = 7; int entry(void){return common;}", win)
        cb = compile("comdat-b", "__declspec(selectany) int common = 7;", win)
        run(ld, ca, cb, "/entry:entry", "/subsystem:console", "-o", root / "comdat.exe")
        for target, machine in (("i686-pc-windows-msvc", "IMAGE_FILE_MACHINE_I386"),
                                ("aarch64-pc-windows-msvc", "IMAGE_FILE_MACHINE_ARM64")):
            obj = compile(machine, "__declspec(dllexport) int value(void){return 42;}", target)
            output = root / (machine + ".dll")
            run(ld, obj, "/dll", "/noentry", "-o", output)
            assert machine in info(output, "--file-headers")
        fail("pe", provider, obj, "/dll", "/noentry", contains="machine type")

        # Windows response grammar: spaces, apostrophes, backslashes and nesting.
        rsp = root / "pe.rsp"
        nested = root / "pe-nested.rsp"
        odd = root / "quote ' ; $ name.dll"
        nested.write_text(subprocess.list2cmdline([str(provider), "/dll", "/noentry", "/out:" + str(odd)]))
        rsp.write_text(subprocess.list2cmdline(["@" + str(nested)]))
        run(ld, "--format=pe", "@" + str(rsp))
        pe_headers(odd)
        run(api, "pe", provider, tools["lld-link"])
        run(c_api, "pe", provider, root / "c-api.dll", tools["lld-link"])
        fail("pe", consumer, "/entry:entry", "/subsystem:console", "/implib:" + str(implib),
             contains="undefined symbol", extra=implib)
        fail("pe", provider, "-r", contains="partial linking")
        fail("pe", provider, "-T", root / "any.lua", contains="does not support")
        fail("pe", provider, "--pe-linker=/nonexistent/qld", contains="cannot execute")
        fail("pe", provider, "--pe-linker=" + ld, contains="recursive")
        fail("pe", provider, "/dll", "/noentry", "/map:" + str(dll), output=dll, contains="paths collide")
        fail("pe", provider, "/dll", "/noentry", "-qld-unknown-option", contains="unsupported option")

    if not macho_enabled:
        fail("macho", mach_obj, "-dylib", contains="Mach-O toolchain backend disabled")
    elif not tools["ld64.lld"]:
        raise RuntimeError("Mach-O backend enabled but ld64.lld unavailable")
    else:
        target = ["-arch", "x86_64", "-platform_version", "macos", "11.0", "11.0"]
        dylib = root / "libprovider.dylib"
        run(ld, mach_obj, "--shared", *target, "-install_name", "@rpath/libprovider.dylib",
            "-current_version", "2.3", "-compatibility_version", "1.0", "-o", dylib)
        assert struct.unpack_from("<I", dylib.read_bytes(), 12)[0] == 6
        header = run(tools["llvm-objdump"], "--macho", "--private-headers", dylib)
        assert "@rpath/libprovider.dylib" in header and "2.3" in header
        if platform.system() == "Darwin" and platform.machine() == "x86_64":
            assert ctypes.CDLL(str(dylib)).answer() == 42

        # Non-lazy GOT binding avoids requiring an SDK's dyld_stub_binder.
        consumer = compile("mach-consumer", """
extern int answer(void);
extern int datum;
int (*ptr)(void) = answer;
int main(void) { return ptr() + datum; }
""", mac)
        exe = root / "mach-executable"
        run(ld, consumer, "-L", root, "-lprovider", *target, "-rpath", "@executable_path",
            "-o", exe, "-Map", root / "mach.map")
        assert struct.unpack_from("<I", exe.read_bytes(), 12)[0] == 2
        header = run(tools["llvm-objdump"], "--macho", "--private-headers", exe)
        assert "LC_LOAD_DYLIB" in header and "LC_MAIN" in header and "LC_RPATH" in header
        binds = run(tools["llvm-objdump"], "--macho", "--bind", exe)
        assert "_answer" in binds and "_datum" in binds
        assert "_main" in (root / "mach.map").read_text()

        # Text stub libraries are actual ld64 inputs. This deliberately narrow
        # SDK fixture describes the two real libSystem dyld/TLV exports used
        # here; it is not a replacement implementation or a runtime test.
        system = root / "libSystem.tbd"
        system.write_text("""--- !tapi-tbd
tbd-version: 4
targets: [ x86_64-macos ]
install-name: '/usr/lib/libSystem.B.dylib'
exports:
  - targets: [ x86_64-macos ]
    symbols: [ dyld_stub_binder, __tlv_bootstrap ]
...
""")
        lazy = compile("mach-lazy", "extern int answer(void); int main(void){return answer();}", mac)
        run(ld, lazy, dylib, system, *target, "-rpath", "@executable_path", "-o", exe)
        assert "_answer" in run(tools["llvm-objdump"], "--macho", "--lazy-bind", exe)
        tlv = compile("mach-tlv", "_Thread_local int tls=5; int next(void){return ++tls;}", mac)
        run(ld, tlv, system, "-dylib", *target, "-o", root / "libtlv.dylib")
        assert "__thread_vars" in info(root / "libtlv.dylib", "--sections")
        assert "__tlv_bootstrap" in run(tools["llvm-objdump"], "--macho", "--bind", root / "libtlv.dylib")

        # Static archives, forced archive loading, archive-only discovery.
        used = compile("mach-used", "int add(int a, int b){return a+b;}", mac)
        unused = compile("mach-unused", "extern int absent(void); int unused(void){return absent();}", mac)
        main = compile("mach-main", "extern int add(int,int); int main(void){return add(19,23);}", mac)
        archive = root / "libstatic.a"
        run(tools["llvm-ar"], "--format=darwin", "rcs", archive, used, unused)
        run(ld, main, archive, *target, "-o", exe)
        assert "LC_LOAD_DYLIB" not in run(tools["llvm-objdump"], "--macho", "--private-headers", exe)
        assert "_add" in run(tools["llvm-nm"], exe)
        run(ld, archive, *target, "-e", "_add", "-o", root / "archive.macho")
        fail("macho", main, "-force_load", archive, *target, contains="undefined symbol")
        if platform.system() == "Darwin" and platform.machine() == "x86_64":
            run(exe, expected=42)

        # Bundle with runtime lookup, reexports, weak dependencies and install-name default.
        run(ld, consumer, "-bundle", *target, "-undefined", "dynamic_lookup", "-o", root / "plugin.bundle")
        assert struct.unpack_from("<I", (root / "plugin.bundle").read_bytes(), 12)[0] == 8
        reexport = root / "libreexport.dylib"
        run(ld, "--format=macho", "-dylib", *target, "-reexport_library", dylib, "-o", reexport)
        header = run(tools["llvm-objdump"], "--macho", "--private-headers", reexport)
        assert "LC_REEXPORT_DYLIB" in header and str(reexport) in header and "qobjld-" not in header
        run(ld, consumer, *target, "-weak_library", dylib, "-o", exe)
        assert "LC_LOAD_WEAK_DYLIB" in run(tools["llvm-objdump"], "--macho", "--private-headers", exe)
        arm = compile("mach-arm", "int answer(void){return 42;}", "arm64-apple-macos11")
        run(ld, arm, "-dylib", "-arch", "arm64", "-platform_version", "macos", "11.0", "11.0",
            "-o", root / "arm.dylib")
        assert "Arm64" in info(root / "arm.dylib", "--file-headers")
        fail("macho", mach_obj, arm, "-dylib", *target, contains="incompatible")

        rsp = root / "macho.rsp"
        odd = root / "mach ' ; $ name.dylib"
        rsp.write_text(shlex.join([str(mach_obj), "-dylib", *target, "-o", str(odd)]))
        run(ld, "@" + str(rsp))
        assert struct.unpack_from("<I", odd.read_bytes(), 12)[0] == 6
        run(api, "macho", mach_obj, tools["ld64.lld"])
        run(c_api, "macho", mach_obj, root / "c-api.dylib", tools["ld64.lld"])
        fail("macho", consumer, *target, contains="undefined symbol")
        fail("macho", main, archive, *target, "--static", contains="fully static")
        fail("macho", main, archive, *target, "-r", contains="requested output kind")
        fail("macho", main, *target, "--macho-linker=/nonexistent/qld", contains="cannot execute")
        fail("macho", main, *target, "--macho-linker=" + ld, contains="recursive")
        fail("macho", main, *target, "--script-text=SECTIONS {}", contains="does not support")
        fail("macho", mach_obj, "-dylib", *target, "-map", dylib, output=dylib, contains="paths collide")

    for fmt, enabled, obj in (("pe", pe_enabled, provider), ("macho", macho_enabled, mach_obj)):
        if not enabled:
            continue
        rsp = root / (fmt + "-cycle.rsp")
        rsp.write_text('@"' + str(rsp) + '"')
        fail(fmt, "@" + str(rsp), contains="response file cycle")
        rsp.write_text('"unterminated')
        fail(fmt, "@" + str(rsp), contains="unterminated quote")
        fail(fmt, obj, "--" + fmt + "-linker=", contains="empty linker")
        # Backend exiting successfully without an image is never success.
        if os.name != "nt":
            fake = root / "fake linker"
            fake.write_text(f"#!{sys.executable}\nprint('no image produced')\n")
            fake.chmod(0o755)
            fail(fmt, obj, "--" + fmt + "-linker=" + str(fake), contains="no valid")
            fake.write_text(f"#!{sys.executable}\nimport os,signal\nos.kill(os.getpid(),signal.SIGTERM)\n")
            fail(fmt, obj, "--" + fmt + "-linker=" + str(fake), contains="signal 15")
    assert not list(root.glob("qobjld-*")), "backend workspace leaked"

print("PE/COFF and Mach-O cross-link/inspection/API regressions passed")
