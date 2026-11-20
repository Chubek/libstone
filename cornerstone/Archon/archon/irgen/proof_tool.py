"""Generate bounded proof obligations from the emitted Rewrite and ISel banks."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from ..diagnostics import ArchonError
from . import isel, rewrite
from .tool_templates import proof_cli


STAGE_ORDER = 700
_TEMPLATES = Path(__file__).with_name("tool_templates")
_SEMANTIC_OPS = {
    "+%": "add", "-%": "sub", "*%": "mul", "&": "and", "|": "or", "^": "xor",
    "~": "not", "<<": "shl",
}
_MAX_WIDTH = 64


class _Unmodeled(Exception):
    """This rule is registered but has no obligation in our value fragment."""


def _primitive(expr):
    if expr["op"] == ">>":
        return "ashr" if expr["type"]["kind"] == "sint" else "lshr"
    return _SEMANTIC_OPS[expr["op"]]


def _sem_expr(expr, locals_, operands):
    op = expr["op"]
    width = expr["type"]["width"]
    if not 1 <= width <= _MAX_WIDTH:
        raise _Unmodeled("ArchSem expression exceeds the 64-bit proof bound")
    if op == "constant":
        return proof_cli.bv_literal(expr["value"], width)
    if op == "local":
        return locals_[expr["name"]]
    if op in ("read", "immediate"):
        return operands[expr["name"]]
    if op in {"zext", "sext", "trunc", "extract", "~"}:
        child = expr["value"]
        value = _sem_expr(child, locals_, operands)
        size = child["type"]["width"]
        if op == "~":
            return ("bvnot", value)
        if op == "extract":
            return (("_", "extract", expr["high"], expr["low"]), value)
        if width == size:
            return value
        if op == "trunc":
            return (("_", "extract", width - 1, 0), value)
        return proof_cli.extend_word(value, size, width, signed=op == "sext")
    if op in _SEMANTIC_OPS or op in {">>", "concat"}:
        left = _sem_expr(expr["left"], locals_, operands)
        right = _sem_expr(expr["right"], locals_, operands)
        if op == "concat":
            return ("concat", left, right)
        if op in {"<<", ">>"}:
            count_width = expr["right"]["type"]["width"]
            if count_width > width:
                raise _Unmodeled("shift count wider than its result word")
            right = proof_cli.extend_word(right, count_width, width, signed=False)
        return proof_cli.primitive_expression(_primitive(expr), (left, right))
    raise _Unmodeled(f"unmodeled ArchSem expression {op!r}")


def _target_value(instruction):
    """Compile the fully inspected ArchSem result, not the selector's mnemonic."""
    sem = instruction.get("semantics")
    if sem is None:
        raise _Unmodeled("target instruction has no ArchSem")
    if any(value for key, value in sem["effects"].items() if key not in {"reads", "writes"}):
        raise _Unmodeled("non-value effects are not modeled")
    width = sem["word_width"]
    if not 1 <= width <= _MAX_WIDTH:
        raise _Unmodeled("register word exceeds the 64-bit proof bound")
    result = []
    arguments = []
    arguments_by_name = {}
    fields = instruction["fields"]
    for operand in instruction["operands"]:
        name, direction = operand["name"], operand.get("direction")
        if "register_range" in operand:
            raise _Unmodeled("restricted register classes are not modeled")
        if direction == "out" and operand["kind"] == "reg":
            result.append(name)
            continue
        if direction != "in":
            raise _Unmodeled("tied or nonstandard operand roles are not modeled")
        arguments_by_name[name] = f"@{len(arguments)}"
        if operand["kind"] == "reg":
            arguments.append(None)
        else:
            size = operand["width"]
            if size > width:
                raise _Unmodeled("immediate wider than the result word")
            if [row["width"] for row in fields if row["operand"] == name] != [size]:
                raise _Unmodeled("immediate does not have a complete encoding field")
            arguments.append([size, operand["signed"]])
    if len(result) != 1 or sem["effects"]["writes"] != result:
        raise _Unmodeled("expected exactly one unconstrained register output")
    statements = sem["statements"]
    if (not statements or statements[-1]["op"] != "write"
            or statements[-1]["name"] != result[0]):
        raise _Unmodeled("expected one final register write")
    locals_ = {}
    for statement in statements[:-1]:
        if statement["op"] != "let":
            raise _Unmodeled("only the final register write and local values are modeled")
        locals_[statement["name"]] = _sem_expr(statement["value"], locals_, arguments_by_name)
    expression = _sem_expr(statements[-1]["value"], locals_, arguments_by_name)
    # Local bindings share trees during translation. Bound their expansion
    # before serializing it or substituting argument trees.
    proof_cli.check_expression_size(expression)
    return {
        "name": instruction["name"], "width": width, "arguments": arguments,
        "expression": expression,
    }


def _rewrite_row(architecture, artifact, rule, instruction_by_name):
    entry = {
        "id": rule["id"],
        "kind": rule["kind"],
        "target": architecture,
        "evidence": f"{artifact} ({rule['name']})",
    }
    try:
        root = rule["root"]
        op = _primitive(root)
        width = root["type"]["width"]
        if not 1 <= width <= _MAX_WIDTH:
            raise _Unmodeled("result width exceeds the 64-bit proof bound")
        if any(child.get("width") != width for child in rule["child_types"]):
            raise _Unmodeled("nonuniform term widths are not modeled")
        if rule["arity"] != proof_cli.PROGRAM_ARITY[op]:
            raise _Unmodeled("unexpected semantic arity")

        entries = {"op": op, "width": width}
        if rule["kind"] == "rename":
            model = _target_value(instruction_by_name[rule["instruction"]])
            if len(model["arguments"]) != rule["arity"] or model["width"] != width:
                raise _Unmodeled("target and rewrite signatures differ")
            if any(arg is not None and arg[0] != width for arg in model["arguments"]):
                raise _Unmodeled("target rename requires word-sized operands")
            args = [f"v{i}" for i in range(rule["arity"])]
            declarations = [(symbol, width) for symbol in args]
            left = proof_cli.bind_archsem(model["expression"], args)
            right = proof_cli.primitive_expression(op, [args[i] for i in rule["map"]])
            entries.update(model=model, mapping=rule["map"])
        elif rule["kind"] == "commute":
            declarations = [("v0", width), ("v1", width)]
            left = proof_cli.primitive_expression(op, ("v0", "v1"))
            right = proof_cli.primitive_expression(op, ("v1", "v0"))
        else:
            assert rule["kind"] == "identity"
            declarations = [("v0", width)]
            constant = rule["value"]
            side = rule["side"]
            args = ["v0", "v0"]
            args[side] = proof_cli.bv_literal(constant, width)
            left = proof_cli.primitive_expression(op, args)
            right = "v0"
            entries.update(side=side, constant=constant)
        query = proof_cli.equation_query(declarations, left, right)
    except (ValueError, _Unmodeled) as exc:
        return {**entry, "unknown": str(exc)}, None
    return {**entry, **entries}, query


def _isel_row(db, row, instruction, artifact):
    source, name = row["source"], row["target"]
    entry = {
        "id": rewrite.stable_rule_id("isel", db["architecture"], source, name),
        "kind": "explicit" if row["explicit"] else "select",
        "target": db["architecture"],
        "evidence": f"{artifact} (pattern {row['id']}: {source} -> {name})",
    }
    if row["explicit"]:
        return {**entry, "unknown": "explicit target choice makes no source equivalence claim"}, None
    try:
        model = _target_value(instruction)
        if (row["width"] != model["width"] or source not in proof_cli.PROGRAM_ARITY
                or row["arity"] != len(model["arguments"])
                or row["arity"] != proof_cli.PROGRAM_ARITY[source]):
            raise _Unmodeled("selector pattern and value model signatures differ")
        declarations, target_args, source_args = [], [], []
        for i, argument in enumerate(model["arguments"]):
            symbol = f"v{i}"
            is_constant = bool(row["const_mask"] & (1 << i))
            signed = bool(row["signed_mask"] & (1 << i))
            arg_width = row["arg_widths"][i]
            if is_constant:
                if argument != [arg_width, signed]:
                    raise _Unmodeled("selector immediate field disagrees with the target value model")
            elif argument is not None or arg_width != row["width"]:
                raise _Unmodeled("selector requires nonuniform register widths")
            declarations.append((symbol, arg_width))
            target_args.append(symbol)
            # Quantify over all encodable bit patterns. Reconstruct the
            # word-width source literal exactly as archon_constant_fits does,
            # rather than treating an arbitrary word as a legal immediate.
            source_args.append(proof_cli.extend_word(symbol, arg_width, row["width"], signed))
        left = proof_cli.primitive_expression(source, source_args)
        right = proof_cli.bind_archsem(model["expression"], target_args)
        query = proof_cli.equation_query(declarations, left, right)
    except (ValueError, _Unmodeled) as exc:
        return {**entry, "unknown": str(exc)}, None
    return {**entry, "model": model, "width": row["width"], "op": source}, query


def _boundaries(context, databases):
    if not databases:
        return
    options = context.get("options", {})
    target = options.get("target") or min(databases)
    boundaries = [
        ("pipeline", "rewrite-bridge", "Pipeline.c:AP_write_rewrite_artifact",
         "engine statistics only; the extracted root is not committed to the program"),
        ("isel", "aliases-cse", "ISel.c:archon_canonical_operation / archon_nodes_equal",
         "MIR alias canonicalization and e-class CSE are not checked by value pattern proofs"),
        ("late", "register-homes", "Pipeline.c:AP_write_emission_operand",
         "allocated physical operands depend on register liveness and are not term rewrites"),
    ]
    if options.get("profile") == "native":
        boundaries.extend([
            ("native", "lowered-sequences", "NativeISel.c:an_select / IntegerConversion.c:an_convert",
             "multi-instruction native lowering, clobbers and traps are not modeled"),
            ("late", "conditional-transfer", "NativeEmitter.c:an_branch / NativeEncoding.c:an_encode_pcrel",
             "native control transfer, fixups and layout are not modeled"),
        ])
    if options.get("assembler") == "sandstone":
        boundaries.append((
            "late", "sandstone-asmopt", "AsmOPT.lua (Sandstone --asm-opt)",
            "Sandstone checks conditional alias byte equivalence against the loaded bank, not Proof.py",
        ))
    if options.get("linker") == "yld" and options.get("lto") is True:
        boundaries.append((
            "late", "yld-section-gc", "LTO.lua (qobjld --lto-script)",
            "YLD section garbage collection depends on link-time reachability, not value SMT",
        ))
    for stage, label, evidence, reason in boundaries:
        yield {
            "id": rewrite.stable_rule_id(stage, target, label),
            "kind": "boundary",
            "target": target,
            "evidence": evidence,
            "unknown": reason,
        }, None


def _inventory(context):
    entries = []
    databases = {}
    for db, filename, _, rules in rewrite.generated_rule_banks(context):
        name = db["architecture"]
        instructions = {row["name"]: row for row in db["instructions"]}
        databases[name] = instructions
        entries.extend(_rewrite_row(name, filename, row, instructions) for row in rules)

    if databases:
        stems = {}
        pattern_files = {}
        for db in isel._context_databases(context):
            stem = isel._sanitize_architecture(db["architecture"])
            stems[stem] = stems.get(stem, 0) + 1
            suffix = "" if stems[stem] == 1 else f"_{stems[stem]}"
            # Mirror ISel.emit's file map, including precedence when a
            # sanitized name and another name with a suffix coincide.
            pattern_files[f"ISelPattern_{stem}{suffix}.i"] = db
        for filename, db in pattern_files.items():
            for row in isel._patterns(db):
                instruction = databases[db["architecture"]][row["target"]]
                entries.append(_isel_row(db, row, instruction, filename))
    entries.extend(_boundaries(context, databases))
    entries.sort(key=lambda entry: entry[0]["id"])
    ids = {entry["id"] for entry, _ in entries}
    if len(ids) != len(entries):
        raise ArchonError("duplicate generated proof rule IDs")
    return entries


def emit(context: dict) -> dict[str, str]:
    """Emit one self-contained Python runner and an SMT-LIB2 query corpus."""
    pieces = [
        "; Generated by Archon. Static pattern inventory, not a runtime trace.\n",
        "; Each modeled query asks for unequal value results in a pure bit-vector fragment.\n",
        "; Target terms model the computed register value, not physical state or encoding.\n",
        "; Unknown entries and verification boundaries contain no SMT assertions.\n",
    ]
    for entry, query in _inventory(context):
        pieces.append(
            "; archon-rule " + json.dumps(entry, sort_keys=True, separators=(",", ":")) + "\n"
        )
        if query is not None:
            pieces.extend((query, "(reset)\n"))
    corpus = "".join(pieces)
    if len(corpus.encode("utf-8")) > proof_cli.CORPUS_MAX_BYTES:
        raise ArchonError("generated proof corpus exceeds its reader's byte bound")
    digest = hashlib.sha256(corpus.encode("utf-8")).hexdigest()
    engine = (_TEMPLATES / "proof_engine.py").read_text(encoding="utf-8")
    cli = (_TEMPLATES / "proof_cli.py").read_text(encoding="utf-8")
    return {
        "Proof.py": (
            "#!/usr/bin/env python3\n# Generated by Archon.\n" + engine.rstrip() + "\n\n"
            + f"_PC_CORPUS_SHA256 = {digest!r}\n\n" + cli.rstrip()
            + "\n\nif __name__ == '__main__':\n"
            + "    raise SystemExit(_pc_main(solve, _PC_CORPUS_SHA256))\n"
        ),
        "Proofs.smt": corpus,
    }
