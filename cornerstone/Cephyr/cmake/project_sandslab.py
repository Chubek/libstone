"""Embed the Sandstone catalog subset needed by the native constant selector."""
from pathlib import Path
import json
import sys

catalog, header = map(Path, sys.argv[1:])
sys.path.insert(0, str(catalog.parent))
import simple_isa

isa = simple_isa.load(catalog)
text = [f"arch {isa.name} {{ word_size = {isa.word_size}; addr_size = {isa.addr_size}; "
        f"endian = {isa.endian}; align = {isa.align}; }}"]
for name in ("mov_eax_imm32", "mov_rax_sext32"):
    op = isa.operation(name)
    enc = op.encoding
    if enc.width is not None or op.operands != (("imm", "imm"),):
        raise ValueError(f"expected variable-width accumulator-immediate form: {name}")
    directives = " ".join(f"{k} = {v};" for k, v in enc.directives.items())
    text.append(f"encoding {enc.name} {{ width = variable; {directives} }}")
    text.append(f"op {op.name} {{ class = {op.category}; encoding = {enc.name}; "
                f"operands = imm: imm; syntax = {json.dumps(op.syntax)}; }}")
header.write_text("namespace cephyr { constexpr char sandslab_x86_isa[] = "
                  + json.dumps("\n".join(text)) + "; }\n")
