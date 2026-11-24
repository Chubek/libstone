#!/usr/bin/env python3
"""Compare generated C11 recipes with LLVM MC for RV64GC, RV32GC and RV64IM."""

import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import subprocess
import tempfile


HERE = Path(__file__).resolve().parent
PROFILES = ("riscv64", "riscv32", "rv64im")
GC = set("i m a f d c zicsr zifencei".split())
EXTRAS = set("v zba zbb zbs zbkb zknd zkne zknh zfh zfa zicond zihintpause priv".split())
NAMED_R = set("add sub sll slt sltu xor srl sra or and mul div rem andn sh1add czeroeqz".split())
INT_LOAD = set("lb lh lw ld lbu lhu lwu".split())
INT_STORE = set("sb sh sw sd".split())
MOVES_FROM_VECTOR = {"vmv.x.s", "vfmv.f.s"}
FENCE_SETS = ("0", "w", "r", "rw", "o", "ow", "or", "orw",
              "i", "iw", "ir", "irw", "io", "iow", "ior", "iorw")
ROUNDING = ("rne", "rtz", "rdn", "rup", "rmm", None, None, "dyn")
CHECKLIST = {
    "i": """lui auipc jal jalr beq bne blt bge bltu bgeu lb lh lw lbu lhu sb sh sw
            addi slti sltiu xori ori andi slli srli srai add sub sll slt sltu xor srl sra or and
            fence fence.tso ecall ebreak""",
    "i64": "lwu ld sd addiw slliw srliw sraiw addw subw sllw srlw sraw",
    "m": "mul mulh mulhsu mulhu div divu rem remu",
    "m64": "mulw divw divuw remw remuw",
    "a": "lr sc amoswap amoadd amoxor amoand amoor amomin amomax amominu amomaxu",
    "f": """flw fsw fmadd.s fmsub.s fnmsub.s fnmadd.s fadd.s fsub.s fmul.s fdiv.s fsqrt.s
            fsgnj.s fsgnjn.s fsgnjx.s fmin.s fmax.s fcvt.w.s fcvt.wu.s fmv.x.w feq.s flt.s
            fle.s fclass.s fcvt.s.w fcvt.s.wu fmv.w.x""",
    "f64": "fcvt.l.s fcvt.lu.s fcvt.s.l fcvt.s.lu",
    "d": """fld fsd fmadd.d fmsub.d fnmsub.d fnmadd.d fadd.d fsub.d fmul.d fdiv.d fsqrt.d
            fsgnj.d fsgnjn.d fsgnjx.d fmin.d fmax.d fcvt.s.d fcvt.d.s feq.d flt.d fle.d fclass.d
            fcvt.w.d fcvt.wu.d fcvt.d.w fcvt.d.wu""",
    "d64": "fcvt.l.d fcvt.lu.d fmv.x.d fcvt.d.l fcvt.d.lu fmv.d.x",
    "c": """c.addi4spn c.lw c.sw c.nop c.addi c.li c.addi16sp c.lui c.andi c.sub c.xor c.or
            c.and c.j c.beqz c.bnez c.lwsp c.jr c.mv c.ebreak c.jalr c.add c.swsp c.srli c.srai c.slli""",
    "c64": "c.ld c.sd c.addiw c.subw c.addw c.ldsp c.sdsp",
    "c32": "c.jal",
    "cd": "c.fld c.fsd c.fldsp c.fsdsp",
    "cf32": "c.flw c.fsw c.flwsp c.fswsp",
    "zicsr": "csrrw csrrs csrrc csrrwi csrrsi csrrci",
    "zifencei": "fence.i",
}


def run(*args, source=None):
    return subprocess.run(args, input=source, text=True, capture_output=True, check=True).stdout


@dataclass
class Form:
    mnemonic: str
    encoder: str
    required: set
    operands: list


def catalog(exe, profile):
    features, forms = None, []
    for line in run(exe, profile, "list").splitlines():
        if line.startswith("@"):
            subarch, features = line[1:].split("\t")
            assert subarch == ({"riscv64": "rv64gc", "riscv32": "rv32gc"}.get(profile, profile))
            features = set(features.split(","))
        else:
            m, enc, requires, ops = line.split("\t")
            forms.append(Form(m, enc, set(requires.split(",")), ops.split(",") if ops else []))
    return features, forms


def check_inventory(forms, profile, opcodes):
    req = {"i64": {"i"}, "m64": {"m"}, "f64": {"f"}, "d": {"f", "d"},
           "d64": {"f", "d"}, "c32": {"c"}, "c64": {"c"}, "cd": {"c", "f", "d"},
           "cf32": {"c", "f"}}
    for k, names in CHECKLIST.items():
        if k.endswith("64") and profile == "riscv32" or k.endswith("32") and profile != "riscv32":
            continue
        required = {"i"} | req.get(k, {k})
        found = {f.mnemonic for f in forms if f.required == required}
        expected = set(names.split())
        if k == "a":
            expected = {f"{m}.{width}{order}" for m in expected
                        for width in ("w" if profile == "riscv32" else "wd")
                        for order in ("", ".aq", ".rl", ".aqrl")}
        assert expected <= found, (profile, k, expected - found)
    if opcodes:
        def canonical(file):
            lines = (opcodes / "extensions" / file).read_text().splitlines()
            return {line.split()[0] for line in lines if line and line[0] not in "#$"}

        width = "rv32" if profile == "riscv32" else "rv64"
        subsets = ("zba", "zbb", "zbs", "zbkb", "zknd", "zkne", "zknh", "zfh", "zicond")
        groups = {k: ["rv_" + k, width + "_" + k] for k in subsets}
        groups["zfh"] += ["rv_zfhmin", "rv_d_zfhmin"]
        groups["zfa"] = ["rv_f_zfa", "rv_d_zfa", "rv_zfh_zfa", width + "_d_zfa"]
        groups["v"] = ["rv_v"]
        groups["priv"] = ["rv_system", "rv_s"]
        for feature, files in groups.items():
            expected = set()
            for file in files:
                if (opcodes / "extensions" / file).exists():
                    expected.update(canonical(file))
            if feature == "zbkb" and "rev8.rv32" in expected:
                expected.remove("rev8.rv32")
                expected.add("rev8")
            if feature == "zkne" and width == "rv64":
                expected.update({"aes64ks1i", "aes64ks2"})
            if feature == "zbkb":
                expected.update({"andn", "orn", "xnor", "rol", "ror", "rori", "rev8"})
                if width == "rv64":
                    expected.update({"rolw", "rorw", "roriw"})
            if feature == "v" and width == "rv32":
                expected -= {"vluxei64.v", "vloxei64.v", "vsuxei64.v", "vsoxei64.v"}
            found = {f.mnemonic for f in forms if feature in f.required}
            assert expected <= found, (profile, feature, expected - found)


def encode_line(form, values):
    if form.encoder in NAMED_R:
        names = ("rd", "rs1", "rs2")
    elif form.encoder == "jal":
        names = ("rd", "sym")
    elif form.encoder in INT_LOAD:
        names = ("rd", "rs1", "imm")
    elif form.encoder in INT_STORE:
        names = ("r0", "rs1", "imm")
    else:
        names = tuple("sym" if v.startswith("@") else f"r{i}" for i, v in
                      enumerate(map(str, values)))
    return " ".join([form.encoder, *(f"{a}={b}" for a, b in zip(names, values))])


def sample(form):
    """Use distinct operands to catch reversed fields; exercise nonzero/scattered bits."""
    m, enc, ops = form.mnemonic, form.encoder, form.operands
    if any(op == "sym" for op in ops):
        return None  # Separate ELF relocation test.
    if m == "li" and enc == "li_wide":
        return "li x7, 0x5a3f8021", [7, 0x5a3f8021]
    vec = "v" in form.required
    comp = m.startswith("c.")
    atomic = re.match(r"(amo|lr\.|sc\.)", m)
    # Four distinct vector registers, aligned for whole-register groups.
    rv = [8, 16, 24, 0, 0]
    rx = [9, 10, 13, 15, 17] if comp else [5, 7, 11, 13, 17]
    args, vals = [], []
    for i, op in enumerate(ops):
        kind, _, bounds = op.partition(":")
        if kind == "gpr":
            val = 2 if (m.endswith("sp") and i == 1 or m == "c.addi16sp" and i == 0
                        or m == "c.addi4spn" and i == 1) else rx[i]
            arg = f"x{val}"
        elif kind == "fp":
            val = rx[i]
            if m == "c.fldsp":
                val = 0  # Unlike integer stack loads, f0 is legal.
            arg = f"f{val}"
        elif kind == "vec":
            val = rv[i]
            if enc.endswith("_mask") and i == len(ops) - 1:
                val = 0
                arg = "v0" if re.match(r"v(f?merge|adc|sbc|madc|msbc)\.", m) else "v0.t"
            else:
                if (m in MOVES_FROM_VECTOR or m.startswith(("vfcvt", "vfwcvt", "vfncvt"))) and i == 1:
                    val = 16
                arg = f"v{val}"
        elif kind == "ctrl":
            val, arg = 0x305, "mtvec"
        else:
            assert kind == "imm" and bounds, (m, enc, op)
            lo, hi = map(int, bounds.split(":"))
            val = min(hi, max(lo, -13 if lo < 0 else 13))
            if enc.endswith("_offset") and (m.startswith(("b", "c.b", "c.j")) or m in ("j", "jal")):
                val = max(lo, -186)
            if m in {"csrrw", "csrrs", "csrrc", "csrrwi", "csrrsi", "csrrci", "csrr",
                     "csrw", "csrs", "csrc", "csrwi", "csrsi", "csrci"} and hi == 4095:
                val = 0x873
            if m in {"c.addi4spn", "c.addi16sp"}:
                val = 956 if m == "c.addi4spn" else -432
            if m == "c.lui":
                val = 1048563
            if comp and re.match(r"c\.(f?[ls][wd])", m):
                val = (hi + 1) // 2 + (28 if m.startswith(("c.lw", "c.sw", "c.flw", "c.fsw")) else 24)
            if m == "fence":
                val = 10 if i == 0 else 5
            if enc.endswith("_rm"):
                val = 3
            if m in ("vsetvli", "vsetivli") and i == 2:
                val = 0xD7  # e32,mf2,ta,ma
            if m.startswith("fli."):
                val = 20  # 2.0 (see Zfa's table)
            arg = str(val)
            if m == "fence":
                arg = FENCE_SETS[val]
            if enc.endswith("_rm") or m == "fcvtmod.w.d":
                arg = ROUNDING[val]
            if m in ("vsetvli", "vsetivli") and i == 2:
                arg = "e32,mf2,ta,ma"
            if m.startswith("fli."):
                arg = "2.0"
        args.append(arg)
        vals.append(val)
    if atomic:
        args[-1] = f"({args[-1]})"
    elif vec and re.match(r"v[ls](?:(?:e|se|uxei|oxei)\d|(?:s|ux|ox)?seg|m\.v|[1-8]r)", m):
        args[1] = f"({args[1]})"
    elif m in INT_LOAD | INT_STORE or re.match(r"(?:c\.)?f?[ls][wdh](sp)?$", m):
        args = args[:1] + [f"{args[-1]}({args[-2]})"]
    elif m == "jalr" and len(args) == 3:
        args = args[:1] + [f"{args[-1]}({args[-2]})"]
    elif m == "jr" and len(args) == 2:
        args = [f"{args[-1]}({args[0]})"]
    return f"{m} " + ", ".join(args), vals


def check_bytes(exe, llvm, profile, forms, enabled):
    samples = []
    for form in forms:
        if not form.required <= enabled:
            continue
        pair = sample(form)
        if pair:
            asm, values = pair
            samples.append((form, asm, values))
    bindings = "".join(encode_line(f, v) + "\n" for f, _, v in samples)
    have = run(exe, profile, "encode", source=bindings).splitlines()
    attributes = ",".join(f"+{k}" for k in sorted(enabled - {"i", "priv"}))
    target = "riscv32" if profile == "riscv32" else "riscv64"
    assembly = []
    for form, asm, _ in samples:
        assembly.append(".option rvc" if form.mnemonic.startswith("c.") else ".option norvc")
        assembly.append(asm)
    try:
        reference = run(llvm, f"-triple={target}", f"-mattr={attributes}", "-show-encoding",
                        source="\n".join(assembly) + "\n")
    except subprocess.CalledProcessError as e:
        raise AssertionError(e.stderr) from e
    words = re.findall(r"encoding:\s*\[([^\]]+)\]", reference)
    assert len(have) == len(samples), (len(have), len(samples))
    # li on RV32 can expand into two instructions; collapse those words.
    pos = 0
    for (form, asm, _), actual in zip(samples, have):
        count = 2 if form.encoder == "li_wide" else 1
        expected = "".join(x.strip()[2:] for w in words[pos:pos + count] for x in w.split(","))
        assert expected == actual, (profile, asm, form.encoder, expected, actual)
        pos += count
    assert pos == len(words), (pos, len(words))
    return len(samples)


def check_invalid(exe, profile, forms):
    by_encoder = {f.encoder: f for f in forms}
    invalid = {
        "add": [32, 7, 11], "lb": [0, -1, 0], "sw": [5, 7, 2048],
        "addi": [5, 7, -2049], "slli": [5, 7, 32 if profile == "riscv32" else 64],
        "beq_offset": [5, 7, 4096], "bltu_offset": [5, 7, -4095],
        "jal_offset": [5, 1048576], "j_offset": [-1048578],
        "fence_pred_succ": [16, 0], "csrrwi_numeric": [5, 0xfff, 32],
        "csrrw_numeric": [5, 4096, 7], "fadd.s_rm": [1, 2, 3, 5],
        "fmadd.d_rm": [1, 2, 3, 4, 6], "fcvtmod.w.d": [5, 2, 0],
        "caddi": [5, 0], "c.li": [0, -32], "c.lui": [2, 1],
        "c.addi16sp": [2, 31], "c.addi4spn": [8, 2, 0],
        "c.slli": [8, 0], "c.lw": [7, 9, 4], "c.sw": [8, 9, 2],
        "c.lwsp": [0, 2, 4], "c.fldsp": [0, 5, 8],
        "c.j_offset": [2048], "c.beqz_offset": [8, -257],
        "aes64ks1i": [5, 7, 11], "vsetvli": [5, 7, 4],
        "vsetivli": [5, 32, 0], "vadd.vv_mask": [8, 16, 24, 1],
        "vadc.vvm_mask": [0, 16, 24, 0], "vl8re64.v": [4, 7],
        "vmv8r.v": [8, 12], "vslideup.vx": [8, 8, 7],
    }
    cases = [(by_encoder[k], v) for k, v in invalid.items() if k in by_encoder]
    cases.append((by_encoder["c.slli"], [8, 32 if profile == "riscv32" else 64]))
    output = run(exe, profile, "encode",
                 source="".join(encode_line(f, v) + "\n" for f, v in cases)).splitlines()
    for (f, v), result in zip(cases, output, strict=True):
        assert result == "rejected", (profile, f.encoder, v, result)


def check_boundaries(exe, llvm, profile, forms, enabled):
    by_encoder = {f.encoder: f for f in forms}
    cases = []
    for enc, asm, values, maximum in [
        ("beq_offset", "beq x31,x1,", [31, 1], 4096),
        ("bltu_offset", "bltu x1,x31,", [1, 31], 4096),
        ("jal_offset", "jal x31,", [31], 1048576),
        ("c.beqz_offset", "c.beqz x15,", [15], 256),
        ("c.j_offset", "c.j ", [], 2048),
    ]:
        if by_encoder[enc].required <= enabled:
            for v in (-maximum, -2, 0, 2, maximum - 2):
                cases.append((enc, asm + str(v), [*values, v]))
    for v, flag in enumerate(FENCE_SETS):
        cases.append(("fence_pred_succ", f"fence {flag}, {FENCE_SETS[15 - v]}", [v, 15 - v]))
    if profile != "rv64im":
        for i, rm in enumerate(ROUNDING):
            if rm:
                cases.append(("fmadd.d_rm", f"fmadd.d f31, f30, f16, f1, {rm}", [31, 30, 16, 1, i]))
        cases.append(("csrrsi_numeric", "csrrsi x31, 4095, 31", [31, 4095, 31]))
    references, recipes = [], []
    for enc, asm, vals in cases:
        references.extend([".option rvc" if asm.startswith("c.") else ".option norvc", asm])
        recipes.append(encode_line(by_encoder[enc], vals))
    target = "riscv32" if profile == "riscv32" else "riscv64"
    attrs = ",".join(f"+{k}" for k in sorted(enabled - {"i"}))
    obj = run(llvm, f"-triple={target}", f"-mattr={attrs}", "-show-encoding",
              source="\n".join(references) + "\n")
    expected = [w.replace("0x", "").replace(" ", "").replace(",", "")
                for w in re.findall(r"encoding:\s*\[(.*?)\]", obj)]
    actual = run(exe, profile, "encode", source="\n".join(recipes) + "\n").splitlines()
    for (_, asm, _), have, want in zip(cases, actual, expected, strict=True):
        assert have == want, (profile, asm, have, want)


def check_relocations(exe, llvm, readobj, directory, profile, forms):
    target = "riscv32" if profile == "riscv32" else "riscv64"
    cases = [("beq", "beq x5,x7", [5, 7], "R_RISCV_BRANCH", "63807200"),
             ("bltu", "bltu x5,x7", [5, 7], "R_RISCV_BRANCH", "63e07200"),
             ("jal", "jal x5", [5], "R_RISCV_JAL", "ef020000")]
    if profile != "rv64im":
        cases.extend([("c.beqz", "c.beqz x9", [9], "R_RISCV_RVC_BRANCH", "81c0"),
                      ("c.j", "c.j", [], "R_RISCV_RVC_JUMP", "01a0")])
    # LLVM expands out-of-section/unresolved branches into long jumps.
    # An in-range local symbol plus .option relax preserves the instruction
    # and asks LLVM to leave the actual ELF fixup in the object.
    src = ".option relax\n.local branch_target\n"
    lines, expected_output, expected_relocs, offset = [], [], [], 0
    by_encoder = {f.encoder: f for f in forms}
    for enc, asm, values, reloc, encoded in cases:
        src += asm + (" " if asm == "c.j" else ",") + "branch_target\n"
        lines.append(encode_line(by_encoder[enc], [*values, "@branch_target"]))
        expected_output.append(f"{encoded}|{reloc}:branch_target:0:0")
        expected_relocs.append((offset, reloc))
        offset += len(encoded) // 2
    assert run(exe, profile, "encode", source="\n".join(lines) + "\n").splitlines() == expected_output
    obj = directory / (profile + ".o")
    attrs = "-mattr=+c" if profile != "rv64im" else "-mattr=-c"
    run(llvm, f"-triple={target}", attrs, "-filetype=obj", "-o", obj, source=src + "branch_target:\n")
    relocs = run(readobj, "--relocations", obj)
    for offset, reloc in expected_relocs:
        assert re.search(rf"0x{offset:X}\s+{reloc}\s+branch_target\s+0x0", relocs), relocs
    dumped = run(readobj, "--hex-dump=.text", obj)
    hex_rows = re.findall(r"0x[0-9a-f]+\s+((?:[0-9a-f]{8}\s+)*[0-9a-f]{4,8})", dumped)
    assert "".join("".join(row.split()) for row in hex_rows) == "".join(row[4] for row in cases), dumped


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("generated", type=Path, help="directory containing the generated .i files")
    parser.add_argument("--work", type=Path, default=Path("build"), help="scratch directory")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--llvm-mc", default="llvm-mc")
    parser.add_argument("--llvm-readobj", default="llvm-readobj")
    parser.add_argument("--opcodes", type=Path, help="local riscv-opcodes checkout for extension inventory")
    ns = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="riscv-enc-", dir=ns.work) as directory:
        directory = Path(directory)
        exe = directory / "encode"
        subprocess.run([ns.cc, "-std=c11", "-Wall", "-Wextra", "-pedantic-errors", "-I", ns.generated,
                        "-I", HERE.parents[1], HERE / "encode.c", "-o", exe],
                       env={**os.environ, "TMPDIR": str(directory.resolve())}, check=True)
        for profile in PROFILES:
            enabled, forms = catalog(exe, profile)
            assert enabled == ({"i", "m"} if profile == "rv64im" else GC)
            assert all("i" in f.required for f in forms)
            check_inventory(forms, profile, ns.opcodes)
            active = {f.mnemonic for f in forms if f.required <= enabled}
            assert {"add", "mul", "fence"} <= active
            assert not active.intersection({"sh1add", "vsetvli", "mret"})
            if profile == "rv64im":
                assert not active.intersection({"csrrs", "fence.i", "lr.w", "fmadd.s", "c.addi"})
            if profile == "riscv32":
                assert {"c.jal", "c.flw", "c.fswsp"} <= active
                assert not active.intersection({"addw", "lwu", "c.addiw", "c.ld", "lr.d", "fcvt.l.s", "fmv.x.d"})
            else:
                assert "addw" in active and "c.jal" not in active and "c.flw" not in active
            count = check_bytes(exe, ns.llvm_mc, profile, forms, enabled)
            if profile != "rv64im":
                count = check_bytes(exe, ns.llvm_mc, profile, forms, enabled | EXTRAS)
            check_invalid(exe, profile, forms)
            check_boundaries(exe, ns.llvm_mc, profile, forms, enabled)
            check_relocations(exe, ns.llvm_mc, ns.llvm_readobj, directory, profile, forms)
            print(f"{profile}: checked {count} byte samples, invalid operands and ELF relocations")


if __name__ == "__main__":
    main()
