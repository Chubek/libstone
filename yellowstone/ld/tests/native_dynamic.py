"""Run native ELF dynamic links with external linker execution forbidden."""
import ctypes
import os
import pathlib
import struct
import subprocess
import sys
import tempfile


ld, cc, api, c_api = map(os.path.abspath, sys.argv[1:])


def run(*args, env=None, expected=0):
    process = subprocess.run(list(map(str, args)), capture_output=True,
                             text=True, env=env)
    assert process.returncode == expected, (
        args, process.returncode, process.stdout, process.stderr)
    return process


def elf_type(data):
    assert data[:6] == b"\x7fELF\x02\x01"
    return struct.unpack_from("<H", data, 16)[0]


def program_types(data):
    phoff = struct.unpack_from("<Q", data, 32)[0]
    entsize, count = struct.unpack_from("<HH", data, 54)
    return [struct.unpack_from("<I", data, phoff + i * entsize)[0]
            for i in range(count)]


with tempfile.TemporaryDirectory(prefix="qld-native-dynamic-") as temporary:
    root = pathlib.Path(temporary)
    isolated = {**os.environ, "PATH": str(root / "no-tools"),
                "QLD_TOOLCHAIN_BACKEND_ACTIVE": "1"}
    base_source = root / "base.c"
    base_source.write_text(
        "int base_value = 40;\n"
        "int imported(void) { return base_value; }\n"
        "__attribute__((visibility(\"protected\")))"
        " int protected_value(void) { return 3; }\n"
        "__attribute__((visibility(\"hidden\")))"
        " int hidden_value(void) { return 9; }\n")
    base_object = root / "base.o"
    run(cc, "-fPIC", "-fno-stack-protector", "-c", base_source,
        "-o", base_object)
    base = root / "libbase.so"
    run(ld, "--shared", "--soname=libbase-runtime.so", "--hash-style=both",
        base_object, "-o", base, env=isolated)
    assert elf_type(base.read_bytes()) == 3
    os.symlink(base.name, root / "libbase-runtime.so")
    base_library = ctypes.CDLL(str(base))
    assert base_library.protected_value() == 3
    try:
        base_library.hidden_value
        raise AssertionError("hidden symbol was exported")
    except AttributeError:
        pass

    consumer_source = root / "consumer.c"
    consumer_source.write_text(
        "extern int imported(void), base_value;\n"
        "int local_value;\n"
        "int *local_pointer = &local_value;\n"
        "__attribute__((constructor)) static void initialize(void)"
        " { local_value = 2; }\n"
        "int answer(void) { return imported() + base_value"
        " + *local_pointer - 40; }\n")
    consumer_object = root / "consumer.o"
    run(cc, "-fPIC", "-fno-stack-protector", "-c", consumer_source,
        "-o", consumer_object)
    consumer = root / "libconsumer.so"
    command = [ld, "--shared", "--soname=libconsumer.so",
               "--hash-style=both", "--rpath=$ORIGIN",
               consumer_object, "-L", root, "-lbase", "-o", consumer]
    run(*command, env=isolated)
    first = consumer.read_bytes()
    assert b"libbase-runtime.so\0" in first and b"$ORIGIN\0" in first
    run(*command, env=isolated)
    assert consumer.read_bytes() == first, "native DSO output is nondeterministic"
    library = ctypes.CDLL(str(consumer))
    assert library.answer() == 42
    pointer = ctypes.c_void_p.in_dll(library, "local_pointer").value
    value = ctypes.c_int.in_dll(library, "local_value")
    assert pointer == ctypes.addressof(value)
    run(api, consumer_object, env=isolated)

    unused = root / "unused.so"
    run(ld, "--shared", "--as-needed", consumer_object,
        "-L", root, "-lbase", "-o", unused, env=isolated)
    # This object does use base, so the provider remains needed.
    assert b"libbase-runtime.so\0" in unused.read_bytes()
    leaf_source = root / "leaf.c"
    leaf_source.write_text("int leaf(void) { return 7; }\n")
    leaf_object = root / "leaf.o"
    run(cc, "-fPIC", "-c", leaf_source, "-o", leaf_object)
    run(ld, "--shared", "--as-needed", leaf_object,
        "-L", root, "-lbase", "-o", unused, env=isolated)
    assert b"libbase-runtime.so\0" not in unused.read_bytes()
    run(ld, "--shared", "--no-as-needed", leaf_object,
        "-L", root, "-lbase", "-o", unused, env=isolated)
    assert b"libbase-runtime.so\0" in unused.read_bytes()

    weak_source = root / "weak.c"
    weak_source.write_text(
        "extern int optional(void) __attribute__((weak));\n"
        "int maybe(void) { return optional ? optional() : 5; }\n")
    weak_object = root / "weak.o"
    weak_library = root / "libweak.so"
    run(cc, "-fPIC", "-c", weak_source, "-o", weak_object)
    run(ld, "--shared", weak_object, "-o", weak_library, env=isolated)
    assert ctypes.CDLL(str(weak_library)).maybe() == 5
    c_library = root / "libc-native.so"
    run(c_api, leaf_object, c_library, env=isolated)
    assert ctypes.CDLL(str(c_library)).leaf() == 7

    start_source = root / "start.s"
    start_source.write_text(
        ".text\n.global _start\n.type _start,@function\n"
        "_start:\n call answer@PLT\n mov %eax,%edi\n"
        " mov $60,%eax\n syscall\n")
    start = root / "start.o"
    run(cc, "-fPIE", "-c", start_source, "-o", start)
    pie = root / "pie"
    run(ld, "--pie", "-z", "now", "-z", "relro", "--rpath=$ORIGIN",
        start, "-L", root, "-lconsumer", "-o", pie, env=isolated)
    pie_data = pie.read_bytes()
    assert elf_type(pie_data) == 3
    types = program_types(pie_data)
    assert 3 in types and 2 in types and 0x6474E552 in types  # INTERP/DYNAMIC/RELRO
    run(pie, env=isolated, expected=42)

    fixed_object = root / "start-fixed.o"
    run(cc, "-fno-pie", "-c", start_source, "-o", fixed_object)
    fixed = root / "dynamic-exec"
    run(ld, "--rpath=$ORIGIN", fixed_object, "-L", root, "-lconsumer",
        "-o", fixed, env=isolated)
    assert elf_type(fixed.read_bytes()) == 2
    run(fixed, env=isolated, expected=42)

    bad_source = root / "not-pic.c"
    bad_source.write_text("int value; int read_value(void) { return value; }\n")
    bad_object = root / "not-pic.o"
    run(cc, "-fno-pic", "-fno-pie", "-c", bad_source, "-o", bad_object)
    preserved = root / "preserved.so"
    preserved.write_bytes(b"preserved")
    failed = subprocess.run(
        [ld, "--shared", bad_object, "-o", preserved],
        capture_output=True, text=True, env=isolated)
    assert failed.returncode and "recompile as PIC" in failed.stderr, failed.stderr
    assert preserved.read_bytes() == b"preserved"

print("native dynamic/shared/PIE/PIC ELF links passed without toolchain access")
