#!/usr/bin/env python3
"""Native codecs/ELF readers: strict C99, deterministic generation and hostile input.

LUA, CC, CFLAGS are overridable, as in VMCollection/test_collection.py.
Use --all to run that existing VM suite too. No binutils build is required.
"""
import argparse
import ctypes as C
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
LUA = shlex.split(os.environ.get("LUA", "lua5.4"))
CC = shlex.split(os.environ.get("CC", "cc"))
FLAGS = ["-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic"] + shlex.split(os.environ.get("CFLAGS", ""))
RNG = random.Random(1907)


def run(args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


class Operand(C.Structure):
    _fields_ = [("name", C.c_char_p)] + [(n, C.c_uint) for n in ("shift", "bits", "signed", "scale")]


class Instruction(C.Structure):
    _fields_ = [("name", C.c_char_p), ("match", C.c_uint32), ("mask", C.c_uint32),
                ("count", C.c_size_t), ("operands", Operand * 6)]


class Decoded(C.Structure):
    _fields_ = [("instruction", C.POINTER(Instruction)), ("count", C.c_size_t), ("values", C.c_int64 * 6)]


class View(C.Structure):
    _fields_ = [("data", C.c_void_p), ("size", C.c_size_t)] + [
        (n, C.c_uint64) for n in ("entry", "phoff", "shoff", "phnum", "shnum", "shstrndx")
    ] + [("flags", C.c_uint32)] + [(n, C.c_uint16) for n in ("type", "machine", "phentsize", "shentsize")]


class Section(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ("name", "type", "link", "info")] + [
        (n, C.c_uint64) for n in ("flags", "address", "offset", "size", "alignment", "entsize")]


class Segment(C.Structure):
    _fields_ = [("type", C.c_uint32), ("flags", C.c_uint32)] + [
        (n, C.c_uint64) for n in ("offset", "vaddr", "paddr", "filesz", "memsz", "alignment")]


class Target(C.Structure):
    _fields_ = [(n, C.c_char_p) for n in ("target", "arch", "source")] + [
        (n, C.c_uint) for n in ("bits", "little", "machine")]


def function(lib, name, result, *args):
    f = getattr(lib, name)
    f.restype, f.argtypes = result, list(args)
    return f


def arch_api(lib, name):
    return {
        "words": function(lib, name + "_instructions", C.POINTER(Instruction), C.POINTER(C.c_size_t)),
        "encode": function(lib, name + "_encode_word", C.c_int, C.c_char_p, C.POINTER(C.c_int64), C.c_size_t, C.POINTER(C.c_uint32)),
        "decode": function(lib, name + "_decode_word", C.c_int, C.c_uint32, C.POINTER(Decoded)),
        "encode_bytes": function(lib, name + "_encode", C.c_int, C.c_char_p, C.POINTER(C.c_int64), C.c_size_t, C.c_void_p, C.c_size_t, C.c_int),
        "decode_bytes": function(lib, name + "_decode", C.c_int, C.c_void_p, C.c_size_t, C.c_int, C.POINTER(Decoded)),
    }


GOLDEN = {
    "MIPS32": [("addu", [2, 3, 4], 0x00641021), ("addiu", [8, 0, -1], 0x2408FFFF),
               ("beq", [1, 2, -4], 0x1022FFFF), ("sll", [2, 3, 4], 0x00031100),
               ("j", [0x1000], 0x08000400), ("jr", [31], 0x03E00008)],
    "PowerPC32": [("addi", [3, 0, 42], 0x3860002A), ("add", [3, 4, 5], 0x7C642A14),
                  ("ori", [3, 4, 0x1234], 0x60831234), ("b", [-4], 0x4BFFFFFC), ("blr", [], 0x4E800020)],
    "SPARC32": [("add.r", [1, 2, 3], 0x82008003), ("add.i", [1, 2, -1], 0x8200BFFF),
                ("sethi", [1, 0x12345], 0x03012345), ("call", [-4], 0x7FFFFFFF),
                ("ba", [-4], 0x10BFFFFF), ("st.i", [1, 2, 4], 0xC220A004)],
    "Edge": [("edge", [-(1 << 30) * 8], 0x40000000), ("edge", [((1 << 30) - 1) * 8], 0x3FFFFFFF)],
}


def test_arch(lib, name):
    api = arch_api(lib, name)
    for mnemonic, values, expected in GOLDEN[name]:
        values = (C.c_int64 * len(values))(*values)
        word = C.c_uint32(0xDEADBEEF)
        assert api["encode"](mnemonic.encode(), values, len(values), C.byref(word)) == 0
        assert word.value == expected, (name, mnemonic, hex(word.value), hex(expected))
        for little in (0, 1):
            data = C.create_string_buffer(4)
            assert api["encode_bytes"](mnemonic.encode(), values, len(values), data, 4, little) == 0
            assert data.raw == expected.to_bytes(4, "little" if little else "big")
            decoded = Decoded()
            assert api["decode_bytes"](data, 4, little, C.byref(decoded)) == 0
            assert decoded.instruction.contents.name == mnemonic.encode()
            assert list(decoded.values)[:decoded.count] == list(values)
            before = bytes(decoded)
            for size in range(4):
                assert api["decode_bytes"](data, size, little, C.byref(decoded)) == -2
                assert bytes(decoded) == before
            assert api["decode_bytes"](data, 4, 2, C.byref(decoded)) == -3
            assert api["decode_bytes"](None, 4, little, C.byref(decoded)) == -3
    count = C.c_size_t()
    words = api["words"](C.byref(count))
    assert count.value > 0
    for index in range(count.value):
        word = words[index]
        fields = list(word.operands)[:word.count]
        limits = [(-(1 << (f.bits - 1)) if f.signed else 0,
                   (1 << (f.bits - (1 if f.signed else 0))) - 1) for f in fields]
        for trial in range(40):
            values = [(lo if trial == 0 else hi if trial == 1 else RNG.randint(lo, hi)) * f.scale
                      for f, (lo, hi) in zip(fields, limits)]
            array = (C.c_int64 * len(values))(*values)
            encoded, decoded = C.c_uint32(), Decoded()
            assert api["encode"](word.name, array, len(values), C.byref(encoded)) == 0
            assert api["decode"](encoded, C.byref(decoded)) == 0
            assert decoded.instruction.contents.name == word.name
            assert list(decoded.values)[:decoded.count] == values
        for index, (field, (lo, hi)) in enumerate(zip(fields, limits)):
            for invalid in ((lo - 1) * field.scale, (hi + 1) * field.scale, -(1 << 63), (1 << 63) - 1):
                values = [0] * len(fields); values[index] = invalid
                array = (C.c_int64 * len(values))(*values)
                encoded = C.c_uint32(0xDEADBEEF)
                assert api["encode"](word.name, array, len(values), C.byref(encoded)) == -3
                assert encoded.value == 0xDEADBEEF
            if field.scale > 1:
                values = [0] * len(fields); values[index] = 1
                array = (C.c_int64 * len(values))(*values)
                assert api["encode"](word.name, array, len(values), C.byref(encoded)) == -3
        assert api["encode"](word.name, None, 1, C.byref(C.c_uint32())) == -3
    for _ in range(2000):
        raw = RNG.getrandbits(32)
        decoded = Decoded(); before = bytes(decoded)
        status = api["decode"](raw, C.byref(decoded))
        assert status in (0, -1)
        if status == 0:
            encoded = C.c_uint32()
            assert api["encode"](decoded.instruction.contents.name, decoded.values, decoded.count, C.byref(encoded)) == 0
            assert encoded.value == raw
        else:
            assert bytes(decoded) == before
    assert api["encode"](b"missing", None, 0, C.byref(C.c_uint32())) == -1
    assert api["encode"](None, None, 0, C.byref(C.c_uint32())) == -3
    assert api["decode"](0, None) == -3
    print(f"PASS {name}: golden vectors, all operand edges, 2000 random words")


def elf_api(lib, name):
    return {
        "info": function(lib, name + "_info", C.POINTER(Target)),
        "open": function(lib, name + "_open", C.c_int, C.c_void_p, C.c_size_t, C.POINTER(View)),
        "section": function(lib, name + "_read_section", C.c_int, C.POINTER(View), C.c_uint64, C.POINTER(Section)),
        "segment": function(lib, name + "_read_segment", C.c_int, C.POINTER(View), C.c_uint64, C.POINTER(Segment)),
        "data": function(lib, name + "_section_data", C.c_int, C.POINTER(View), C.c_uint64, C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)),
        "name": function(lib, name + "_section_name", C.c_int, C.POINTER(View), C.c_uint64, C.POINTER(C.c_void_p), C.POINTER(C.c_size_t)),
    }


def fixture(target, extended=False):
    """Independent ELF external layout (include/elf/external.h)."""
    bits, endian = target.bits, "<" if target.little else ">"
    eh, ph, sh = (64, 56, 64) if bits == 64 else (52, 32, 40)
    shoff = eh + ph
    textoff = shoff + 4 * sh
    names = b"\0.text\0.shstrtab\0.bss\0"
    strsoff = textoff + 4
    image = bytearray(strsoff + len(names))
    image[:16] = b"\x7fELF" + bytes([2 if bits == 64 else 1, 1 if target.little else 2, 1]) + bytes(9)
    hdr = (1, target.machine, 1, 0x1000, eh, shoff, 0, eh, ph, 0xFFFF if extended else 1,
           sh, 0 if extended else 4, 0xFFFF if extended else 2)
    struct.pack_into(endian + ("HHIQQQIHHHHHH" if bits == 64 else "HHIIIIIHHHHHH"), image, 16, *hdr)
    if bits == 64:
        struct.pack_into(endian + "IIQQQQQQ", image, eh, 1, 5, textoff, 0x1000, 0x1000, 4, 4, 4)
    else:
        struct.pack_into(endian + "IIIIIIII", image, eh, 1, textoff, 0x1000, 0x1000, 4, 4, 5, 4)
    sections = [
        (0, 0, 0, 0, 0, 4 if extended else 0, 2 if extended else 0, 1 if extended else 0, 0, 0),
        (1, 1, 6, 0x1000, textoff, 4, 0, 0, 4, 0),
        (7, 3, 0, 0, strsoff, len(names), 0, 0, 1, 0),
        (17, 8, 3, 0x2000, 0xFFFFFFF0, 8192, 0, 0, 16, 0),
    ]
    for index, item in enumerate(sections):
        struct.pack_into(endian + ("IIQQQQIIQQ" if bits == 64 else "IIIIIIIIII"), image, shoff + index * sh, *item)
    arch = {b"mips": "MIPS32", b"powerpc": "PowerPC32", b"sparc": "SPARC32"}.get(target.arch)
    word = GOLDEN[arch][0][2] if arch else 0
    image[textoff:textoff + 4] = word.to_bytes(4, "little" if target.little else "big")
    image[strsoff:] = names
    return image, dict(eh=eh, ph=ph, sh=sh, shoff=shoff, textoff=textoff, strsoff=strsoff)


def opened(api, image, expected=0):
    data = C.create_string_buffer(bytes(image), len(image))
    view = View(); C.memset(C.byref(view), 0xA5, C.sizeof(view))
    before = bytes(view)
    result = api["open"](data, len(image), C.byref(view))
    assert result == expected, (result, expected)
    if result:
        assert bytes(view) == before, "open changed output on error"
    return data, view


def test_elf(lib, name):
    api = elf_api(lib, name)
    target = api["info"]().contents
    for extended in (False, True):
        image, layout = fixture(target, extended)
        data, view = opened(api, image)
        assert (view.shnum, view.phnum, view.shstrndx, view.entry) == (4, 1, 2, 0x1000)
        for index, expected_name in enumerate((b"", b".text", b".shstrtab", b".bss")):
            pointer, size = C.c_void_p(), C.c_size_t()
            section = Section()
            assert api["section"](C.byref(view), index, C.byref(section)) == 0
            assert api["name"](C.byref(view), index, C.byref(pointer), C.byref(size)) == 0
            assert C.string_at(pointer, size.value) == expected_name
            assert api["data"](C.byref(view), index, C.byref(pointer), C.byref(size)) == 0
            if index in (0, 3):
                assert pointer.value is None and size.value == 0
            if index == 1:
                assert C.string_at(pointer, size.value) == image[layout["textoff"]:layout["textoff"] + 4]
                arch = {b"mips": "MIPS32", b"powerpc": "PowerPC32", b"sparc": "SPARC32"}.get(target.arch)
                if arch:
                    decoded = Decoded()
                    assert arch_api(lib, arch)["decode_bytes"](pointer, size, target.little, C.byref(decoded)) == 0
                    assert decoded.instruction.contents.name == GOLDEN[arch][0][0].encode()
        segment = Segment()
        assert api["segment"](C.byref(view), 0, C.byref(segment)) == 0
        assert (segment.type, segment.flags, segment.filesz, segment.memsz) == (1, 5, 4, 4)
        assert api["section"](C.byref(view), 4, C.byref(Section())) == -3
        assert api["segment"](C.byref(view), 1, C.byref(Segment())) == -3
        assert api["open"](None, len(image), C.byref(view)) == -3
        for length in range(len(image)):
            opened(api, image[:length], -2)
    image, layout = fixture(target)
    endian = "<" if target.little else ">"

    def changed(offset, fmt, value, expected):
        corrupt = image.copy(); struct.pack_into(endian + fmt, corrupt, offset, value)
        opened(api, corrupt, expected)

    changed(4, "B", 1 if target.bits == 64 else 2, -1)
    changed(5, "B", 2 if target.little else 1, -1)
    changed(6, "B", 0, -3)
    changed(18, "H", 0xFFFF, -1)
    changed(20, "I", 0, -3)
    changed(16, "H", 0, -3)
    changed(52 if target.bits == 64 else 40, "H", 1, -3)
    changed(40 if target.bits == 64 else 32, "Q" if target.bits == 64 else "I", (1 << target.bits) - 1, -2)
    changed(layout["shoff"] + layout["sh"] + (48 if target.bits == 64 else 32),
            "Q" if target.bits == 64 else "I", 3, -3)
    changed(layout["eh"] + (32 if target.bits == 64 else 16),
            "Q" if target.bits == 64 else "I", 5, -3)  # PT_LOAD filesz > memsz
    changed(layout["shoff"] + layout["sh"] + (24 if target.bits == 64 else 16),
            "Q" if target.bits == 64 else "I", (1 << target.bits) - 1, -2)
    # String termination is checked on access, not during structural open.
    corrupt = image.copy(); corrupt[layout["strsoff"]:] = b"x" * (len(corrupt) - layout["strsoff"])
    data, view = opened(api, corrupt)
    pointer, size = C.c_void_p(123), C.c_size_t(456)
    assert api["name"](C.byref(view), 1, C.byref(pointer), C.byref(size)) == -3
    assert (pointer.value, size.value) == (123, 456)
    # A valid sectionless ELF, including ET_EXEC/ET_DYN/ET_CORE.
    header = image[:layout["eh"]]
    for off, fmt in ((40, "Q"), (32, "Q"), (56, "H"), (60, "H"), (62, "H")) if target.bits == 64 else (
            (32, "I"), (28, "I"), (44, "H"), (48, "H"), (50, "H")):
        struct.pack_into(endian + fmt, header, off, 0)
    for kind in (1, 2, 3, 4):
        struct.pack_into(endian + "H", header, 16, kind)
        data, view = opened(api, header)
        assert view.shnum == 0 and view.phnum == 0
    # Mutation smoke-fuzzing: structural failures must never publish a view.
    for _ in range(500):
        corrupt = image.copy()
        for _ in range(RNG.randint(1, 5)):
            corrupt[RNG.randrange(len(corrupt))] = RNG.randrange(256)
        data = C.create_string_buffer(bytes(corrupt), len(corrupt))
        view = View(); before = bytes(view)
        status = api["open"](data, len(corrupt), C.byref(view))
        assert status in (0, -1, -2, -3)
        if status:
            assert bytes(view) == before
        else:
            for index in range(view.shnum):
                assert api["section"](C.byref(view), index, C.byref(Section())) == 0
                assert api["name"](C.byref(view), index, C.byref(pointer), C.byref(size)) in (0, -3, -4)
    print(f"PASS {name}: ELF layouts, extended numbering, truncations, 500 mutated files")


def test_reference(lib, bfd_names):
    reference = ROOT / ".info/binutils"
    assert "20111121" in (reference / "bfd/version.h").read_text()
    constants = (reference / "include/elf/common.h").read_text()
    for name in bfd_names:
        info = elf_api(lib, name)["info"]().contents
        source = info.source.decode()
        backend = source.split(";", 1)[0]
        assert '"' + info.target.decode() + '"' in (ROOT / backend).read_text()
        symbol = re.search(r"\((EM_[A-Z0-9_]+)\)", source).group(1)
        match = re.search(r"^#define\s+" + symbol + r"\s+(\d+)\b", constants, re.M)
        assert match and int(match[1]) == info.machine
    # Every MIPS match/mask must be an actual, non-macro binutils table entry.
    table = (reference / "opcodes/mips-opc.c").read_text()
    entries = {(n.encode(), int(m, 16), int(k, 16)) for n, m, k in re.findall(
        r'\{"([^"]+)",\s*"[^"]*",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+)', table)}
    api = arch_api(lib, "MIPS32"); count = C.c_size_t(); words = api["words"](C.byref(count))
    for i in range(count.value):
        assert (words[i].name, words[i].match, words[i].mask) in entries
    print("PASS local binutils reference: target names, EM_* constants, MIPS match/mask tables")


def test_regeneration(temp, collection):
    here = ROOT / collection
    script = here / "regenerate.lua"
    extension = "archw" if collection == "ArchCollection" else "bfdw"
    names = sorted(p.name for p in here.iterdir() if p.is_dir() and (p / (p.name + "." + extension)).exists())
    run(LUA + [str(script), "--check"], cwd=temp)
    run(LUA + ["regenerate.lua", "--check", names[0]], cwd=here)
    listed = run(LUA + [str(script), "--list"], capture_output=True, text=True)
    assert listed.stdout.splitlines() == names
    for option in ("MissingProfile", "--bad"):
        assert subprocess.run(LUA + [str(script), option], capture_output=True).returncode != 0
    copy = temp / collection
    shutil.copytree(here, copy)
    path = copy / names[0] / (names[0] + ".h")
    path.write_text("stale\n")
    result = subprocess.run(LUA + [str(copy / "regenerate.lua"), "--check", names[0]], capture_output=True)
    assert result.returncode != 0 and path.read_text() == "stale\n"
    path.unlink()
    result = subprocess.run(LUA + [str(copy / "regenerate.lua"), "--check", names[0]], capture_output=True)
    assert result.returncode != 0 and not path.exists()
    run(LUA + [str(copy / "regenerate.lua")], cwd=temp)
    for name in names:
        for suffix in ("h", "c"):
            rel = Path(name) / (name + "." + suffix)
            assert (copy / rel).read_bytes() == (here / rel).read_bytes()
    return names


def test_real_object(lib, temp):
    source, obj = temp / "answer.c", temp / "answer.o"
    source.write_text("int answer(void) { return 42; }\n")
    run(CC + FLAGS + ["-c", str(source), "-o", str(obj)])
    image = obj.read_bytes()
    if image[:6] != b"\x7fELF\x02\x01" or image[18:20] != b"\x3e\x00":
        return  # The portable synthetic tests still cover all targets.
    api = elf_api(lib, "ELF64_X86_64_LE")
    data, view = opened(api, image)
    names = []
    for index in range(view.shnum):
        pointer, size = C.c_void_p(), C.c_size_t()
        assert api["name"](C.byref(view), index, C.byref(pointer), C.byref(size)) == 0
        names.append(C.string_at(pointer, size.value).decode())
    assert ".text" in names and ".symtab" in names
    if shutil.which("readelf"):
        output = run(["readelf", "-SW", str(obj)], capture_output=True, text=True).stdout
        assert all(name in output for name in names if name)
    print("PASS host compiler ELF object and readelf section names")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true", help="also run VMCollection tests")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="weavers-", dir="/tmp/opencode") as work:
        temp = Path(work)
        shutil.copytree(ROOT / "Weavers", temp / "Weavers")
        arch_names = test_regeneration(temp, "ArchCollection")
        bfd_names = test_regeneration(temp, "BFDCollection")
        assert set(arch_names) == set(GOLDEN) - {"Edge"}, "missing architecture test vectors"
        paths = ";".join(str(ROOT / "Weavers" / name / "?.lua") for name in ("ArchWeave", "BFDWeave")) + ";;"
        run(LUA + [str(ROOT / "Weavers/test_weavers.lua"), str(temp)], env={**os.environ, "LUA_PATH": paths})
        sources = [p for name in ("ArchCollection", "BFDCollection") for p in (ROOT / name).glob("*/*.c")]
        sources.append(temp / "Edge.c")
        run(CC + FLAGS + ["-fPIC", "-shared"] + [str(p) for p in sources] + ["-o", str(temp / "collections.so")])
        lib = C.CDLL(str(temp / "collections.so"))
        for name in arch_names + ["Edge"]:
            test_arch(lib, name)
        for name in bfd_names:
            test_elf(lib, name)
        test_reference(lib, bfd_names)
        test_real_object(lib, temp)
        headers = [f'#include "{p.relative_to(ROOT)}"' for name in ("ArchCollection", "BFDCollection", "VMCollection")
                   for p in sorted((ROOT / name).glob("*/*.h"))]
        header_test = temp / "headers.c"
        header_test.write_text("\n".join(headers) + "\nint main(void) { return 0; }\n")
        run(CC + FLAGS + ["-I", str(ROOT), str(header_test), "-o", str(temp / "headers")])
        if shutil.which("c++"):
            run(["c++", "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", "-x", "c++", "-I", str(ROOT),
                 str(header_test), "-o", str(temp / "headers-cxx")])
    if args.all:
        run([os.sys.executable, str(ROOT / "VMCollection/test_collection.py")])
    print(f"All native collections passed ({len(arch_names)} ISA codecs, {len(bfd_names)} ELF targets).")


if __name__ == "__main__":
    main()
