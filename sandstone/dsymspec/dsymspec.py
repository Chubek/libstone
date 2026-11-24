#!/usr/bin/env python3
"""Evaluate the LLSynth parse tree and write a standalone C11 macro file."""

import argparse
import importlib.util
import math
import os
from pathlib import Path
import re
import stat
import sys
import tempfile
from dataclasses import dataclass

from dsymspec_emit import DeclarationError, emit_header


KINDS = ("target", "feature", "register", "mnemonic", "encoder", "form",
         "syntax", "section", "linkage", "reloc", "debug")
MAX_FILE = 512 * 1024
MAX_STRING = 65536
MAX_ITEMS = 20000
# Standard profiles expand declarative families (notably x86 and RVV) into thousands
# of records. This separately bounds computation that emits no records.
MAX_EVAL_STEPS = 20_000_000
MAX_EVAL_STRING_BYTES = 24 * 1024 * 1024
STRING_METHODS = {"find", "sub", "format", "gsub", "gmatch", "match", "rep"}
STRING_PATTERNS = {"%x%x": "[A-Fa-f0-9]{2}", "%S+": r"[^\t-\r ]+",
                   "%.": r"\.", " ": " ", "^[sdq]$": r"^[sdq]$"}
FORMAT = re.compile(r"%(?:0?[1-9][0-9]?)?[dsxX]|%%")
INTEGER_MASK = (1 << 64) - 1
PROFILES = {"rv64im.dsymspec": ("rv64im", "rv64im"),
            "riscv32.dsymspec": ("rv32gc", "riscv32")}


class SpecError(ValueError):
    pass


class Table:
    def __init__(self, fields=()):
        self.fields = dict(fields)
        self.prefix = 0
        self.extend_prefix()

    def extend_prefix(self):
        while self.prefix + 1 in self.fields:
            self.prefix += 1

    def get(self, key):
        check_key(key)
        return self.fields.get(key)

    def set(self, key, value):
        check_key(key)
        if value is None:
            self.fields.pop(key, None)
            if type(key) is int and 1 <= key <= self.prefix:
                self.prefix = key - 1
        else:
            self.fields[key] = value
            if key == self.prefix + 1:
                self.extend_prefix()
        if len(self.fields) > MAX_ITEMS:
            raise SpecError("table size limit exceeded")

    def length(self):
        return self.prefix

    def array(self):
        return tuple(self.fields[i] for i in range(1, self.length() + 1))


@dataclass
class Cell:
    value: object


class Scope:
    def __init__(self, parent=None):
        self.parent = parent
        self.locals = {}

    def find(self, name):
        s = self
        while s is not None:
            if name in s.locals:
                return s.locals[name]
            s = s.parent
        raise SpecError(f"unknown name {name!r}")

    def capture(self):
        result = Scope(self.parent.capture() if self.parent else None)
        result.locals.update(self.locals)
        return result


@dataclass(eq=False)
class Closure:
    parameters: tuple
    block: object
    scope: Scope


@dataclass(eq=False)
class Builtin:
    name: str


@dataclass
class Iteration:
    values: tuple


@dataclass
class SourceLocation:
    file: Path
    line: int
    column: int

    def __str__(self):
        return f"{self.file}:{self.line}:{self.column}"


def check_key(key):
    if type(key) not in (str, int):
        raise SpecError("table keys must be strings or integers")


def truth(value):
    return value is not None and value is not False


def first(values):
    return values[0] if values else None


def padded(values, size):
    return (tuple(values) + (None,) * size)[:size]


def text(node):
    return node.token.lexeme


def sequence(node):
    """Flatten the grammar's left-recursive list rules without Python recursion."""
    result = []
    while node.children:
        children = node.children
        if children[0].symbol != node.symbol:
            result.append(children[0])
            break
        result.append(children[-1])
        node = children[0]
    result.reverse()
    return result


def check_string(value):
    if not isinstance(value, str):
        raise SpecError("expected a string")
    if len(value) > MAX_STRING:
        raise SpecError("string size limit exceeded")
    return value


def parse_string(lexeme):
    """Decode short-string escapes. Latin-1 transports Lua bytes through str."""
    raw = lexeme[1:-1]
    pieces = []
    escapes = {"a": "\a", "b": "\b", "f": "\f", "n": "\n", "r": "\r",
               "t": "\t", "v": "\v", "\\": "\\", '"': '"', "'": "'"}
    i = 0
    while i < len(raw):
        start = raw.find("\\", i)
        if start < 0:
            pieces.append(raw[i:].encode("utf-8").decode("latin-1"))
            break
        pieces.append(raw[i:start].encode("utf-8").decode("latin-1"))
        i = start + 2
        e = raw[start + 1]
        if e in escapes:
            pieces.append(escapes[e])
        elif e == "x":
            digits = raw[i:i + 2]
            if not re.fullmatch(r"[0-9A-Fa-f]{2}", digits):
                raise SpecError("invalid hexadecimal string escape")
            pieces.append(chr(int(digits, 16)))
            i += 2
        elif e in "0123456789":
            digits = re.match(r"[0-9]{1,3}", raw[i - 1:i + 2])[0]
            value = int(digits)
            if value > 255:
                raise SpecError("decimal string escape exceeds 255")
            pieces.append(chr(value))
            i += len(digits) - 1
        elif e == "u":
            match = re.match(r"\{([0-9A-Fa-f]{1,8})\}", raw[i:i + 10])
            if not match:
                raise SpecError("invalid Unicode string escape")
            value = int(match[1], 16)
            if not 0 <= value <= 0x10FFFF or 0xD800 <= value <= 0xDFFF:
                raise SpecError("Unicode escape is not a scalar value")
            pieces.append(chr(value).encode("utf-8").decode("latin-1"))
            i += len(match[0])
        elif e == "z":
            while i < len(raw) and raw[i] in " \t\r\n\f\v":
                i += 1
        else:
            raise SpecError(f"unsupported string escape \\{e}")
    return check_string("".join(pieces))


def number(value):
    if type(value) not in (int, float):
        raise SpecError("expected a number")
    return value


def integer(value):
    if type(value) is not int:
        if type(value) is float and value.is_integer() and -(1 << 63) <= value < 1 << 63:
            return int(value)
        raise SpecError("expected an integer")
    return value


def int64(value):
    return ((value + (1 << 63)) & INTEGER_MASK) - (1 << 63)


def lua_text(value):
    if type(value) is str:
        return value
    if type(value) in (int, float):
        return str(value)
    raise SpecError("expected a string or number")


def lua_equal(left, right):
    if type(left) in (int, float) and type(right) in (int, float):
        return left == right
    return type(left) is type(right) and left == right


def binary(op, left, right):
    if op == "==":
        return lua_equal(left, right)
    if op == "~=":
        return not lua_equal(left, right)
    if op == "..":
        return check_string(lua_text(left) + lua_text(right))
    if op in ("<", ">", "<=", ">="):
        if type(left) is not str or type(right) is not str:
            left, right = number(left), number(right)
        return {"<": left < right, ">": left > right,
                "<=": left <= right, ">=": left >= right}[op]
    if op in ("<<", ">>"):
        shift = integer(right) * (1 if op == "<<" else -1)
        value = integer(left) & INTEGER_MASK
        if not -64 < shift < 64:
            return 0
        return int64(value << shift if shift >= 0 else value >> -shift)
    left, right = number(left), number(right)
    if op == "+":
        value = left + right
    elif op == "-":
        value = left - right
    elif op == "*":
        value = left * right
    elif op == "^":
        value = math.pow(left, right)
    elif op == "/":
        value = left / right
    elif op == "//":
        value = left // right
    elif op == "%":
        value = left % right
    else:
        raise SpecError(f"unsupported operator {op}")
    if type(value) is int:
        return int64(value)
    if not math.isfinite(value):
        raise SpecError("non-finite arithmetic result")
    return value


class Evaluator:
    def __init__(self, parser, path):
        self.parser = parser
        self.path = path
        self.declarations = []
        self.fuel = MAX_EVAL_STEPS
        self.string_budget = MAX_EVAL_STRING_BYTES
        self.location = None
        self.profile = None
        self.arch = None
        self.emit_marker = object()
        self.loaded = False

    def step(self):
        self.fuel -= 1
        if self.fuel < 0:
            raise SpecError("evaluation step limit exceeded")

    def site(self, node):
        return SourceLocation(self.path, node.token.line, node.token.column)

    def parse(self, path):
        with open(path, "rb", opener=self.open_source) as f:
            data = f.read(MAX_FILE + 1)
        if len(data) > MAX_FILE:
            raise SpecError("input size limit exceeded")
        try:
            return self.parser.parse(data.decode("utf-8"))
        except self.parser.LanguageError as exc:
            raise SpecError(f"{path}:{exc}") from None

    def open_source(self, path, flags):
        # Reject symlink traversal for implicit profile inputs and avoid
        # blocking on FIFOs (the selected source may be a symlink).
        if self.loaded:
            flags |= getattr(os, "O_NOFOLLOW", 0)
        fd = os.open(path, flags | getattr(os, "O_NONBLOCK", 0))
        if not stat.S_ISREG(os.fstat(fd).st_mode):
            os.close(fd)
            raise SpecError("input must be a regular file")
        return fd

    def new_string(self, value):
        self.string_budget -= len(value)
        if self.string_budget < 0:
            raise SpecError("cumulative string size limit exceeded")
        return value

    def globals(self):
        scope = Scope()
        names = ("assert", "require", "loadfile", "print", "ipairs", "pairs",
                 "tonumber", "tostring", "type")
        scope.locals.update({name: Cell(Builtin(name)) for name in names})
        modules = {
            "string": STRING_METHODS,
            "table": ("concat", "insert", "sort", "unpack"),
            "math": ("min",),
        }
        for module, names in modules.items():
            scope.locals[module] = Cell(Table((k, Builtin(f"{module}.{k}")) for k in names))
        scope.locals["math"].value.set("mininteger", -(1 << 63))
        scope.locals["math"].value.set("maxinteger", (1 << 63) - 1)
        scope.locals["..."] = Cell(self.profile)
        return scope

    def run(self):
        root = self.parse(self.path)
        self.block(root.children[0], self.globals())
        targets = [record for kind, record, _ in self.declarations if kind == "target"]
        if len(targets) != 1:
            raise SpecError("exactly one target declaration is required")
        declared_arch = targets[0].get("arch")
        if declared_arch != self.arch:
            raise SpecError("target arch does not match require('backend').new(arch)")
        return PROFILES[self.path.name][1] if self.loaded else self.arch

    def block(self, node, scope):
        statements, return_part = node.children
        for statement in sequence(statements):
            self.location = self.site(statement)
            returned = self.statement(statement, scope)
            if returned is not None:
                return returned
        if return_part.children:
            self.location = self.site(return_part)
            return self.values(return_part.children[1], scope)
        return None

    def statement(self, node, scope):
        self.step()
        c = node.children
        head = text(c[0])
        if head == ";":
            return
        if head == "local":
            if text(c[1]) == "function":
                name = text(c[2])
                scope.locals[name] = cell = Cell(None)
                cell.value = self.closure(c[3], c[4], scope)
            else:
                children = c[2].children
                values = self.values(children[1], scope) if children else ()
                names = sequence(c[1])
                for name, value in zip(names, padded(values, len(names))):
                    scope.locals[text(name)] = Cell(value)
        elif head == "do":
            return self.block(c[1], Scope(scope))
        elif head == "if":
            return self.conditional(c[1], c[3], c[4], scope)
        elif head == "for":
            if text(c[2]) == "=":
                names = (text(c[1]),)
                low = integer(self.value(c[3], scope))
                high = integer(self.value(c[5], scope))
                step = integer(self.value(c[6].children[1], scope)) if c[6].children else 1
                if step == 0:
                    raise SpecError("numeric for loop step must not be zero")
                rows = ((i,) for i in range(low, high + (1 if step > 0 else -1), step))
                block = c[8]
            else:
                names = tuple(text(n) for n in sequence(c[1]))
                iterator = first(self.values(c[3], scope))
                if not isinstance(iterator, Iteration):
                    raise SpecError("expected an ipairs, pairs or string.gmatch iterator")
                rows, block = iterator.values, c[5]
            for values in rows:
                self.step()
                child = Scope(scope)
                child.locals.update((n, Cell(v)) for n, v in zip(names, padded(values, len(names))))
                returned = self.block(block, child)
                if returned is not None:
                    return returned
        else:
            tail = c[1].children
            if not tail:
                children = c[0].children
                if len(children) != 2 or children[1].children[-1].symbol != "CallArgs":
                    raise SpecError("expected an assignment or function call")
                self.eval(c[0], scope)
            else:
                nodes = [c[0], *sequence(tail[0])]
                slots = [self.lvalue(target, scope) for target in nodes]
                values = self.values(tail[2], scope)
                for slot, value in zip(slots, padded(values, len(slots))):
                    if isinstance(slot, Cell):
                        slot.value = value
                    else:
                        table, key = slot
                        table.set(key, value)
        return None

    def conditional(self, condition, block, other, scope):
        if truth(self.value(condition, scope)):
            return self.block(block, Scope(scope))
        elif other.children:
            c = other.children
            if text(c[0]) == "else":
                return self.block(c[1], Scope(scope))
            return self.conditional(c[1], c[3], c[4], scope)
        return None

    def lvalue(self, node, scope):
        if node.symbol == "Postfix":
            node = node.children[0]
        c = node.children
        if len(c) == 1:
            return scope.find(text(c[0]))
        if len(c) != 2:
            raise SpecError("invalid assignment target")
        table, suffix = self.value(c[0], scope), c[1].children
        if not isinstance(table, Table) or len(suffix) not in (2, 3):
            raise SpecError("assignment requires a table field")
        if text(suffix[0]) == ".":
            return table, text(suffix[1])
        if text(suffix[0]) == "[":
            return table, self.value(suffix[1], scope)
        raise SpecError("invalid assignment target")

    def values(self, node, scope):
        exprs = sequence(node)
        if not exprs:
            return ()
        return (*[self.value(n, scope) for n in exprs[:-1]], *self.eval(exprs[-1], scope))

    def value(self, node, scope):
        return first(self.eval(node, scope))

    def closure(self, parameters, block, scope):
        names = parameters.children[1].children
        return Closure(tuple(text(n) for n in sequence(names[0])) if names else (),
                       block, scope.capture())

    def eval(self, node, scope):
        self.step()
        c = node.children
        symbol = node.symbol
        if symbol == "Name":
            return (scope.find(text(node)).value,)
        if symbol == "StringLiteral":
            return (self.new_string(parse_string(text(node))),)
        if symbol == "Number":
            spelling = text(node)
            base = 16 if spelling[:2].lower() == "0x" else 10
            if len(spelling) > 21:
                raise SpecError("integer literal is out of range")
            value = int(spelling, base)
            if value > INTEGER_MASK:
                raise SpecError("integer literal is out of range")
            return (int64(value),)
        if not c:
            return ({"true": True, "false": False, "nil": None,
                     "...": scope.find("...").value}[text(node)],)
        if symbol == "Table":
            return (self.make_table(c[1], scope),)
        if symbol == "Atom" and len(c) == 4:
            return (self.closure(c[1], c[2], scope),)
        if symbol in ("NamedPostfix", "ParenPostfix"):
            if len(c) == 3:
                return (self.value(c[1], scope),)
            if len(c) == 2:
                base, suffix = self.value(c[0], scope), c[1].children
                if len(suffix) == 1:
                    return self.call(base, self.arguments(suffix[0], scope))
                if text(suffix[0]) == ":":
                    method = self.field(base, text(suffix[1]))
                    return self.call(method, (base, *self.arguments(suffix[2], scope)))
                key = text(suffix[1]) if text(suffix[0]) == "." else self.value(suffix[1], scope)
                return (self.field(base, key),)
        if len(c) == 1:
            return self.eval(c[0], scope)
        if symbol == "Unary":
            op, value = text(c[0]), self.value(c[1], scope)
            if op == "not":
                return (not truth(value),)
            if op == "#":
                if isinstance(value, Table):
                    return (value.length(),)
                return (len(check_string(value)),)
            return (int64(-value) if type(value) is int else -number(value),)
        op, left = text(c[1]), self.value(c[0], scope)
        if op == "and":
            return (self.value(c[2], scope) if truth(left) else left,)
        if op == "or":
            return (left if truth(left) else self.value(c[2], scope),)
        result = binary(op, left, self.value(c[2], scope))
        return (self.new_string(result) if op == ".." else result,)

    def make_table(self, fields, scope):
        result, list_index = Table(), 1
        while fields.children:
            field, tail = fields.children
            c = field.children
            if len(c) == 1:
                values = self.eval(c[0], scope) if not tail.children or not tail.children[1].children \
                    else (self.value(c[0], scope),)
                for value in values:
                    result.set(list_index, value)
                    list_index += 1
            elif len(c) == 3:
                result.set(text(c[0]), self.value(c[2], scope))
            else:
                result.set(self.value(c[1], scope), self.value(c[4], scope))
            if not tail.children:
                break
            fields = tail.children[1]
        return result

    def arguments(self, node, scope):
        c = node.children
        if len(c) == 1:
            return (self.value(c[0], scope),)
        return self.values(c[1].children[0], scope) if c[1].children else ()

    @staticmethod
    def field(table, key):
        if isinstance(table, Table):
            return table.get(key)
        if isinstance(table, str) and key in STRING_METHODS:
            return Builtin(f"string.{key}")
        raise SpecError(f"cannot read field {key!r} from a non-table")

    def call(self, function, args):
        self.step()
        if isinstance(function, Closure):
            scope = Scope(function.scope)
            scope.locals.update((n, Cell(v)) for n, v in
                                zip(function.parameters, padded(args, len(function.parameters))))
            old_location = self.location
            result = self.block(function.block, scope)
            self.location = old_location
            return result if result is not None else ()
        if not isinstance(function, Builtin):
            raise SpecError("attempt to call a value not defined by the DSymspec DSL")
        result = self.builtin(function.name, args)
        if function.name.startswith("string.") or function.name in ("table.concat", "tostring"):
            for element in result:
                if isinstance(element, str):
                    self.new_string(element)
        return result

    @staticmethod
    def arity(name, args, low, high=None):
        high = low if high is None else high
        if not low <= len(args) <= high:
            allowed = str(low) if low == high else f"{low}–{high}"
            raise SpecError(f"{name} expects {allowed} argument(s), got {len(args)}")

    def builtin(self, name, args):
        arg = first(args)
        if name.startswith("symas."):
            kind = name.split(".")[1]
            if kind == "emit":
                self.arity(name, args, 0)
                return (self.emit_marker,)
            self.arity(name, args, 1)
            self.table(arg)
            if len(self.declarations) >= MAX_ITEMS:
                raise SpecError("declaration count limit exceeded")
            self.declarations.append((kind, arg, self.location))
            return ()
        if name == "require":
            self.arity(name, args, 1)
            if arg != "backend":
                raise SpecError("require only permits the built-in 'backend' declaration collector")
            return (Table((("new", Builtin("backend.new")),)),)
        if name == "backend.new":
            self.arity(name, args, 1)
            if self.arch is not None:
                raise SpecError("only one backend instance is permitted")
            if not re.fullmatch(r"[a-z][a-z0-9_]{0,62}", check_string(arg)):
                raise SpecError("target architecture must be a lowercase C identifier (at most 63 characters)")
            self.arch = arg
            return (Table((k, Builtin(f"symas.{k}")) for k in (*KINDS, "emit")),)
        if name == "print":
            self.arity(name, args, 1)
            if arg is not self.emit_marker:
                raise SpecError("print only accepts the result of symas.emit()")
            return ()
        if name == "assert":
            self.arity(name, args, 1, 2)
            if not truth(arg):
                raise SpecError(check_string(args[1]) if len(args) == 2 else "assertion failed")
            return args
        if name == "loadfile":
            self.arity(name, args, 1)
            if self.path.name not in PROFILES or arg != "riscv64.dsymspec" or self.loaded:
                raise SpecError("loadfile is permitted only for the riscv32 and rv64im profiles")
            return (Builtin("riscv.profile"),)
        if name == "riscv.profile":
            self.arity(name, args, 1)
            if self.loaded or arg != PROFILES[self.path.name][0]:
                raise SpecError("invalid RISC-V profile")
            self.loaded = True
            self.profile = arg
            old_path = self.path
            self.path = old_path.with_name("riscv64.dsymspec")
            if self.path.is_symlink() or not self.path.is_file():
                raise SpecError("profile requires a regular, non-symlink riscv64.dsymspec sibling")
            root = self.parse(self.path)
            self.block(root.children[0], self.globals())
            self.path = old_path
            return ()
        if name == "ipairs":
            self.arity(name, args, 1)
            return (Iteration(tuple(enumerate(self.table(arg).array(), 1))),)
        if name == "pairs":
            self.arity(name, args, 1)
            fields = self.table(arg).fields
            keys = sorted(fields, key=lambda k: (type(k) is str, k))
            return (Iteration(tuple((k, fields[k]) for k in keys)),)
        if name == "tonumber":
            self.arity(name, args, 1)
            if type(arg) in (int, float):
                return (arg,)
            if not isinstance(arg, str):
                return (None,)
            value = arg.strip()
            if len(value) <= 21 and re.fullmatch(r"[+-]?(?:[0-9]+|0[xX][0-9a-fA-F]+)", value):
                return (int64(int(value, 16 if "x" in value.lower() else 10)),)
            return (None,)
        if name == "tostring":
            self.arity(name, args, 1)
            return (("nil" if arg is None else str(arg).lower() if type(arg) is bool
                     else lua_text(arg)),)
        if name == "type":
            self.arity(name, args, 1)
            types = {type(None): "nil", bool: "boolean", str: "string",
                     int: "number", float: "number", Table: "table",
                     Closure: "function", Builtin: "function", Iteration: "function"}
            return (types.get(type(arg), "userdata"),)
        if name == "math.min":
            self.arity(name, args, 1, MAX_ITEMS)
            return (min(number(x) for x in args),)
        if name == "table.insert":
            self.arity(name, args, 2)
            table = self.table(arg)
            table.set(table.length() + 1, args[1])
            return ()
        if name == "table.sort":
            self.arity(name, args, 1)
            table = self.table(arg)
            values = table.array()
            if not (all(type(v) is str for v in values) or
                    all(type(v) in (int, float) for v in values)):
                raise SpecError("table.sort requires a list of strings or numbers")
            for i, value in enumerate(sorted(values), 1):
                table.set(i, value)
            return ()
        if name == "table.unpack":
            self.arity(name, args, 1, 3)
            table = self.table(arg)
            start = integer(args[1]) if len(args) > 1 else 1
            last = integer(args[2]) if len(args) > 2 else table.length()
            if last - start + 1 > MAX_ITEMS:
                raise SpecError("table.unpack result exceeds size limit")
            return tuple(table.get(i) for i in range(start, last + 1))
        if name == "table.concat":
            self.arity(name, args, 1, 2)
            values, separator = self.table(arg).array(), check_string(args[1]) if len(args) == 2 else ""
            parts = [lua_text(x) for x in values]
            if sum(map(len, parts)) + max(0, len(parts) - 1) * len(separator) > MAX_STRING:
                raise SpecError("string size limit exceeded")
            return (separator.join(parts),)
        if name == "string.sub":
            self.arity(name, args, 2, 3)
            value, start = check_string(arg), integer(args[1])
            stop = integer(args[2]) if len(args) == 3 else len(value)
            start = start if start >= 0 else len(value) + start + 1
            stop = stop if stop >= 0 else len(value) + stop + 1
            return (value[max(1, start) - 1:max(0, stop)],)
        if name == "string.find":
            self.arity(name, args, 2, 4)
            value, needle = check_string(arg), check_string(args[1])
            start = integer(args[2]) if len(args) >= 3 else 1
            start = start if start >= 0 else len(value) + start + 1
            start = max(0, start - 1)
            if len(args) != 4 or not truth(args[3]):
                if re.fullmatch(r"\^[A-Za-z_0-9]+", needle):
                    needle = needle[1:]
                    pos = start if value.startswith(needle, start) else -1
                elif any(c in "^$()%.[]*+-?" for c in needle):
                    raise SpecError("string.find supports only literal and anchored literal searches")
                else:
                    pos = value.find(needle, start)
            else:
                pos = value.find(needle, start)
            return (pos + 1, pos + len(needle)) if pos >= 0 else (None,)
        if name == "string.rep":
            self.arity(name, args, 2)
            value, count = check_string(arg), max(0, integer(args[1]))
            if count > MAX_STRING or len(value) * count > MAX_STRING:
                raise SpecError("string size limit exceeded")
            return (value * count,)
        if name == "string.format":
            return (self.format_string(check_string(arg), args[1:]),)
        if name in ("string.gsub", "string.gmatch", "string.match"):
            self.arity(name, args, 3 if name.endswith("gsub") else 2)
            value, pattern = check_string(arg), check_string(args[1])
            pattern = STRING_PATTERNS.get(pattern)
            if pattern is None:
                raise SpecError(f"unsupported Lua string pattern {args[1]!r}")
            if name.endswith("gmatch"):
                return (Iteration(tuple((m[0],) for m in re.finditer(pattern, value))),)
            if name.endswith("match"):
                match = re.search(pattern, value)
                return (match[0] if match else None,)
            return self.replace_string(pattern, value, args[2])
        raise SpecError(f"unsupported builtin {name!r}")

    @staticmethod
    def table(value):
        if not isinstance(value, Table):
            raise SpecError("expected a table")
        return value

    @staticmethod
    def format_string(fmt, args):
        pieces = []
        index = used = length = 0
        while index < len(fmt):
            start = fmt.find("%", index)
            if start < 0:
                pieces.append(fmt[index:])
                break
            pieces.append(fmt[index:start])
            match = FORMAT.match(fmt, start)
            if not match:
                raise SpecError("unsupported string.format conversion")
            spec = match[0]
            if spec == "%%":
                converted = "%"
            else:
                if used >= len(args):
                    raise SpecError("missing string.format argument")
                value = lua_text(args[used]) if spec[-1] == "s" else integer(args[used])
                if spec[-1] in ("x", "X"):
                    value &= INTEGER_MASK
                converted = spec % value
                used += 1
            length += len(converted) + start - index
            if length > MAX_STRING:
                raise SpecError("string size limit exceeded")
            index = match.end()
            pieces.append(converted)
        return check_string("".join(pieces))

    def replace_string(self, pattern, value, replacement):
        pieces = []
        count = end = length = 0
        for match in re.finditer(pattern, value):
            self.step()
            part = value[end:match.start()]
            if isinstance(replacement, (Closure, Builtin)):
                new = first(self.call(replacement, (match[0],)))
                new = lua_text(new) if truth(new) else match[0]
            else:
                new = check_string(replacement)
                if "%" in new:
                    raise SpecError("capture substitutions are outside the DSymspec DSL")
            length += len(part) + len(new)
            if length > MAX_STRING:
                raise SpecError("string size limit exceeded")
            pieces.extend((part, new))
            end, count = match.end(), count + 1
        pieces.append(value[end:])
        return check_string("".join(pieces)), count


def load_parser(path):
    spec = importlib.util.spec_from_file_location("_sandstone_dsymspec_parser", path)
    if spec is None or spec.loader is None:
        raise SpecError(f"could not load LLSynth-generated parser {path}")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def write_header(path, output):
    # A successful compilation replaces the destination without following a
    # preexisting output symlink; interrupted writes never leave a partial .i.
    temporary = None
    try:
        with tempfile.NamedTemporaryFile("w", dir=path.parent, prefix=".dsymspec-",
                                         encoding="utf-8", newline="\n", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(output)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main(argv=None):
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--parser", "--frontend", required=True, type=Path,
                     help="trusted LLSynth-generated Python parser")
    cli.add_argument("input", type=Path)
    cli.add_argument("-o", "--output", type=Path,
                     help="output path (default: <profile>.i in the current directory)")
    args = cli.parse_args(argv)
    evaluator = None
    try:
        if args.output is not None and args.output.suffix != ".i":
            raise SpecError("output must have .i extension")
        evaluator = Evaluator(load_parser(args.parser), args.input)
        name = evaluator.run()
        output_path = args.output if args.output is not None else Path(f"{name}.i")
        dependencies = [args.input, args.parser]
        if args.input.name in PROFILES:
            dependencies.append(args.input.with_name("riscv64.dsymspec"))
        for dependency in dependencies:
            if dependency.resolve() == output_path.resolve() or (
                    output_path.exists() and dependency.exists() and output_path.samefile(dependency)):
                raise SpecError("output must not overwrite an input or the parser")
        output = emit_header(name, evaluator.declarations, Table)
        write_header(output_path, output)
    except DeclarationError as exc:
        print(f"dsymspec: {exc}", file=sys.stderr)
        return 1
    except (OSError, ValueError, ArithmeticError, RecursionError) as exc:
        message = str(exc)
        if not message.startswith(f"{args.input}:"):
            site = evaluator.location if evaluator and evaluator.location else args.input
            message = f"{site}: {message}"
        print(f"dsymspec: {message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
