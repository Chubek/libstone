"""Lower evaluated DSymspec declarations and Sandstone encoder recipes to C11."""

from dataclasses import dataclass, field
import re


LIST_FIELDS = {"features", "operands", "flags", "aliases", "required", "forbidden", "syntaxes"}
TOKEN = re.compile(
    r'(?P<skip>[ \r\n\t]+|//[^\n]*)'
    r'|(?P<number>0[xX][0-9A-Fa-f]+|[0-9]+)'
    r'|(?P<name>[A-Za-z_][A-Za-z_0-9]*)'
    r'|(?P<string>"(?:[^"\\]|\\[\s\S])*")'
    r'|(?P<punct>==|!=|<=|>=|&&|\|\||<<|>>|[(){};,=<>+*/%!\-&|])'
)
FIXED_EMIT = {"emit8": 1, "emit16le": 2, "emit16be": -2, "emit32le": 4,
              "emit32be": -4, "emit64le": 8, "emit64be": -8}
BINARY = {"||": (1, "OR"), "&&": (2, "AND"),
          "==": (3, "EQ"), "!=": (3, "NE"),
          "<": (4, "LT"), "<=": (4, "LE"), ">": (4, "GT"), ">=": (4, "GE"),
          "+": (5, "ADD"), "-": (5, "SUB"), "*": (6, "MUL"),
          "/": (6, "DIV"), "%": (6, "MOD")}
EXPR_CALLS = {"select": (3, "SELECT"), "regnum": (1, "LOW8"), "value_of": (1, "LOW8")}
OP_CALLS = {"emit_imm": (2, "EMIT_SIZED"), "emit_disp": (2, "EMIT_SIZED"),
            "emit_modrm": (2, "MODRM"), "emit_sib": (3, "SIB"),
            "require": (1, "REQUIRE")}
WORD = re.compile(r"[a-z][a-z0-9_]{0,62}\Z")


def c_string(value):
    body = []
    for byte in value.encode("latin-1"):
        if byte in (ord('"'), ord('\\')):
            body.append("\\" + chr(byte))
        elif 32 <= byte < 127 and byte != ord('?'):
            body.append(chr(byte))
        else:
            body.append(f"\\{byte:03o}")
    return '"' + "".join(body) + '"'


def c_integer(value):
    if value == -(1 << 63):
        return "INT64_MIN"
    if value < 0:
        return f"-INT64_C({-value})"
    return f"INT64_C({value})"


def c_nullable(value):
    return "NULL" if value is None else c_string(value)


def decode_encoder_string(s):
    def replace(match):
        ch = match[0][1]
        return {"n": "\n", "t": "\t"}.get(ch, ch)
    return re.sub(r"\\[\s\S]", replace, s[1:-1])


class UnsupportedEncoder(ValueError):
    pass


class DeclarationError(ValueError):
    """A diagnostic with a declaration's source location."""


class EncoderSyntax:
    def __init__(self, source):
        self.tokens = []
        pos = 0
        while pos < len(source):
            token = TOKEN.match(source, pos)
            if not token:
                raise ValueError(f"invalid encoder token at offset {pos}: {source[pos:pos + 12]!r}")
            if token.lastgroup != "skip":
                self.tokens.append((token.lastgroup, token[0], pos))
            pos = token.end()
        self.tokens.append(("eof", "", pos))
        self.index = 0

    def token(self):
        return self.tokens[self.index]

    def peek(self):
        return self.token()[1]

    def fail(self, message):
        raise ValueError(f"{message} at encoder offset {self.token()[2]}")

    def take(self):
        token = self.token()
        if token[0] == "eof":
            self.fail("unexpected end of encoder")
        self.index += 1
        return token

    def eat(self, op):
        if self.peek() != op:
            return False
        self.take()
        return True

    def expect(self, op):
        if not self.eat(op):
            self.fail(f"expected {op!r}, found {self.peek()!r}")

    def number(self, spelling):
        hex_literal = spelling[:2].lower() == "0x"
        digits = (spelling[2:] if hex_literal else spelling).lstrip("0") or "0"
        if len(digits) > (16 if hex_literal else 19):
            self.fail("encoder integer outside signed/bit-pattern 64-bit range")
        value = int(digits, 16 if hex_literal else 10)
        if not hex_literal and value >= 1 << 63:
            self.fail("encoder decimal integer is out of range")
        return value if value < 1 << 63 else value - (1 << 64)

    def primary(self):
        kind, text, offset = self.take()
        if kind == "number":
            return ("INT", self.number(text))
        if kind == "string":
            string = decode_encoder_string(text)
            if "\x00" in string:
                self.fail("NUL is not allowed in encoder strings")
            return ("STRING", string)
        if kind == "name":
            if self.eat("("):
                return ("CALL", text, self.args())
            return ("BINDING", text)
        if text == "(":
            node = self.expr()
            self.expect(")")
            return node
        if text == "-":
            # Allow the most-negative immediate as one representable value.
            kind, spelling, _ = self.token()
            if kind == "number" and spelling.lstrip("0") == "9223372036854775808":
                self.take()
                return ("INT", -(1 << 63))
            return ("NEG", self.primary())
        if text == "!":
            return ("NOT", self.primary())
        raise ValueError(f"expected an expression at encoder offset {offset}, found {text!r}")

    def expr(self, minimum=1):
        expr = self.primary()
        while self.peek() in BINARY and BINARY[self.peek()][0] >= minimum:
            _, token, _ = self.take()
            precedence, kind = BINARY[token]
            expr = (kind, expr, self.expr(precedence + 1))
        return expr

    def args(self):
        result = []
        if not self.eat(")"):
            while True:
                result.append(self.expr())
                if self.eat(")"):
                    break
                self.expect(",")
        return result

    def statement(self):
        if self.eat("{"):
            result = []
            while not self.eat("}"):
                result.append(self.statement())
            return ("BLOCK", result)
        kind, name, offset = self.take()
        if kind != "name":
            raise ValueError(f"expected encoder statement at offset {offset}, found {name!r}")
        self.expect("(")
        if name == "if":
            condition = self.expr()
            self.expect(")")
            yes = self.statement()
            no = self.statement() if self.eat("else") else None
            return ("IF", condition, yes, no)
        arguments = self.args()
        self.expect(";")
        return ("CALL", name, arguments)

    def run(self):
        result = []
        while self.token()[0] != "eof":
            result.append(self.statement())
        return result


@dataclass
class Recipe:
    code: list = field(default_factory=list)
    exprs: list = field(default_factory=list)
    bindings: dict = field(default_factory=dict)
    unsupported_reason: str = None
    expr_indices: dict = field(default_factory=dict)

    @staticmethod
    def check_arity(name, args, *arities):
        if len(args) not in arities:
            raise ValueError(f"{name} expects {' or '.join(map(str, arities))} arguments, got {len(args)}")

    def add_expr(self, expr):
        kind, *args = expr
        number = 0
        text = None
        if kind == "INT":
            number, args = args[0], []
        elif kind in ("BINDING", "STRING"):
            text, args = args[0], []
            if kind == "BINDING":
                self.bindings[text] = None
        elif kind == "CALL":
            name, args = args
            if name not in EXPR_CALLS:
                raise UnsupportedEncoder(f"unsupported encoder expression builtin {name!r}")
            arity, kind = EXPR_CALLS[name]
            self.check_arity(name, args, arity)
        children = tuple(self.add_expr(arg) for arg in args)
        entry = kind, children, number, text
        index = self.expr_indices.get(entry)
        if index is None:
            index = len(self.exprs)
            self.exprs.append(entry)
            self.expr_indices[entry] = index
        return index

    def add_op(self, kind, args=(), jump=0, detail=0, text=None):
        children = [self.add_expr(arg) if arg is not None else None for arg in args]
        index = len(self.code)
        self.code.append([kind, children, jump, detail, text])
        return index

    def compile_statement(self, statement):
        kind, *args = statement
        if kind == "BLOCK":
            for stmt in args[0]:
                self.compile_statement(stmt)
            return
        if kind == "IF":
            condition, yes, no = args
            branch = self.add_op("IF_FALSE", (condition,))
            self.compile_statement(yes)
            if no is None:
                self.code[branch][2] = len(self.code)
            else:
                jump = self.add_op("JUMP")
                self.code[branch][2] = len(self.code)
                self.compile_statement(no)
                self.code[jump][2] = len(self.code)
            return
        name, args = args
        if name in FIXED_EMIT:
            self.check_arity(name, args, 1)
            self.add_op("EMIT", args, detail=FIXED_EMIT[name])
        elif name in OP_CALLS:
            arity, op = OP_CALLS[name]
            self.check_arity(name, args, arity)
            self.add_op(op, args)
        elif name in ("emit_reloc", "emit_relocation"):
            self.check_arity(name, args, 2, 3)
            symbol, type_expr = args[:2]
            addend = args[2] if len(args) == 3 else None
            type_kind = type_expr[0]
            if type_kind in ("BINDING", "STRING"):
                self.add_op("RELOC_NAME", (symbol, None, addend), text=type_expr[1])
            elif type_kind == "INT":
                if not 0 <= type_expr[1] <= (1 << 32) - 1:
                    raise ValueError("relocation type outside uint32 range")
                self.add_op("RELOC_ID", (symbol, None, addend), detail=type_expr[1])
            else:
                self.add_op("RELOC_DYNAMIC", (symbol, type_expr, addend))
        elif name == "fail":
            self.check_arity(name, args, 0, 1)
            self.add_op("FAIL", text=args[0][1] if args and args[0][0] == "STRING" else None)
        else:
            raise UnsupportedEncoder(f"unsupported encoder statement builtin {name!r}")

    @classmethod
    def compile(cls, source):
        ast = EncoderSyntax(source).run()
        if not ast:
            return cls(unsupported_reason="empty encoder program")
        result = cls()
        try:
            for statement in ast:
                result.compile_statement(statement)
        except UnsupportedEncoder as exc:
            return cls(unsupported_reason=str(exc))
        if not result.code:
            return cls(unsupported_reason="encoder has no executable statements")
        return result


class Header:
    def __init__(self, target, table_type):
        self.target = target
        self.prefix = f"dsymspec_{target}"
        self.macro = f"DSYMSPEC_{target.upper()}"
        self.lines = []
        self.size = 0
        self.value_id = 0
        self.field_count = 0
        self.field_arrays = {}
        self.table_type = table_type

    def add(self, line=""):
        self.size += len(line) + 1
        if self.size > 64 * 1024 * 1024:
            raise ValueError("generated header exceeds size limit")
        self.lines.append(line)

    def c_value(self, value, field_name=None, seen=None, record=False):
        if seen is None:
            seen = set()
        if record and (not isinstance(value, self.table_type) or
                       any(type(k) is not str for k in value.fields)):
            raise ValueError("declaration requires a record with named fields")
        if type(value) is bool:
            payload = f"BOOL, {int(value)}, 0.0, NULL, 0, NULL"
        elif type(value) is int:
            payload = f"INTEGER, {c_integer(value)}, 0.0, NULL, 0, NULL"
        elif type(value) is float:
            # C11 has hexadecimal floating constants; C++11 does not.
            payload = f"REAL, 0, {value!r}, NULL, 0, NULL"
        elif type(value) is str:
            payload = f"STRING, 0, 0.0, {c_string(value)}, {len(value)}, NULL"
        elif isinstance(value, self.table_type):
            keys = value.fields.keys()
            if id(value) in seen:
                raise ValueError("cyclic declaration table")
            seen.add(id(value))
            ordered = sorted(keys, key=lambda x: (type(x) is str, x))
            if not keys:
                kind = "LIST" if field_name in LIST_FIELDS else "RECORD"
            elif all(type(k) is str for k in keys):
                kind = "RECORD"
            elif ordered == list(range(1, len(keys) + 1)):
                kind = "LIST"
            else:
                kind = "TABLE"
            if not keys:
                payload = f"{kind}, 0, 0.0, NULL, 0, NULL"
            else:
                self.field_count += len(keys)
                if self.field_count > 200000:
                    raise ValueError("generated header exceeds field count limit")
                children = []
                for k in ordered:
                    if type(k) is str:
                        if "\x00" in k:
                            raise ValueError("NUL is not allowed in field names")
                        key = c_string(k)
                    else:
                        key = "NULL"
                    child = self.c_value(value.fields[k], k, seen)
                    children.append(f"    {{ {key}, {0 if type(k) is str else c_integer(k)}, {child} }},")
                key = tuple(children)
                name = self.field_arrays.get(key)
                if name is None:
                    self.value_id += 1
                    name = f"{self.prefix}_fields_{self.value_id}"
                    self.field_arrays[key] = name
                    self.add(f"static const dsymspec_field {name}[] = {{")
                    for child in children:
                        self.add(child)
                    self.add("};")
                payload = f"{kind}, 0, 0.0, NULL, {len(children)}, {name}"
            seen.remove(id(value))
        else:
            raise ValueError("declaration contains a runtime value instead of data")
        return "{ DSYMSPEC_VALUE_" + payload + " }"

    @staticmethod
    def indices(args):
        padded = [*args, *([None] * (3 - len(args)))]
        return "{ " + ", ".join("UINT32_MAX" if i is None else str(i) for i in padded) + " }"

    def encoder(self, index, record):
        raw = record.get("source")
        if raw is not None and type(raw) is not str:
            raise ValueError("encoder source must be a string")
        max_bytes, max_relocs = record.get("max_bytes"), record.get("max_relocs")
        max_bytes = 64 if max_bytes is None else max_bytes
        max_relocs = 8 if max_relocs is None else max_relocs
        for label, value in (("max_bytes", max_bytes), ("max_relocs", max_relocs)):
            if type(value) is not int or not 0 <= value <= (1 << 32) - 1:
                raise ValueError(f"{label} must be a uint32 integer")
        recipe = Recipe.compile(raw or "")
        if recipe.exprs:
            exprs = f"{self.prefix}_exprs_{index}"
            self.add(f"static const dsymspec_expr {exprs}[] = {{")
            for kind, children, number, text in recipe.exprs:
                self.add(f"    {{ DSYMSPEC_EXPR_{kind}, {self.indices(children)}, "
                         f"{c_integer(number)}, {c_nullable(text)} }},")
            self.add("};")
        else:
            exprs = "NULL"
        if recipe.code:
            ops = f"{self.prefix}_ops_{index}"
            self.add(f"static const dsymspec_op {ops}[] = {{")
            for kind, children, jump, detail, text in recipe.code:
                self.add(f"    {{ DSYMSPEC_OP_{kind}, {self.indices(children)}, {jump}, "
                         f"{c_integer(detail)}, {c_nullable(text)} }},")
            self.add("};")
        else:
            ops = "NULL"
        if recipe.bindings:
            bindings = f"{self.prefix}_bindings_{index}"
            self.add(f"static const char *const {bindings}[] = {{")
            for name in recipe.bindings:
                self.add(f"    {c_string(name)},")
            self.add("};")
        else:
            bindings = "NULL"
        name = f"{self.prefix}_encoder_{index}"
        self.add(f"static const dsymspec_encoder {name} = {{")
        self.add(f"    {len(recipe.exprs)}, {exprs}, {len(recipe.code)}, {ops},")
        self.add(f"    {len(recipe.bindings)}, {bindings}, {max_bytes}u, {max_relocs}u, "
                 f"{c_nullable(recipe.unsupported_reason)}")
        self.add("};")
        return f"&{name}"

    def generate(self, declarations):
        if not WORD.fullmatch(self.target):
            raise ValueError("generated target is not a supported C identifier")
        guard = f"SANDSTONE_{self.macro}_I"
        self.add("/* Generated by DSymspec (LLSynth). Do not edit. */")
        self.add(f"#ifndef {guard}")
        self.add(f"#define {guard}")
        self.add('#include "DSymspec.h"')
        self.add()
        descs, encoders, names = [], {}, {}
        for index, (kind, record, site) in enumerate(declarations):
            try:
                desc = self.declaration(index, kind, record, encoders)
                if kind not in ("form", "debug"):
                    current = names.setdefault(kind, set())
                    if desc[1] in current:
                        raise ValueError(f"duplicate {kind} name {desc[1]!r}")
                    current.add(desc[1])
                descs.append(desc)
            except (ValueError, RecursionError) as exc:
                raise DeclarationError(f"{site}: {kind} declaration: {exc}") from None
        self.add()
        self.add(f"#define {self.macro}_DECLARATION_COUNT {len(descs)}")
        self.add(f"#define {self.macro}_DECLARATIONS(X) \\")
        for index, (kind, name, record, _) in enumerate(descs):
            tail = " \\" if index + 1 < len(descs) else ""
            c_name = c_string(name)
            self.add(f"    X({kind}, {index}, {c_name}, {record}){tail}")
        self.add()
        self.add(f"static const dsymspec_decl {self.prefix}_declarations[] = {{")
        for (kind, name, record, encoder), (domain, source, site) in zip(descs, declarations):
            try:
                self.references(domain, source, name, names)
                if domain == "form" and encoder is not None:
                    if encoder not in encoders:
                        raise ValueError(f"references unknown encoder {encoder!r}")
                    encoder = encoders[encoder]
            except ValueError as exc:
                raise DeclarationError(f"{site}: {domain} {name!r}: {exc}") from None
            self.add(f"    {{ {kind}, {c_string(name)}, {record}, {encoder or 'NULL'} }},")
        self.add("};")
        self.add(f"static const dsymspec_target {self.prefix} = {{")
        self.add(f"    {c_string(self.target)}, {self.macro}_DECLARATION_COUNT, {self.prefix}_declarations")
        self.add("};")
        self.add()
        self.add(f"#endif /* {guard} */")
        self.add()
        return "\n".join(self.lines)

    def list_field(self, record, key):
        table = record.get(key)
        if table is None:
            return ()
        if not isinstance(table, self.table_type) or len(table.fields) != table.length():
            raise ValueError(f"{key} must be a contiguous list")
        return table.array()

    def references(self, kind, record, name, names):
        if kind == "form":
            if name not in names.get("mnemonic", ()):
                raise ValueError(f"references unknown mnemonic {name!r}")
            checks = (("required", "feature"), ("forbidden", "feature"), ("syntaxes", "syntax"))
            for operand in self.list_field(record, "operands"):
                if not isinstance(operand, self.table_type) or not isinstance(operand.get("kind"), str):
                    raise ValueError("each operand must be a record with a string kind")
        elif kind == "target":
            checks = (("features", "feature"),)
        else:
            return
        for key, category in checks:
            for item in self.list_field(record, key):
                if not isinstance(item, str) or item not in names.get(category, ()):
                    raise ValueError(f"{key} references an unknown {category}: {item!r}")

    def declaration(self, index, kind, record, encoders):
        value = self.c_value(record, record=True)
        record_id = f"{self.prefix}_record_{index}"
        self.add(f"static const dsymspec_value {record_id} = {value};")
        name_key = {"target": "arch", "form": "mnemonic", "debug": "msg"}.get(kind, "name")
        name = record.get(name_key)
        if type(name) is not str or "\x00" in name or not name:
            raise ValueError(f"{name_key} must be a nonempty string without NUL")
        encoder = None
        if kind == "encoder":
            encoder = encoders[name] = self.encoder(index, record)
        elif kind == "form":
            encoder = record.get("encoder")
            if encoder is not None and (type(encoder) is not str or "\x00" in encoder or not encoder):
                raise ValueError("encoder reference must be a nonempty string without NUL")
        return f"DSYMSPEC_DECL_{kind.upper()}", name, f"&{record_id}", encoder


def emit_header(name, declarations, table_type):
    return Header(name, table_type).generate(declarations)
