"""Bundle validated-at-runtime ISA sources with ArchVirt machine extensions.

The extra A64 forms cover scalar addressing/operand variants missing from the
upstream compiler profile. Fixed bits are checked by ArchML, and variable bits
are decoded into a single bitvector for the execution adapter. The adapter must
still reject reserved encodings and unsupported features within each class.
"""
import pathlib
import sys

root, output = map(pathlib.Path, sys.argv[1:])
spec = root / "third_party/libstone/sandstone/archml/stdspec"
classes = {
    "pc-relative": (0x1F000000, 0x10000000),
    "add-sub-immediate": (0x1F000000, 0x11000000),
    "logical-immediate": (0x1F800000, 0x12000000),
    "move-wide": (0x1F800000, 0x12800000),
    "bitfield": (0x1F800000, 0x13000000),
    "extract": (0x1F800000, 0x13800000),
    "branch-immediate": (0x7C000000, 0x14000000),
    "branch-condition": (0xFF000010, 0x54000000),
    "compare-branch": (0x7E000000, 0x34000000),
    "test-branch": (0x7E000000, 0x36000000),
    "br": (0xFFFFFC1F, 0xD61F0000),
    "blr": (0xFFFFFC1F, 0xD63F0000),
    "ret": (0xFFFFFC1F, 0xD65F0000),
    "load-store-unsigned": (0x3F000000, 0x39000000),
    "load-store-unscaled": (0x3F200000, 0x38000000),
    "load-store-register": (0x3F200C00, 0x38200800),
    "load-store-pair": (0x3E000000, 0x28000000),
    "load-literal": (0x3F000000, 0x18000000),
    "logical-register": (0x1F000000, 0x0A000000),
    "add-sub-register": (0x1F200000, 0x0B000000),
    "add-sub-extended": (0x1F200000, 0x0B200000),
    "two-source": (0x5FE00000, 0x1AC00000),
    "three-source": (0x1F000000, 0x1B000000),
    "conditional-select": (0x1FE00000, 0x1A800000),
    "conditional-compare": (0x1FE00000, 0x1A400000),
    "halt": (0xFFFFFFFF, 0xD4400000),
}

def extensions(forms=classes):
    result = []
    for name, (mask, base) in forms.items():
        variable = [i for i in range(32) if not mask >> i & 1]
        fields = " ".join(f"(field bits {i} 1 {j})" for j, i in enumerate(variable))
        operands = f"(bits (kind immediate) (bits {len(variable)}) (signed false) (scale 1) (access read))" if fields else ""
        result.append(f'''(instruction archvirt-{name}
          (operands {operands})
          (encoding (base "{base.to_bytes(4, 'little').hex()}") {fields})
          (pcode (trap archvirt-execution-adapter))
          (assembly "archvirt-{name}"))''')
    return "\n".join(result)

output.parent.mkdir(parents=True, exist_ok=True)
(output.parent / "machines").mkdir(exist_ok=True)
with output.open("w") as out:
    out.write("// Generated from ArchML ISA and machine sources.\n#pragma once\n")
    out.write("namespace domvirt::machine::sources {\n")
    for name, isa in (("x86_64", "amd64"), ("aarch64", "arm64"), ("riscv64", "riscv64")):
        text = (spec / f"{isa}.aml").read_text().rstrip()
        assert text.endswith(")")
        text = text[:-1] + (root / "machines" / f"{name}.aml").read_text()
        if name == "aarch64":
            text += extensions()
        if name == "riscv64":
            # The execution adapter checks funct fields precisely. Upstream
            # metadata for other extensions does not admit them for execution.
            text += extensions({f"rv-op-{op:02x}": (0x7f, op) for op in
                                [0x37, 0x17, 0x6f, 0x67, 0x63, 0x03, 0x23,
                                 0x13, 0x1b, 0x33, 0x3b, 0x0f, 0x73]})
        text += ")\n"
        (output.parent / "machines" / f"{name}.aml").write_text(text)
        out.write(f'inline constexpr char {name}[] = R"ARCHVIRT({text})ARCHVIRT";\n')
    out.write("}\n")
