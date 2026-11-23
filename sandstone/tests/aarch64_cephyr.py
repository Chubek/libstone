"""AAPCS64/Cephyr GAS conformance, with fixed GNU as 2.40 reference bytes."""
import hashlib
from pathlib import Path
import struct
import subprocess


def text_and_relocations(obj):
    data = obj.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 40)
    stride, count, names = struct.unpack_from("<HHH", data, 58)
    sections = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i*stride) for i in range(count)]

    def string(section, offset):
        return data[section[4]+offset:].split(b"\0", 1)[0].decode()

    by_name = {string(sections[names], s[0]): s for s in sections}
    text = by_name[".text"]
    rela = by_name[".rela.text"]
    syms = sections[rela[6]]
    strings = sections[syms[6]]
    rows = []
    for i in range(0, rela[5], rela[9]):
        offset, info, addend = struct.unpack_from("<QQq", data, rela[4]+i)
        index, = struct.unpack_from("<I", data, syms[4]+(info >> 32)*syms[9])
        rows.append((offset, info & 0xffffffff, string(strings, index), addend))
    return data[text[4]:text[4]+text[5]], rows


def check_aarch64_cephyr(assembler, bank, directory):
    fixtures = Path(__file__).parent / "fixtures/aarch64"
    obj = Path(directory) / "cephyr-aapcs64.o"
    def assemble(source):
        subprocess.run([assembler, "--bank", bank, "-f", "elf", "-o", obj, source], check=True)
        return text_and_relocations(obj)

    # Unmodified -S output for test_cephyr_c11_aarch64_long_double.py's
    # mixed callback/strtold/variadic translation unit (including its .set).
    text, relocs = assemble(fixtures / "cephyr-mixed-quad.s")
    assert len(text) == 6908
    assert hashlib.sha256(text).hexdigest() == "79a6b789a342c46e6a8bc105030e9bb5cdd1fe54a085bd4531f88b33a3880af4"
    assert len(relocs) == 30 and (0x8a4, 283, "external_var", 0) in relocs

    text, relocs = assemble(fixtures / "cephyr-forms.s")
    assert text == bytes.fromhex("""
ffc30ad1ff0740d1ff0300f9ff434091ec632c8bfdff3f91e00309aaea0f8012
0a008052e9ff9fd28946a2f2e9ffdff20b00f0d229010a8b29010acb29010aab
29010aeb297d0a9b2b7d4a9b29010a8a29010aaa29010aca290161d2290141d2
2921ca9a2925ca9a2929ca9a290141d3297d40d3291d40933f010beb7ffd89eb
5f0100f15f0500b1e9179f9ae9a79f9ae9279f9ae9579f9ae9879f9a69119fda
2b0dca9a2b09ca9a69a50a9bc7000054a000005461faff544afaffb502000014
000020d429018039290140392901807929014079290180b9290140b92a010039
2a0100792a0100b9e9ff7ff9e9ff3ff9e7ffff3de0ffbf3de7ff7fbde1ff7ffd
4101271e4101679e0900261e0900669e0028211e0038211e0008211e0018211e
0028611e0038611e0008611e0018611e0020211e1020211e0020611e1020611e
00c0221e0040621e0900389e0900399e0900789e0900799e2001229e2001239e
2001629e2001639e090000902901009109000090290140f900000094a0013fd6
c0035fd6""")
    assert relocs == [(0x168,275,"obj",0), (0x16c,277,"obj",0), (0x170,311,"obj",0),
                      (0x174,312,"obj",0), (0x178,283,"x0",0)]

    source = Path(directory) / "bad-aarch64.s"
    for insn in ("ldr q0,[sp,#8]", "str x0,[xzr]", "ldr x0,[sp,#32768]",
                 "fmov s0,x9", "fcvt s0,s1", "movk x0,#1,lsl #15", "sub sp,sp,#4096",
                 "eor x0,x0,#3", "cset x0,typo", "add x9,x9,:unknown:foo"):
        source.write_text(insn + "\n")
        p = subprocess.run([assembler, "--bank", bank, "-f", "elf", "-o", obj, source],
                           capture_output=True)
        assert p.returncode, insn
