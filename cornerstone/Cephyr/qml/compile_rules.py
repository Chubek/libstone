#!/usr/bin/env python3
"""Compile QML rule fragments using the LLSynth-generated Python frontend."""
import argparse
import importlib.util
import os
from pathlib import Path
import stat
import sys


HEADER = "CEPHYR-QML-1"
MAX_INPUT_BYTES = 64 * 1024
MAX_DIRECTIVES = 128
MAX_TERMS = 4096
MAX_DEPTH = 32
MAX_ARITY = 32
MAX_IDENTIFIER = 64


class CompileError(ValueError):
    pass


def load_frontend(path):
    # This is the trusted generated tool, not one of the source fragments.
    spec = importlib.util.spec_from_file_location("_cephyr_qml_frontend", path)
    if spec is None or spec.loader is None:
        raise CompileError(f"{path}: expected an LLSynth-generated Python frontend")
    frontend = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = frontend
    spec.loader.exec_module(frontend)
    if (getattr(frontend, "SPEC", {}).get("language") != "QML"
            or getattr(frontend, "MODE", None) != "interpret"):
        raise CompileError(f"{path}: expected the QML frontend generated with --mode=interpret")
    return frontend


class Limits:
    """Check the generated parse tree before walking its semantic actions."""

    def __init__(self):
        self.directives = 0
        self.terms = 0

    def check(self, root):
        pending = [(root, 0)]
        while pending:
            node, depth = pending.pop()
            if node.symbol == "ProgramEnd" and node.children:
                self.fail(node, "'run' is only supported by the expression tool")
            if node.symbol == "Directive":
                self.directives += 1
                if self.directives > MAX_DIRECTIVES:
                    self.fail(node, f"too many directives (maximum {MAX_DIRECTIVES})")
            if node.symbol == "Tree":
                depth += 1
                self.terms += 1
                if depth > MAX_DEPTH:
                    self.fail(node, f"tree nesting exceeds {MAX_DEPTH}")
                if self.terms > MAX_TERMS:
                    self.fail(node, f"too many tree terms (maximum {MAX_TERMS})")
            if node.symbol == "ArgList":
                self.check_arity(node)
            if node.symbol == "ID" and len(node.token.lexeme) > MAX_IDENTIFIER:
                self.fail(node, f"identifier is longer than {MAX_IDENTIFIER} characters")
            pending.extend((child, depth) for child in reversed(node.children))

    def check_arity(self, args):
        # Do not descend into Tree here: each immediate Tree is one argument,
        # regardless of the generated repetition nodes surrounding it.
        pending = [args]
        count = 0
        while pending:
            node = pending.pop()
            if node.symbol == "Tree":
                count += 1
                if count > MAX_ARITY:
                    self.fail(node, f"constructor has more than {MAX_ARITY} arguments")
            else:
                pending.extend(reversed(node.children))

    @staticmethod
    def fail(node, message):
        token = node.token
        raise CompileError(f"{token.line}:{token.column}: {message}")


def tree_tokens(tree):
    tag = tree[0]
    if tag == "node":
        _, name, children = tree
        yield from ("n", name, str(len(children)))
        for child in children:
            yield from tree_tokens(child)
    elif tag == "wild":
        yield "w"
    elif tag == "bool":
        yield from ("b", "1" if tree[1] else "0")
    else:
        yield from ({"int": "i", "meta": "m", "sym": "s"}[tag], str(tree[1]))


def encode_directive(record):
    fields = 2 if record[0] == "P" else 3
    tokens = record[:fields]
    for tree in record[fields:]:
        tokens.extend(tree_tokens(tree))
    return " ".join(tokens)


def compile_rules(frontend, files):
    lines = [HEADER]
    limits = Limits()
    remaining = MAX_INPUT_BYTES
    for path in files:
        try:
            # Admit only regular files, checking the opened descriptor so a
            # path changed after admission cannot replace it with a FIFO.
            with os.fdopen(os.open(path, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY), "rb") as file:
                if not stat.S_ISREG(os.fstat(file.fileno()).st_mode):
                    raise CompileError("expected a regular rule file")
                data = file.read(remaining + 1)
            if len(data) > remaining:
                raise CompileError(f"total rule input exceeds {MAX_INPUT_BYTES} bytes")
            remaining -= len(data)
            root = frontend.parse(data.decode("utf-8"))
            limits.check(root)
            frontend.Context().walk(root, "semantics")
            lines.extend(encode_directive(record) for record in root.value)
        except (CompileError, frontend.LanguageError, OSError, UnicodeError) as exc:
            raise CompileError(f"{path}: {exc}") from None
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frontend", required=True, type=Path, help="generated QML Python frontend")
    parser.add_argument("--output", required=True, type=Path, help="compiled rule file")
    parser.add_argument("files", nargs="+", type=Path, metavar="FILE")
    args = parser.parse_args()
    try:
        result = compile_rules(load_frontend(args.frontend), args.files)
        args.output.write_bytes(result.encode("ascii"))
    except (CompileError, OSError) as exc:
        print(f"QML: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
