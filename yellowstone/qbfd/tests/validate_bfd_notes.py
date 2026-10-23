#!/usr/bin/env python3
"""Validate selected source-derived BFD.yaml tables against the local snapshot.

Needs PyYAML and binutils/. Exit 77 if those optional inputs are unavailable.
Behavioral prose still requires source review; this is not a completeness test.
"""
import ast
import operator
from pathlib import Path
import re
import sys

try:
    import yaml
except ImportError:
    print("SKIP: install PyYAML to validate BFD.yaml")
    sys.exit(77)

QBFD = Path(__file__).resolve().parent.parent
BINUTILS = QBFD.parent.parent / "binutils"
if not (BINUTILS / "bfd/bfd-in2.h").is_file():
    print("SKIP: local binutils snapshot is unavailable")
    sys.exit(77)


class UniqueLoader(yaml.SafeLoader):
    """Reject duplicate mapping keys rather than silently keeping the last."""


def mapping(loader, node, deep=False):
    result = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result:
            raise ValueError(f"duplicate key {key!r} at line {key_node.start_mark.line + 1}")
        result[key] = loader.construct_object(value_node, deep=deep)
    return result


UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)


def check(condition, message):
    if not condition:
        raise ValueError(message)


def validate():
    text = (QBFD / "BFD.yaml").read_text()
    doc = yaml.load(text, Loader=UniqueLoader)
    external = (BINUTILS / "include/elf/external.h").read_text()
    layouts = {}
    for body, name in re.findall(
            r"typedef\s+struct\s*\{(.*?)\}\s*(Elf\w*_External_\w+);", external, re.S):
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
        offset, fields = 0, {}
        for field, width in re.findall(r"unsigned\s+char\s+(\w+)\[(\d+)\]", body):
            if name.endswith("_Dyn") and field == "d_ptr":
                fields[field] = fields["d_val"]  # the two union alternatives overlap
                continue
            fields[field] = offset
            offset += int(width)
        layouts[name] = offset, fields
    layout_checks = 0
    for group in doc["formats"]["elf"]["headers"].values():
        if not isinstance(group, dict):
            continue
        for record in group["structs"]:
            name = record["struct"]
            size, fields = layouts[name]
            check(record["size"] == size, f"{name}: size mismatch")
            layout_checks += 1
            bits = "32" if name.startswith("Elf32") else "64"
            for field in group["fields"]:
                key = field["field"]
                if key == "d_un.d_val / d_un.d_ptr":
                    key = "d_val"
                check(field["off" + bits] == fields[key], f"{name}.{key}: offset mismatch")
                layout_checks += 1

    header = (BINUTILS / "bfd/bfd-in2.h").read_text()
    for group in doc["architecture"]["target_vector"]["jump_table_groups"]:
        name = group["name"]
        body = re.search(r"#define BFD_JUMP_TABLE_" + name + r"\(NAME\)(.*?)(?=\n\n)",
                         header, re.S).group(1)
        slots = [a or b for a, b in re.findall(
            r"NAME##_(\w+)|(?<!#)\b(_bfd_generic_\w+)", body)]
        check(group["slots"] == slots, f"{name}: callback mismatch")

    reloc = (BINUTILS / "bfd/reloc.c").read_text()
    body = re.search(r"bfd_get_reloc_size \(reloc_howto_type \*howto\)\s*\{(.*?)\n\}",
                     reloc, re.S).group(1)
    widths = {int(k): int(v) for k, v in re.findall(r"case (-?\d+): return (\d+);", body)}
    check(doc["architecture"]["relocation_engine"]["descriptor_width"]["size_code_to_bytes"]
          == widths, "relocation width-code mismatch")

    definitions = dict(re.findall(r"^#define\s+((?:SEC|BSF)_\w+)\s+([^\n]+)",
                                  header.replace("\\\n", ""), re.M))
    operations = {ast.LShift: operator.lshift, ast.BitOr: operator.or_,
                  ast.Add: operator.add, ast.Sub: operator.sub}

    def evaluate(node):
        if isinstance(node, ast.Constant) and isinstance(node.value, int):
            return node.value
        if isinstance(node, ast.Name):
            expression = re.sub(r"/\*.*?\*/", "", definitions[node.id]).strip()
            return evaluate(ast.parse(expression, mode="eval").body)
        if isinstance(node, ast.BinOp) and type(node.op) in operations:
            return operations[type(node.op)](evaluate(node.left), evaluate(node.right))
        raise ValueError(f"unhandled constant expression: {ast.dump(node)}")

    flag_checks = 0
    for table in [doc["canonical_model"]["section_flags"]["bfd_values"],
                  doc["canonical_model"]["symbol_flags"]["notable"]]:
        for name, expected in table.items():
            check(evaluate(ast.Name(id=name)) == expected, f"{name}: flag mismatch")
            flag_checks += 1

    exceptions = {"bfd/bfd.h", "bfd/elfxx-aarch64.c", "bfd/elfxx-riscv.c",
                  "include/elf/aarch64.h", "include/elf/riscv.h"}
    refs = re.findall(r"\b((?:bfd|include)/[\w./-]+\.(?:h|c))"
                      r"(?::([0-9]+)(?:-([0-9]+))?)?", text)
    for path, start, end in refs:
        file = BINUTILS / path
        if not file.is_file():
            check(path in exceptions, f"missing citation file: {path}")
            continue
        if start:
            count = len(file.read_text(errors="replace").splitlines())
            check(1 <= int(start) <= count, f"{path}: invalid starting line {start}")
            if end:
                check(int(start) <= int(end) <= count, f"{path}: invalid range {start}-{end}")
    print(f"BFD notes valid: unique YAML keys, {layout_checks} ELF layout checks, "
          f"{flag_checks} flags, nine callback groups, width codes, {len(refs)} citations")


if __name__ == "__main__":
    validate()
