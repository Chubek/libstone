"""Python runtime copied into generated Translate.py (no Archon imports)."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile


_SPEC_BYTES = 32 * 1024
_MAPPING_BYTES = 128 * 1024
_SOURCE_BYTES = 64 * 1024
_MAX_DEPTH = 192
_MAX_NODES = 16384
_MAX_WORK = 2000000
_MAX_OPERATIONS = 4096
_IDENTIFIER = re.compile(rb"[A-Za-z_][A-Za-z_0-9]{0,63}")
_DECIMAL = re.compile(rb"[+-]?[0-9]{1,20}")


class TranslationError(Exception):
    pass


def _read(path, limit):
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_NONBLOCK", 0))
    with os.fdopen(fd, "rb") as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode):
            raise TranslationError(f"{path}: expected a regular file")
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise TranslationError(f"{path}: exceeds the {limit}-byte limit")
    if b"\0" in data:
        raise TranslationError(f"{path}: NUL bytes are not allowed")
    try:
        data.decode("utf-8")
    except UnicodeError as exc:
        raise TranslationError(f"{path}: expected UTF-8 input") from exc
    return data, info


def _check_output(path, inputs):
    try:
        info = path.lstat()
    except FileNotFoundError:
        return
    if not stat.S_ISREG(info.st_mode):
        raise TranslationError(f"{path}: output must be a regular file, not a symlink")
    if any(os.path.samestat(info, source) for source in inputs):
        raise TranslationError(f"{path}: output aliases an input")


def _publish(path, text, inputs):
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="ascii", newline="\n", prefix=".archon-translate-",
            dir=path.parent, delete=False,
        ) as stream:
            temporary = stream.name
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        _check_output(path, inputs)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            Path(temporary).unlink(missing_ok=True)


_ASDL_CHECK = r"""
do './asdl.pl';
die $@ if $@;
die "cannot load embedded asdl.pl: $!\n" if !defined &main;
my $file = 'schema.asdl';
my $envelope = parse_envelope(slurp_input($file), $file);
die "native ASDL C sections and schema directives are not supported\n"
    if $envelope->{prologue} || $envelope->{epilogue} || keys %{$envelope->{options}};
die "ASDL schema exceeds 2048 tokens\n" if @{$envelope->{tokens}} > 2048;
for my $token (@{$envelope->{tokens}}) {
    fail($token, "ASDL token exceeds 64 bytes") if length($token->{value}) > 64;
    fail($token, "ASDL parameterized containers and optional fields are not supported")
        if $token->{value} eq '[' || $token->{value} eq '?';
}
main('--check', '--schema', '-', $file);
"""


def _embedded_specifications(asdl_source, aurocks_source, aurocks_adapter,
                             grammar_path, grammar_data, schema_path, schema_data):
    perl = shutil.which("perl")
    if perl is None:
        raise TranslationError("Perl 5.14 or newer is required to run the embedded tools")
    perl = os.path.abspath(perl)

    def invoke(workspace, tool, subject, args):
        try:
            result = subprocess.run(
                [perl, *args], cwd=workspace, stdin=subprocess.DEVNULL,
                capture_output=True, timeout=10,
                env={"PATH": os.defpath, "LC_ALL": "C"},
            )
        except subprocess.TimeoutExpired as exc:
            raise TranslationError(f"{subject}: embedded {tool} exceeded its time limit") from exc
        if result.returncode:
            diagnostic = result.stderr.decode("utf-8", errors="backslashreplace").strip()
            raise TranslationError(
                f"{subject}: embedded {tool}: {diagnostic[:2000] or 'validation failed'}"
            )
        try:
            return json.loads(result.stdout)
        except (ValueError, RecursionError) as exc:
            raise TranslationError(f"{subject}: invalid data from embedded {tool}") from exc

    with tempfile.TemporaryDirectory(prefix="archon-translate-") as directory:
        root = Path(directory)
        (root / "asdl.pl").write_bytes(asdl_source)
        (root / "schema.asdl").write_bytes(schema_data)
        # Link the trusted adapter in place of the canonical C-emitting entry
        # point. Caller data never enters a Perl or C source file.
        (root / "aurocks-data.pl").write_bytes(
            aurocks_source.rstrip().removesuffix(b"\nmain();") + b"\n\n" + aurocks_adapter
        )
        (root / "grammar.g").write_bytes(grammar_data)
        grammar = invoke(root, "Aurocks", grammar_path, ["aurocks-data.pl", "grammar.g"])
        schema = invoke(root, "ASDL", schema_path, ["-e", _ASDL_CHECK])
        return grammar, schema


def _inline_mapping(schema, path):
    lines = schema.splitlines()
    marker = b"-- archon-translate-begin"
    if next((line for line in lines if line.strip()), None) != marker:
        return None
    start = lines.index(marker) + 1
    body = []
    for number, line in enumerate(lines[start:], start + 1):
        if line == b"-- archon-translate-end":
            return b"\n".join(body)
        if not line.startswith(b"-- "):
            raise TranslationError(f"{path}:{number}: expected an inline mapping comment starting with '-- '")
        body.append(line[3:])
    raise TranslationError(f"{path}: unterminated inline mapping; expected -- archon-translate-end")


def _read_mapping(data):
    def pairs(entries):
        result = {}
        for key, value in entries:
            if key in result:
                raise TranslationError(f"duplicate JSON key {key!r}")
            result[key] = value
        return result

    def integer(value):
        if len(value) > 10:
            raise TranslationError("mapping integer exceeds ten characters")
        return int(value)

    def not_integer(value):
        raise TranslationError(f"unsupported JSON number {value[:40]!r}")

    try:
        return json.loads(
            data, object_pairs_hook=pairs, parse_int=integer,
            parse_float=not_integer, parse_constant=not_integer,
        )
    except (ValueError, RecursionError) as exc:
        raise TranslationError(f"invalid mapping JSON: {exc}") from exc


@dataclass(frozen=True, slots=True)
class TypeRef:
    name: str
    qualifier: str = ""

    def __str__(self):
        return self.name + self.qualifier

    def accepts(self, actual):
        return self.name == actual.name and (
            self.qualifier == actual.qualifier
            or (self.qualifier == "*" and actual.qualifier == "+")
        )


_SCALAR_TYPES = frozenset(("identifier", "int", "uint", "bool"))


def _schema_types(schema):
    if schema.get("format_version") != 1:
        raise TranslationError("unsupported ASDL schema version")
    names = {entry["name"] for entry in schema["types"]}
    nodes = {}
    for entry in schema["types"]:
        name = entry["name"]
        if entry["attributes"]:
            raise TranslationError(f"ASDL attributes on {name!r} are not supported")
        for node in [entry] if entry["kind"] == "product" else entry["constructors"]:
            fields = {}
            for field in node["fields"]:
                key, field_type = field["name"], TypeRef(field["type"], field["qualifier"])
                if (field["unnamed"] or field_type.name not in names | _SCALAR_TYPES
                        or field_type.qualifier not in ("", "*", "+")):
                    raise TranslationError(
                        f"unsupported ASDL field {node['name']}.{key}: {field_type}; "
                        "use named identifier/int/uint/bool or node fields (optionally * or +)"
                    )
                fields[key] = field_type
            nodes[node["name"]] = (name, fields)
    return names, nodes


def _require_keys(value, keys, subject):
    if not isinstance(value, dict) or value.keys() != set(keys):
        raise TranslationError(f"{subject}: expected exactly {', '.join(sorted(keys))}")
    return value


def _production_type(value, names, subject):
    if isinstance(value, str):
        suffix = value[-1:] if value[-1:] in ("*", "+") else ""
        name = value[:-1] if suffix else value
        if name in names | _SCALAR_TYPES:
            return TypeRef(name, suffix)
    raise TranslationError(f"{subject}: expected an ASDL type, optionally followed by * or +")


def _recipe(spec, expected, rhs_types, nodes, subject):
    def field(position, target):
        if type(position) is not int or not 1 <= position <= len(rhs_types):
            raise TranslationError(f"{subject}: expected a one-based RHS position, got {position!r}")
        source = rhs_types[position - 1]
        if source is None:
            if target.qualifier or target.name not in _SCALAR_TYPES:
                raise TranslationError(f"{subject}: cannot capture terminal ${position} as {target}")
        elif not target.accepts(source):
            raise TranslationError(f"{subject}: ${position} has ASDL type {source}, expected {target}")
        return position - 1, target

    if isinstance(spec, dict):
        if spec.keys() == {"take"}:
            return "take", None, (field(spec["take"], expected),)
        if spec.keys() == {"node", "fields"}:
            tag = spec["node"]
            if not isinstance(tag, str) or tag not in nodes:
                raise TranslationError(f"{subject}: unknown ASDL constructor/product {tag!r}")
            name, fields = nodes[tag]
            if expected != TypeRef(name):
                raise TranslationError(f"{subject}: {tag} builds {name}, expected {expected}")
            provided = _require_keys(spec["fields"], fields, f"{subject} {tag} fields")
            return "node", tag, tuple(field(provided[key], ty) for key, ty in fields.items())
        if spec.keys() == {"list"} and isinstance(spec["list"], list):
            if not expected.qualifier or (expected.qualifier == "+" and not spec["list"]):
                raise TranslationError(f"{subject}: list does not produce required type {expected}")
            if len(spec["list"]) > 16:
                raise TranslationError(f"{subject}: list build exceeds 16 elements")
            item = TypeRef(expected.name)
            return "list", None, tuple(field(index, item) for index in spec["list"])
        if spec.keys() == {"cons"}:
            parts = spec["cons"]
            if expected.qualifier and isinstance(parts, list) and len(parts) == 2:
                return "cons", None, (
                    field(parts[0], TypeRef(expected.name)), field(parts[1], TypeRef(expected.name, "*")),
                )
    raise TranslationError(f"{subject}: expected a take, node/fields, list, or cons build")


@dataclass(frozen=True, slots=True)
class Rule:
    index: int
    symbols: tuple
    precedence: int


_INTEGER_TYPES = {f"{sign}{width}": width for sign in "iu" for width in (8, 16, 32, 64)}
_BINARY = {
    "add": "ADD", "sub": "SUB", "mul": "MUL",
    "and": "AND", "or": "OR", "xor": "XOR",
    "eq": "EQ", "ne": "NE", "lt": "LT",
}
_UNARY = {"neg": "NEG", "not": "NOT"}
_LOWERING = {
    "program": ("program", {"name": "identifier", "functions": "function*"}),
    "function": ("function", {
        "name": "identifier", "params": "parameter*", "result": "identifier",
        "body": "statement*",
    }),
    "parameter": ("parameter", {"name": "identifier", "type": "identifier"}),
    "let": ("statement", {"name": "identifier", "value": "expr"}),
    "let_typed": ("statement", {"name": "identifier", "type": "identifier", "value": "expr"}),
    "return": ("statement", {"value": "expr"}),
    "var": ("expr", {"name": "identifier"}),
    "integer": ("expr", {"value": "integer"}),
    "bool": ("expr", {"value": "bool"}),
    "true": ("expr", {}),
    "false": ("expr", {}),
    **{key: ("expr", {"left": "expr", "right": "expr"}) for key in _BINARY},
    **{key: ("expr", {"value": "expr"}) for key in _UNARY},
}


def _lowering_table(spec, nodes, root_type):
    _require_keys(spec, nodes, "lowering (one entry per ASDL constructor/product)")
    categories, rules = {}, {}
    for tag, (type_name, fields) in nodes.items():
        subject = f"lowering {tag}"
        entry = spec[tag]
        op = entry.get("op") if isinstance(entry, dict) else None
        if not isinstance(op, str) or op not in _LOWERING:
            raise TranslationError(f"{subject}: unknown lowering operation {op!r}")
        role, slots = _LOWERING[op]
        _require_keys(entry, ("op", *slots), subject)
        if categories.setdefault(type_name, role) != role:
            raise TranslationError(f"{subject}: ASDL type {type_name!r} mixes lowering roles")
        keys = [entry[key] for key in slots]
        if any(not isinstance(key, str) for key in keys) or sorted(keys) != sorted(fields):
            raise TranslationError(f"{subject}: bind each ASDL field exactly once")
        rules[tag] = op, {slot: tuple(fields).index(key) for slot, key in zip(slots, keys)}

    if root_type.qualifier or categories.get(root_type.name) != "program":
        raise TranslationError("the start production must build one ASDL program node")
    for tag, (op, indices) in rules.items():
        fields = tuple(nodes[tag][1].values())
        for slot, expected in _LOWERING[op][1].items():
            actual = fields[indices[slot]]
            if expected == "integer":
                valid = actual.name in ("int", "uint") and not actual.qualifier
            elif expected in _SCALAR_TYPES:
                valid = actual == TypeRef(expected)
            else:
                sequence = expected.endswith("*")
                role = categories.get(actual.name)
                valid = (role == expected.rstrip("*") and bool(actual.qualifier) == sequence)
            if not valid:
                raise TranslationError(
                    f"lowering {tag}.{slot}: ASDL field {actual} does not supply {expected}"
                )
    return rules


def _validate_mapping(mapping, grammar, names, nodes):
    _require_keys(mapping, ("version", "integer_type", "productions", "lowering"), "mapping")
    if type(mapping["version"]) is not int or mapping["version"] != 1:
        raise TranslationError("expected mapping version 1")
    integer_type = mapping["integer_type"]
    if not isinstance(integer_type, str) or integer_type not in _INTEGER_TYPES:
        raise TranslationError("integer_type must be one of " + ", ".join(_INTEGER_TYPES))
    rules = {}
    for row in grammar["rules"]:
        rules.setdefault(row["lhs"], []).append(row)
    spec = _require_keys(mapping["productions"], rules, "mapping productions")
    types = {
        name: _production_type(
            _require_keys(spec[name], ("type", "alternatives"), name)["type"], names, name,
        )
        for name in rules
    }
    lowering = _lowering_table(mapping["lowering"], nodes, types[grammar["start"]])
    grouped, recipes = {}, []
    for name, rows in rules.items():
        alternatives = spec[name]["alternatives"]
        if not isinstance(alternatives, list) or len(alternatives) != len(rows):
            raise TranslationError(f"{name}: expected {len(rows)} alternative mappings in grammar order")
        regular, tails = [], []
        for number, (row, alt) in enumerate(zip(rows, alternatives), 1):
            subject = f"{name} alternative {number}"
            _require_keys(alt, ("rhs", "build"), subject)
            rhs = row["rhs"]
            if alt["rhs"] != rhs:
                raise TranslationError(f"{subject}: mapping rhs does not match the grammar; expected {rhs!r}")
            recipes.append(_recipe(alt["build"], types[name], [types.get(s) for s in rhs], nodes, subject))
            symbols = tuple(s if s in types else grammar["terminals"][s] for s in rhs)
            target = tails if symbols and symbols[0] == name else regular
            target.append(Rule(len(recipes) - 1, symbols, row["precedence"]))
        grouped[name] = tuple(
            sorted(group, key=lambda alt: -alt.precedence) for group in (regular, tails)
        )
    return grouped, recipes, lowering, integer_type


@dataclass(frozen=True, slots=True)
class Token:
    start: int
    end: int


@dataclass(frozen=True, slots=True)
class ParseNode:
    start: int
    rule: int
    children: tuple


class Parse:
    def __init__(self, source, path, grammar, rules):
        self.source = source
        self.path = path
        self.rules = rules
        self.start = grammar["start"]
        self.matchers = [self._machine(m) for m in grammar["matchers"]]
        self.layout = self._machine(grammar["skip"]) if grammar["skip"] is not None else None
        self.work = 0
        self.nodes = 0
        self.furthest = 0
        self.memo = {}

    @staticmethod
    def _machine(spec):
        return spec["start"], spec["states"], [bytes.fromhex(mask) for mask in spec["classes"]]

    def fail(self, pos, message):
        line = self.source.count(b"\n", 0, pos) + 1
        column = pos - self.source.rfind(b"\n", 0, pos)
        raise TranslationError(f"{self.path}:{line}:{column}: {message}")

    def tick(self, cost, pos):
        self.work += cost
        if self.work > _MAX_WORK:
            self.fail(pos, "grammar matching exceeded its work limit")

    def _closure(self, stack, states, start, pos):
        seen, ready = set(), []
        source = self.source
        while stack:
            index = stack.pop()
            if index in seen or index < 0:
                continue
            seen.add(index)
            kind, first, second, arg = states[index]
            if kind == "split":
                stack.extend((first, second))
            elif kind == "jmp":
                stack.append(first)
            elif kind == "bol":
                if pos == start:
                    stack.append(first)
            elif kind == "eol":
                if pos == len(source):
                    stack.append(first)
            elif kind == "wordb":
                before = pos > 0 and _word_byte(source[pos - 1])
                after = pos < len(source) and _word_byte(source[pos])
                if (before != after) == arg:
                    stack.append(first)
            else:
                ready.append(index)
        self.tick(len(seen), pos)
        return ready

    def _match(self, machine, start):
        root, states, classes = machine
        source = self.source
        pos = best = start
        ready = self._closure([root], states, start, pos)
        while ready:
            self.tick(len(ready), pos)
            following = []
            byte = source[pos] if pos < len(source) else -1
            for index in ready:
                kind, first, _, arg = states[index]
                if kind == "match":
                    best = pos
                elif byte >= 0 and (
                    (kind == "char" and byte == arg)
                    or (kind == "any" and byte != 10)
                    or (kind == "class" and classes[arg][byte >> 3] >> (byte & 7) & 1)
                ):
                    following.append(first)
            if not following:
                break
            pos += 1
            ready = self._closure(following, states, start, pos)
        return best

    def _skip(self, pos):
        if self.layout is not None:
            while pos < len(self.source):
                end = self._match(self.layout, pos)
                if end == pos:
                    break
                pos = end
        return pos

    def _attempt(self, rule, pos, depth, prefix=None):
        start = pos
        self.tick(len(rule.symbols) + 1, start)
        children = [] if prefix is None else [prefix]
        for symbol in rule.symbols[len(children):]:
            if isinstance(symbol, str):
                found = self._nonterminal(symbol, pos, depth + 1)
                if found is None:
                    return None
                node, pos = found
            else:
                begin = self._skip(pos)
                pos = self._match(self.matchers[symbol], begin)
                if pos == begin:
                    self.furthest = max(self.furthest, begin)
                    return None
                node = Token(begin, pos)
            children.append(node)
        self.nodes += 1
        if self.nodes > _MAX_NODES:
            self.fail(start, "parse tree exceeds its node limit")
        return ParseNode(
            children[0].start if children else start, rule.index, tuple(children),
        ), self._skip(pos) if prefix is None else pos

    def _nonterminal(self, name, pos, depth):
        self.tick(1, pos)
        if depth > _MAX_DEPTH:
            self.fail(pos, "grammar parse is nested too deeply")
        key = (name, pos)
        if key in self.memo:
            return self.memo[key]
        seeds, tails = self.rules[name]
        for seed in seeds:
            found = self._attempt(seed, pos, depth)
            if found is not None:
                while tails:
                    node, end = found
                    for tail in tails:
                        advanced = self._attempt(tail, end, depth, node)
                        if advanced is not None:
                            if advanced[1] <= end:
                                self.fail(end, "left-recursive tail did not consume input")
                            found = advanced
                            break
                    else:
                        break
                break
        else:
            self.furthest = max(self.furthest, pos)
            found = None
        if len(self.memo) >= _MAX_NODES:
            self.fail(pos, "grammar parse exceeded its memo limit")
        self.memo[key] = found
        return found

    def run(self):
        found = self._nonterminal(self.start, 0, 0)
        if found is None:
            self.fail(self.furthest, "source does not match the grammar")
        tree, end = found
        end = self._skip(end)
        if end != len(self.source):
            if self.furthest > end:
                self.fail(self.furthest, "source does not match the grammar")
            self.fail(end, "unexpected trailing source after the start production")
        return tree


def _word_byte(value):
    return 48 <= value <= 57 or 65 <= value <= 90 or 97 <= value <= 122 or value == 95


@dataclass(frozen=True, slots=True)
class Node:
    tag: str
    fields: tuple
    start: int


@dataclass(frozen=True, slots=True)
class Cons:
    head: object
    rest: object


def _items(sequence):
    while isinstance(sequence, Cons):
        yield sequence.head
        sequence = sequence.rest
    yield from sequence


def _typed_token(parser, node, target):
    text = parser.source[node.start:node.end]
    if target.name == "identifier" and _IDENTIFIER.fullmatch(text):
        return text.decode("ascii")
    if target.name == "bool" and text in (b"true", b"false"):
        return text == b"true"
    if target.name in ("int", "uint") and _DECIMAL.fullmatch(text):
        value = int(text)
        if target.name == "int" and -(1 << 63) <= value < (1 << 63):
            return value
        if target.name == "uint" and 0 <= value < (1 << 64):
            return value
    parser.fail(node.start, f"invalid terminal value for ASDL {target}")


def _build_ast(parser, tree, recipes):
    visited = 0

    def build(node, depth):
        nonlocal visited
        visited += 1
        if visited > _MAX_NODES or depth > _MAX_DEPTH:
            parser.fail(node.start, "ASDL tree exceeds its size or depth limit")
        action, tag, fields = recipes[node.rule]
        values = []
        for index, target in fields:
            child = node.children[index]
            values.append(
                _typed_token(parser, child, target) if isinstance(child, Token)
                else build(child, depth + 1)
            )
        if action == "take":
            return values[0]
        if action == "node":
            return Node(tag, tuple(values), node.start)
        if action == "cons":
            return Cons(*values)
        return tuple(values)

    return build(tree, 0)


@dataclass(frozen=True, slots=True)
class Atom:
    type: str
    identifier: str | None = None
    constant: int = 0

    @property
    def type_macro(self):
        return "IR_TYPE_" + self.type.upper()

    def argument(self):
        if self.identifier is not None:
            return f"IR_ARG_VALUE({self.identifier});"
        if self.type == "bool":
            return f"IR_ARG_BOOL({int(self.constant)});"
        bits = self.constant & ((1 << _INTEGER_TYPES[self.type]) - 1)
        return f"IR_ARG_INT({self.type_macro}, UINT64_C(0x{bits:x}));"


class ANF:
    def __init__(self, parser, lowering, integer_type):
        self.parser = parser
        self.lowering = lowering
        self.integer_type = integer_type
        self.lines = ["/* Archon Translate.py: ANF Program.i */"]
        self.terms = 0
        self.operations = 0
        self.values = {}
        self.source_names = set()

    def _fields(self, node):
        op, positions = self.lowering[node.tag]
        return op, {key: node.fields[index] for key, index in positions.items()}

    def _type(self, name, node):
        if name not in _INTEGER_TYPES and name != "bool":
            self.parser.fail(node.start, f"unsupported ANF value type {name!r}")
        return name

    def _op(self, node):
        self.operations += 1
        if self.operations > _MAX_OPERATIONS:
            self.parser.fail(node.start, "ANF program exceeds its operation limit")

    def _new_value(self, type_name):
        while True:
            self.terms += 1
            identifier = f"archon_v_{self.terms}"
            if identifier not in self.source_names:
                return Atom(type_name, identifier)

    def _let(self, node, type_name, primitive, args, name=None):
        self._op(node)
        value = self._new_value(type_name)
        # Names come only from the bounded ASCII identifier conversion.
        if name is None:
            start = f"IR_LET_BEGIN({value.identifier}, "
        else:
            start = f'IR_LET_NAMED_BEGIN({value.identifier}, "{name}", '
        self.lines.append(start + f"{value.type_macro}, IR_PRIM_{primitive}, IR_EFFECT_NONE);")
        self.lines.extend(f"  {arg.argument()}" for arg in args)
        self.lines.append("IR_LET_END();")
        return value

    def _expr(self, node):
        op, fields = self._fields(node)
        if op == "var":
            name = fields["name"]
            if name not in self.values:
                self.parser.fail(node.start, f"unknown name {name!r}")
            return self.values[name]
        if op == "integer":
            value = fields["value"]
            width = _INTEGER_TYPES[self.integer_type]
            signed = self.integer_type.startswith("i")
            low = -(1 << (width - 1)) if signed else 0
            high = 1 << (width - 1 if signed else width)
            if not low <= value < high:
                self.parser.fail(node.start, f"integer {value} does not fit {self.integer_type}")
            return Atom(self.integer_type, constant=value)
        if op in ("bool", "true", "false"):
            return Atom("bool", constant=fields["value"] if op == "bool" else op == "true")
        if op in _UNARY:
            operand = self._expr(fields["value"])
            if operand.type not in _INTEGER_TYPES:
                self.parser.fail(node.start, f"{op} requires an integer operand")
            return self._let(node, operand.type, _UNARY[op], (operand,))
        if op in _BINARY:
            left, right = self._expr(fields["left"]), self._expr(fields["right"])
            if left.type != right.type:
                self.parser.fail(node.start, f"{op} operands have different types")
            result = left.type
            primitive = _BINARY[op]
            if op in ("eq", "ne", "lt"):
                result = "bool"
            if op not in ("eq", "ne") and left.type not in _INTEGER_TYPES:
                self.parser.fail(node.start, f"{op} requires integer operands")
            if op == "lt":
                primitive = "SLT" if left.type.startswith("i") else "ULT"
            return self._let(node, result, primitive, (left, right))
        raise AssertionError(f"invalid expression operation {op!r}")

    def _function(self, node, index):
        _, fields = self._fields(node)
        name = fields["name"]
        result = self._type(fields["result"], node)
        params = tuple((param, self._fields(param)[1]) for param in _items(fields["params"]))
        body = tuple((stmt, *self._fields(stmt)) for stmt in _items(fields["body"]))
        self.source_names = {attrs["name"] for _, attrs in params}
        self.source_names.update(attrs["name"] for _, op, attrs in body if op != "return")
        self.values = {}
        self.lines.append(f'IR_FUNCTION_BEGIN(archon_fn_{index}, "{name}", IR_TYPE_{result.upper()});')
        for param, attrs in params:
            key = attrs["name"]
            if key in self.values:
                self.parser.fail(param.start, f"duplicate parameter {key!r}")
            value = self._new_value(self._type(attrs["type"], param))
            self.values[key] = value
            self.lines.append(f'IR_PARAM({value.identifier}, "{key}", {value.type_macro});')
        self.lines.append('IR_BLOCK_BEGIN(entry, "entry");')
        returned = False
        for stmt, op, attrs in body:
            if returned:
                self.parser.fail(stmt.start, "statement after return")
            if op in ("let", "let_typed"):
                key = attrs["name"]
                if key in self.values:
                    self.parser.fail(stmt.start, f"duplicate binding {key!r}")
                value = self._expr(attrs["value"])
                if op == "let_typed" and value.type != self._type(attrs["type"], stmt):
                    self.parser.fail(stmt.start, "initializer type does not match the declared type")
                self.values[key] = self._let(stmt, value.type, "COPY", (value,), key)
            else:
                value = self._expr(attrs["value"])
                if value.type != result:
                    self.parser.fail(stmt.start, f"return has type {value.type}, expected {result}")
                if value.identifier is None:
                    value = self._let(stmt, value.type, "COPY", (value,))
                self._op(stmt)
                self.lines.append(f"IR_RETURN({value.identifier});")
                returned = True
        if not returned:
            self.parser.fail(node.start, "function body must end in return")
        self.lines.extend(("IR_BLOCK_END();", "IR_FUNCTION_END();"))

    def emit(self, root):
        _, attrs = self._fields(root)
        self.lines.append(f'IR_PROGRAM_BEGIN("{attrs["name"]}");')
        names = set()
        functions = tuple(_items(attrs["functions"]))
        for function in functions:
            _, fields = self._fields(function)
            name = fields["name"]
            if name in names:
                self.parser.fail(function.start, f"duplicate function {name!r}")
            names.add(name)
        if not names:
            self.parser.fail(root.start, "program must contain at least one function")
        index = 0
        for function in functions:
            index += 1
            # The ANF decoder resolves functions by either key or display name.
            while f"archon_fn_{index}" in names:
                index += 1
            self._function(function, index)
        self.lines.append("IR_PROGRAM_END();")
        return "\n".join(self.lines) + "\n"


def main(representation, asdl_source, aurocks_source, aurocks_adapter):
    cli = argparse.ArgumentParser(
        prog="Translate.py", description=f"Translate a data-only Aurocks/ASDL language to {representation} Program.i",
        allow_abbrev=False,
    )
    cli.add_argument("--grammar", type=Path, required=True, help="data-only Aurocks .g file")
    cli.add_argument("--absyn", type=Path, required=True, help="data-only ASDL .asdl file")
    cli.add_argument(
        "--mapping", type=Path,
        help="JSON semantic mapping (default: inline ASDL or <grammar stem>.translate.json)",
    )
    cli.add_argument("--out", type=Path, required=True, help="output path (published atomically)")
    cli.add_argument("source", type=Path, help="UTF-8 source file")
    args = cli.parse_args()
    try:
        if representation != "anf":
            raise TranslationError(
                f"representation {representation!r} is not supported by this translator; "
                "use an ANF generated tool or supply a native Program.i to the selected IR"
            )
        grammar_data, grammar_info = _read(args.grammar, _SPEC_BYTES)
        schema_data, schema_info = _read(args.absyn, _SPEC_BYTES)
        source, source_info = _read(args.source, _SOURCE_BYTES)
        inputs = (grammar_info, schema_info, source_info, Path(__file__).stat())
        mapping_data = _inline_mapping(schema_data, args.absyn) if args.mapping is None else None
        if mapping_data is None:
            mapping_path = args.mapping if args.mapping is not None else args.grammar.with_suffix(".translate.json")
            try:
                mapping_data, mapping_info = _read(mapping_path, _MAPPING_BYTES)
            except FileNotFoundError as exc:
                raise TranslationError(
                    f"{mapping_path}: an explicit semantic mapping is required (use --mapping)"
                ) from exc
            mapping_subject = str(mapping_path)
            inputs += (mapping_info,)
        else:
            mapping_subject = f"{args.absyn} inline mapping"
        _check_output(args.out, inputs)
        grammar, schema = _embedded_specifications(
            asdl_source, aurocks_source, aurocks_adapter,
            args.grammar, grammar_data, args.absyn, schema_data,
        )
        try:
            names, nodes = _schema_types(schema)
        except TranslationError as exc:
            raise TranslationError(f"{args.absyn}: {exc}") from exc
        try:
            rules, recipes, lowering, integer_type = _validate_mapping(
                _read_mapping(mapping_data), grammar, names, nodes,
            )
        except TranslationError as exc:
            raise TranslationError(f"{mapping_subject}: {exc}") from exc
        parser = Parse(source, args.source, grammar, rules)
        root = _build_ast(parser, parser.run(), recipes)
        _publish(args.out, ANF(parser, lowering, integer_type).emit(root), inputs)
    except (TranslationError, OSError, ValueError) as exc:
        print(f"Translate.py: {exc}", file=sys.stderr)
        return 1
    return 0
