"""End-to-end pipeline test: .dsymspec -> .symas -> assembled bytes.

Drives scripts/spec-compiler.py (Lua DSL step) plus the freshly built
sandstone-spec-compiler binary, then assembles a small source with the
freshly built sandstone-as CLI and checks the emitted bytes.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import struct
import sys
import tempfile


X86_SOURCE = """\
.text
.globl _start
_start:
    mov eax, ebx
    nop
    call target
    ret
target:
    nop
"""

RISCV_SOURCE = """\
.text
.globl _start
_start:
    add x1, x2, x3
    nop
    jalr x1, x2, 0
target:
    nop
"""

RISCV_JAL_SOURCE = """\
.text
_start:
    jal x1, target
target:
    nop
"""


def run(cmd, **kwargs):
    proc = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if proc.returncode != 0:
        raise AssertionError(
            "command failed: %s\nstdout: %s\nstderr: %s"
            % (" ".join(cmd), proc.stdout, proc.stderr)
        )
    return proc


def check_elf(assembler, bank, path, branch, machine, branch_type, addend):
    """Decode the file independently, including links between ELF tables."""
    source = f""".text
.global entry
.type entry, %function
entry:
{branch}
.size entry, .-entry
.section .slots,"aw",%progbits
.byte 1
localptr:
.quad external+8, localptr-4
.section .reserve,"aw",%nobits
.balign 64
.space 65536
"""
    assemble(assembler, bank, source, path, fmt="elf")
    data = Path(path).read_bytes()
    assert data[:7] == b"\x7fELF\x02\x01\x01" and len(data) < 2048
    assert struct.unpack_from("<HH", data, 16) == (1, machine)
    assert struct.unpack_from("<I", data, 48)[0] == 0
    shoff, = struct.unpack_from("<Q", data, 40)
    entsize, count, shstridx = struct.unpack_from("<HHH", data, 58)
    headers = [struct.unpack_from("<IIQQQQIIQQ", data, shoff + i*entsize)
               for i in range(count)]
    def string(table, offset):
        return data[table[4]+offset:].split(b"\0", 1)[0].decode()
    sections = {string(headers[shstridx], h[0]): h for h in headers}
    assert sections[".reserve"][1:3] == (8, 3)
    assert sections[".reserve"][5] == 65536 and sections[".reserve"][8] == 64
    assert sections[".text"][1:3] == (1, 6)
    symtab = sections[".symtab"]
    names = headers[symtab[6]]
    symbols = []
    for i in range(0, symtab[5], symtab[9]):
        name, info, _, sec, value, size = struct.unpack_from("<IBBHQQ", data, symtab[4]+i)
        symbols.append((string(names, name), info, sec, value, size))
    assert symbols[0] == ("", 0, 0, 0, 0)
    assert symbols[1] == ("localptr", 0, headers.index(sections[".slots"]), 1, 0)
    assert symbols[symtab[7]:] == [
        ("entry", 18, headers.index(sections[".text"]), 0, sections[".text"][5]),
        ("external", 16, 0, 0, 0)]
    def relocs(name):
        h = sections[".rela"+name]
        assert h[1] == 4 and headers[h[6]] == symtab and headers[h[7]] == sections[name]
        rows = []
        for i in range(0, h[5], h[9]):
            offset, info, a = struct.unpack_from("<QQq", data, h[4]+i)
            rows.append((offset, symbols[info >> 32][0], info & 0xffffffff, a))
        return rows
    assert relocs(".text") == [(int(machine == 62), "external", branch_type, addend)]
    absolute = {62: 1, 183: 257, 243: 2}[machine]
    assert relocs(".slots") == [(1, "external", absolute, 8), (9, "localptr", absolute, -4)]


def find_binary(build_dir, name):
    for root, _dirs, files in os.walk(build_dir):
        if name in files:
            return os.path.join(root, name)
    raise AssertionError("binary %s not found under %s" % (name, build_dir))


def check_amd64_att(assembler, bank, tmp):
    """Golden bytes checked against independent GNU as, not re-encoded by the test."""
    root = Path(tmp)
    source = """movq %r15,-128(%r12,%r9,8)
movzwl 128(%r13),%r10d
movabsq $0xffffffffffffffff,%r11
movb %sil,(%rbp)
leaq 12(,%r12,4),%r15
movq %xmm8,%r9
cvttsd2siq %xmm11,%r14
fsubrp %st,%st(1)
fdivrp %st,%st(1)
fucomip %st(1),%st
fnstcw 8(%rsp)
jmp *%r11
movq $7,target(%rip)
call alias@PLT
movq target@GOTPCREL(%rip),%r8
.set alias, target
"""
    obj = root / "forms.o"
    assemble(assembler, bank, source, str(obj), syntax="att", fmt="elf")
    data = obj.read_bytes()
    shoff, = struct.unpack_from("<Q", data, 40)
    # Sections in this fixture are null, .text, .rela.text, .symtab, ...
    def content(index):
        offset, size = struct.unpack_from("<QQ", data, shoff+64*index+24)
        return data[offset:offset+size]
    assert content(1) == bytes.fromhex("""
        4f897ccc80 450fb79580000000 49bbffffffffffffffff 40887500
        4e8d3ca50c000000 664d0f7ec1 f24d0f2cf3 dee9 def9 dfe9 d97c2408 41ffe3
        48c7050000000007000000 e800000000 4c8b0500000000""")
    relas = list(struct.iter_unpack("<QQq",content(2)))
    assert [(p,info & 0xffffffff,a) for p,info,a in relas] == [
        (0x3d,2,-8),(0x46,4,-4),(0x4d,9,-4)]
    assert len(set(info >> 32 for _,info,_ in relas)) == 1
    assert b"target\0" in content(4) and b"alias\0" not in content(4)
    for invalid in [
        "movq %xmm1,%eax", "movq %eax,%rax", "movq $0x100000000,(%rax)",
        "movq $0x10000000000000000,%rax", "movq %rax,2147483648(%rax)",
        "movq (%rax,%rcx,3),%rdx", "movq (%rax,%rsp,4),%rdx", "movq (%rip,%rax),%rdx",
        "movq (%eax),%rax", "movb %ah,%r8b", "shrq %dl,%rax", "call %r11", "movq *%rax,%rdx",
        "movq %rax,,%rdx", "movq (%rsp,%rax", "fstpt %rax", "fsubp %st(2),%st(1)",
    ]:
        src = root / "invalid-att.s"
        src.write_text(invalid+"\n")
        bad = root / "bad-att.o"
        p = subprocess.run([assembler,"-b",bank,"-s","att","-f","elf","-o",bad,src],
                           capture_output=True)
        assert p.returncode != 0 and not bad.exists(), invalid


def build_bank(source_dir, compiler_bin, spec, out_path):
    script = os.path.join(source_dir, "scripts", "spec-compiler.py")
    run([sys.executable, script, "--compiler", compiler_bin,
         "--validate", "-o", out_path, spec])


def assemble(asm_bin, bank, source, out_path, syntax=None, fmt="bin"):
    with tempfile.NamedTemporaryFile("w", suffix=".s",
                                     delete=False) as f:
        f.write(source)
        src_path = f.name
    try:
        cmd = [asm_bin, "--bank", bank, "-f", fmt, "-o", out_path, src_path]
        if syntax:
            cmd += ["--syntax", syntax]
        run(cmd)
    finally:
        os.unlink(src_path)


def check_asm_opt(asm_bin, bank, tmp):
    with tempfile.TemporaryDirectory(dir=tmp, prefix="asm-opt-") as work:
        root = Path(work)
        (root / "src").mkdir()
        (root / "src/gap.s").write_text(".macro gap\n    nop\n    nop\n.endm\n",
                                       encoding="utf-8")
        (root / "src/main.s").write_text('.include "gap.s"\n.text\ncall target\n    gap\n',
                                        encoding="utf-8")
        (root / "tail.s").write_text("target:\n    ret\n", encoding="utf-8")
        (root / "pre.lua").write_text(r'''
function preprocess(source, name)
    assert(name == 'src/main.s', name)
    assert(source:find('.include "gap.s"', 1, true))
    assert(asm_context == nil)
    before_opt = true
    return source .. "\n<: assert(before_opt); lsandas.inject('.byte 42\\n') :>\n"
end
''', encoding="utf-8")
        (root / "opt.lua").write_text(r'''
function asm_optimize(source, name)
    assert(name == 'src/main.s', name)
    assert(not before_opt and not lsandas)
    assert(asm_context.arch == 'x86' and asm_context.syntax == 'intel')
    assert(asm_context.riscv64_nop_alias == false)
    assert(not source:find('gap') and not source:find('<:', 1, true))
    assert(source:find('.byte 42', 1, true))
    local output, count = source:gsub('\n    nop\n    nop\n', '\n    nop\n')
    assert(count == 1)
    return output
end
''', encoding="utf-8")
        cmd = [os.path.abspath(asm_bin), "--bank", bank, "--syntax", "intel", "--lua", "pre.lua"]
        inputs = ["src/main.s", "tail.s"]
        run(cmd + ["-o", "out.bin"] + inputs, cwd=root, timeout=30)
        output = root / "out.bin"
        assert output.read_bytes() == bytes.fromhex("e8020000009090c32a")
        run(cmd + ["--asm-opt", "opt.lua", "-o", "out.bin"] + inputs, cwd=root, timeout=30)
        # Removing a macro-expanded NOP shifts the branch target and its rel32.
        optimized = bytes.fromhex("e80100000090c32a")
        assert output.read_bytes() == optimized

        (root / "reject.lua").write_text(r'''
function asm_optimize(source)
    return source .. '\0.byte 0\n'
end
''', encoding="utf-8")
        proc = subprocess.run(cmd + ["--asm-opt", "reject.lua", "-o", "out.bin"] + inputs, cwd=root,
                              capture_output=True, text=True, timeout=30)
        assert proc.returncode != 0 and "NUL in asm_optimize output" in proc.stderr, proc
        assert output.read_bytes() == optimized, "rejection changed existing output"

        (root / "bad.lua").write_text("function asm_optimize(", encoding="utf-8")
        proc = subprocess.run(cmd + ["--asm-opt", "bad.lua", "-o", "bad.bin"] + inputs, cwd=root,
                              capture_output=True, text=True, timeout=30)
        assert proc.returncode != 0 and "asm-opt 'bad.lua'" in proc.stderr, proc
        assert not (root / "bad.bin").exists(), "invalid script produced output"


def check_riscv_asm_context(asm_bin, bank, tmp):
    root = Path(tmp)
    source = root / "context.s"
    source.write_text(".macro zero\naddi x0, x0, 0\n.endm\nzero\n", encoding="utf-8")
    opt = root / "context.lua"
    output = root / "context.bin"
    for syntax, args in [("default", []), ("abi", ["--syntax", "abi"])]:
        opt.write_text(r'''
function asm_optimize(source, name)
    assert(asm_context.arch == 'riscv64')
    assert(asm_context.syntax == '%s')
    assert(asm_context.riscv64_nop_alias == true)
    local expanded, n = source:gsub('addi x0, x0, 0', 'nop')
    assert(n == 1)
    return expanded .. (asm_context.syntax == 'default' and '.byte 1\n' or '.byte 0\n')
end
''' % syntax, encoding="utf-8")
        run([asm_bin, "--bank", bank, "--asm-opt", str(opt), "-o", str(output)] + args + [str(source)],
            timeout=30)
        # The marker proves the hook saw each syntax, in addition to the NOP
        # verified against the real bank and assembled from the expanded macro.
        assert output.read_bytes() == bytes.fromhex("13000000") + bytes([syntax == "default"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--source-dir", required=True)
    args = parser.parse_args()

    compiler_bin = find_binary(args.build_dir, "sandstone-spec-compiler")
    asm_bin = find_binary(args.build_dir, "sandstone-as")
    tmp = tempfile.mkdtemp(prefix="sandstone-pipeline-")

    # x86 (Intel syntax): mov r32,r32 = 0x89 /r, nop, call rel32, ret.
    x86_spec = os.path.join(args.source_dir, "compiler", "stdspec",
                            "x86.dsymspec")
    x86_bank = os.path.join(tmp, "x86.symas")
    build_bank(args.source_dir, compiler_bin, x86_spec, x86_bank)
    x86_out = os.path.join(tmp, "x86.bin")
    assemble(asm_bin, x86_bank, X86_SOURCE, x86_out, syntax="intel")
    with open(x86_out, "rb") as f:
        code = f.read()
    # mov eax,ebx -> 89 d8 ; nop -> 90 ; call +6 -> e8 06 00 00 00 ;
    # ret -> c3 ; nop -> 90
    assert code[:3] == bytes([0x89, 0xD8, 0x90]), code.hex()
    assert code[3] == 0xE8, code.hex()
    disp = int.from_bytes(code[4:8], "little", signed=True)
    assert disp == 1, (code.hex(), disp)  # target is 1 byte past next insn
    assert code[8:] == bytes([0xC3, 0x90]), code.hex()

    check_asm_opt(asm_bin, x86_bank, tmp)

    # Same bank through the obj (JSON) output: one kept relocation is
    # impossible here (all labels local), so relocs must be empty and the
    # symbol table must carry _start/target.
    x86_obj = os.path.join(tmp, "x86.obj.json")
    assemble(asm_bin, x86_bank, X86_SOURCE, x86_obj, syntax="intel",
             fmt="obj")
    with open(x86_obj, encoding="utf-8") as f:
        obj = json.load(f)
    assert obj["format"] == "sandstone-obj-1"
    names = {s["name"] for s in obj["symbols"]}
    assert {"_start", "target"} <= names, names
    assert obj["relocs"] == [], obj["relocs"]

    # External symbol keeps a relocation in obj output but fails in bin.
    ext_src = ".text\ncall puts\n"
    ext_obj = os.path.join(tmp, "ext.obj.json")
    assemble(asm_bin, x86_bank, ext_src, ext_obj, syntax="intel", fmt="obj")
    with open(ext_obj, encoding="utf-8") as f:
        ext = json.load(f)
    assert len(ext["relocs"]) == 1, ext["relocs"]
    assert ext["relocs"][0]["symbol"] == "puts"
    # Note: the CLI takes files; feed the source via a temp file.
    with tempfile.NamedTemporaryFile("w", suffix=".s",
                                     delete=False) as f:
        f.write(ext_src)
        ext_path = f.name
    try:
        proc = subprocess.run([asm_bin, "--bank", x86_bank, "-f", "bin",
                               "-o", os.path.join(tmp, "ext.bin"),
                               ext_path],
                              capture_output=True, text=True)
    finally:
        os.unlink(ext_path)
    assert proc.returncode != 0, "flat output must reject externals"

    # riscv64: add is 4 bytes; jalr with a constant encodes inline.
    # add x1,x2,x3 = 0x003100b3 -> b3 00 31 00 (little endian).
    rv_spec = os.path.join(args.source_dir, "compiler", "stdspec",
                           "riscv64.dsymspec")
    rv_bank = os.path.join(tmp, "riscv64.symas")
    build_bank(args.source_dir, compiler_bin, rv_spec, rv_bank)
    rv_out = os.path.join(tmp, "rv.bin")
    assemble(asm_bin, rv_bank, RISCV_SOURCE, rv_out)
    with open(rv_out, "rb") as f:
        rv = f.read()
    assert rv[:4] == bytes.fromhex("b3003100"), rv.hex()
    assert len(rv) == 16, rv.hex()

    check_riscv_asm_context(asm_bin, rv_bank, tmp)

    # jal against a local label needs linker-style immediate splicing, so
    # flat output keeps a relocation (obj) instead of patching it.
    rv_jal_obj = os.path.join(tmp, "rv_jal.obj.json")
    assemble(asm_bin, rv_bank, RISCV_JAL_SOURCE, rv_jal_obj, fmt="obj")
    with open(rv_jal_obj, encoding="utf-8") as f:
        jal_obj = json.load(f)
    assert len(jal_obj["relocs"]) == 1, jal_obj["relocs"]
    assert jal_obj["relocs"][0]["type"] == "R_RISCV_JAL"
    assert jal_obj["relocs"][0]["symbol"] == "target"

    for arch, call, machine, reloc, addend in [
        ("amd64", "call external", 62, 4, -4),
        ("aarch64", "bl external", 183, 283, 0),
        ("rv64im", "jal x1, external", 243, 17, 0),
    ]:
        bank = os.path.join(tmp, arch+".symas")
        build_bank(args.source_dir, compiler_bin,
                   os.path.join(args.source_dir, "compiler", "stdspec", arch+".dsymspec"), bank)
        check_elf(asm_bin, bank, os.path.join(tmp, arch+".o"), call, machine, reloc, addend)
        if arch == "aarch64":
            from aarch64_cephyr import check_aarch64_cephyr
            check_aarch64_cephyr(asm_bin, bank, tmp)
        if arch == "amd64":
            check_amd64_att(asm_bin, bank, tmp)

    print("PIPELINE_OK")


if __name__ == "__main__":
    main()
