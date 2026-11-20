"""ArchSem's typed, straight-line architectural-state and bit-vector subset.

Statements run in source order. A failing instruction is transactional, and
register, scalar-state, and memory writes inside ``parallel`` blocks are
evaluated against the block-entry state before being committed together.
SemIR contains only JSON values.
"""

from bisect import bisect_right
from dataclasses import dataclass
import re

from .diagnostics import ArchonError, Location


_TOKEN = re.compile(
    r"(?P<trivia>\s+|(?://|#)[^\r\n]*|/\*[\s\S]*?\*/)"
    r"|(?P<name>[A-Za-z_][A-Za-z_0-9]*)"
    r"|(?P<number>[0-9][A-Za-z_0-9]*)"
    r"|(?P<symbol>:=|\+%|-%|\*%|<<|>>|[^\n])"
)
_INTEGER = re.compile(r"[0-9]+|0[xX][0-9a-fA-F]+|0[bB][01]+")
_NEWLINE = re.compile(r"\r\n?|\n")
_KINDS = {"bits", "uint", "sint"}
_MAX_WIDTH = 4096
_MAX_EXPR_DEPTH = 128
_MAX_BLOCK_DEPTH = 64
_MAX_SEMIR_NODES = 65536
_MAX_STACK_CAPACITY = 65536
_MAX_MEMORY_BYTES = 16 * 1024 * 1024
_PRECEDENCE = {"|": 1, "^": 2, "&": 3, "<<": 4, ">>": 4, "+%": 5, "-%": 5, "*%": 6}
_CONVERSIONS = {"zext", "sext", "trunc"}


@dataclass(frozen=True)
class _Type:
    kind: str
    width: int

    def __str__(self) -> str:
        return f"{self.kind}<{self.width}>"

    def ir(self) -> dict:
        return {"kind": self.kind, "width": self.width}


def _type_of(expr: dict) -> _Type:
    return _Type(**expr["type"])


def _compatible_types(actual: _Type, expected: _Type) -> bool:
    """Treat ``bits`` as a neutral bit pattern, but never mix signed and unsigned."""
    return (
        actual.width == expected.width
        and (actual.kind == expected.kind or "bits" in {actual.kind, expected.kind})
    )


class _Compiler:
    def __init__(
        self, source, location, operands, word_width, types, parameters,
        state, address_width, endian,
    ):
        self.source = source
        self.location = location
        self.line_starts = [0, *(match.end() for match in _NEWLINE.finditer(source))]
        self.cursor = 0
        self.token = ""
        self.token_kind = ""
        self.token_start = 0
        self.statement_location = location
        self.operands = {operand["name"]: operand for operand in operands}
        self.word = self.make_type("bits", word_width)
        self.address = self.make_type("bits", address_width)
        self.types = types
        self.parameters = parameters
        state = state or {"scalars": [], "stacks": [], "memories": []}
        self.scalars = {row["name"]: row for row in state["scalars"]}
        self.stacks = {row["name"]: row for row in state["stacks"]}
        self.memories = {row["name"]: row for row in state["memories"]}
        self.resources = self.scalars.keys() | self.stacks.keys() | self.memories.keys()
        self.extended_state = bool(self.resources)
        self.endian = endian
        self.locals: dict[str, _Type] = {}
        self.reads: dict[str, None] = {}
        self.writes: dict[str, None] = {}
        self.state_reads: dict[str, None] = {}
        self.state_writes: dict[str, None] = {}
        self.stack_reads: dict[str, None] = {}
        self.stack_writes: dict[str, None] = {}
        self.memory_reads: dict[str, None] = {}
        self.memory_writes: dict[str, None] = {}
        self.nodes = 0
        self.atom_depth = 0
        self.in_parallel = False
        self.advance()

    def advance(self) -> None:
        while self.cursor < len(self.source):
            self.token_start = self.cursor
            token = _TOKEN.match(self.source, self.cursor)
            self.cursor = token.end()
            if token.lastgroup != "trivia":
                self.token = token[0]
                self.token_kind = token.lastgroup
                return
        self.token = self.token_kind = ""
        self.token_start = self.cursor

    def source_location(self) -> Location:
        row = bisect_right(self.line_starts, self.token_start) - 1
        column = self.token_start - self.line_starts[row]
        column += self.location.column if row == 0 else 1
        return Location(self.location.filename, self.location.line + row, column)

    def error(self, message: str) -> ArchonError:
        return ArchonError(message, self.statement_location)

    def accept(self, text: str) -> bool:
        if self.token != text:
            return False
        self.advance()
        return True

    def expect(self, text: str) -> None:
        # ``load<bits<32>>`` has two adjacent generic closers, while ``>>`` is
        # otherwise a shift token. Consume one closer without losing the other.
        if text == ">" and self.token == ">>":
            self.token = ">"
            self.token_start += 1
            return
        if not self.accept(text):
            found = repr(self.token) if self.token else "end of semantics"
            raise self.error(f"expected {text!r}, found {found}")

    def identifier(self) -> str:
        if self.token_kind != "name":
            raise self.error("expected an identifier")
        name = self.token
        self.advance()
        return name

    def number(self) -> int:
        token = self.token
        self.advance()
        if not _INTEGER.fullmatch(token):
            raise self.error(f"invalid integer literal {token!r}")
        try:
            return int(token, 0 if token.lower().startswith(("0x", "0b")) else 10)
        except ValueError:
            raise self.error(f"invalid integer literal {token!r}") from None

    def make_type(self, kind: str, width: int) -> _Type:
        if kind not in _KINDS:
            raise self.error(f"invalid semantic type {kind!r}")
        if type(width) is not int or not 1 <= width <= _MAX_WIDTH:
            raise self.error(f"invalid semantic type: {kind} width must be in 1..{_MAX_WIDTH}")
        return _Type(kind, width)

    def width_argument(self) -> int:
        self.expect("<")
        if self.token_kind == "number":
            width = self.number()
        else:
            name = self.identifier()
            if name not in self.parameters:
                raise self.error(f"unknown width parameter {name!r}")
            width = self.parameters[name]
        self.expect(">")
        return width

    def annotation(self) -> _Type:
        name = self.identifier()
        if name in _KINDS:
            return self.make_type(name, self.width_argument())
        if name not in self.types:
            raise self.error(f"unknown type {name!r}")
        alias = self.types[name]
        return self.make_type(alias["kind"], alias["width"])

    def type_argument(self) -> _Type:
        self.expect("<")
        ty = self.annotation()
        self.expect(">")
        return ty

    def declared_type(self, row: dict, key: str) -> _Type:
        try:
            return self.make_type(row[key]["kind"], row[key]["width"])
        except (KeyError, TypeError):
            raise self.error("malformed architectural state declaration") from None

    def named_resource(self, resources: dict, name: str, kind: str) -> dict:
        if name not in resources:
            raise self.error(f"unknown {kind} {name!r}")
        return resources[name]

    def address_expression(self) -> dict:
        address = self.expression()
        ty = _type_of(address)
        if ty.width != self.address.width or ty.kind == "sint":
            raise self.error(
                f"memory address must have type bits<{self.address.width}> "
                f"or uint<{self.address.width}>, got {ty}"
            )
        return address

    def memory_value_type(self) -> _Type:
        ty = self.type_argument()
        if ty.width % 8:
            raise self.error("memory load/store type width must be a multiple of eight bits")
        return ty

    def register(self, name: str, action: str, directions: tuple[str, str]) -> None:
        operand = self.operands.get(name)
        if operand is None:
            raise self.error(f"unknown operand {name!r}")
        if operand["kind"] != "reg":
            raise self.error(f"{action} requires a register operand, not {name!r}")
        if operand["direction"] not in directions:
            raise self.error(f"cannot {action} {operand['direction']} operand {name!r}")

    def count_node(self) -> None:
        self.nodes += 1
        if self.nodes > _MAX_SEMIR_NODES:
            raise self.error(f"semantic program exceeds {_MAX_SEMIR_NODES} nodes")

    def expr(self, op: str, ty: _Type, **fields) -> dict:
        self.count_node()
        return {"op": op, "type": ty.ir(), **fields}

    def atom(self) -> dict:
        self.atom_depth += 1
        if self.atom_depth > _MAX_EXPR_DEPTH:
            raise self.error("semantic expression is nested too deeply")
        try:
            return self.parse_atom()
        finally:
            self.atom_depth -= 1

    def parse_atom(self) -> dict:
        if self.accept("("):
            value = self.expression()
            self.expect(")")
            return value
        if self.accept("~"):
            value = self.atom()
            return self.expr("~", _type_of(value), value=value)
        if self.token_kind == "number":
            value = self.number()
            if value.bit_length() > self.word.width:
                raise self.error(f"integer literal does not fit in {self.word}")
            return self.expr("constant", self.word, value=value)
        if self.token_kind != "name":
            raise self.error("expected an ArchSem expression")

        name = self.identifier()
        if name == "read" and self.accept("("):
            operand = self.identifier()
            self.expect(")")
            self.register(operand, "read", ("in", "inout"))
            self.reads[operand] = None
            return self.expr("read", self.word, name=operand)
        if name == "read_state" and self.accept("("):
            scalar = self.identifier()
            self.expect(")")
            row = self.named_resource(self.scalars, scalar, "scalar state")
            self.state_reads[scalar] = None
            return self.expr(
                "read_state", self.declared_type(row, "type"), name=scalar,
            )
        if name in {"peek", "pop"} and self.accept("("):
            stack = self.identifier()
            row = self.named_resource(self.stacks, stack, "stack")
            depth = 0
            if self.accept(","):
                if name == "pop":
                    raise self.error("pop does not accept a depth")
                depth = self.number()
            self.expect(")")
            if name == "peek" and depth >= row["capacity"]:
                raise self.error(
                    f"peek depth {depth} is outside stack {stack!r} capacity {row['capacity']}"
                )
            if name == "pop" and self.in_parallel:
                raise self.error("pop is not permitted in a parallel block")
            self.stack_reads[stack] = None
            if name == "pop":
                self.stack_writes[stack] = None
            return self.expr(
                name, self.declared_type(row, "element_type"),
                name=stack, **({"depth": depth} if name == "peek" else {}),
            )
        if name == "load" and self.token == "<":
            ty = self.memory_value_type()
            self.expect("(")
            memory = self.identifier()
            self.named_resource(self.memories, memory, "memory")
            self.expect(",")
            address = self.address_expression()
            self.expect(")")
            self.memory_reads[memory] = None
            return self.expr("load", ty, name=memory, address=address)
        if name in _CONVERSIONS and self.token == "<":
            width = self.width_argument()
            self.expect("(")
            value = self.expression()
            self.expect(")")
            source = _type_of(value)
            # An explicit extension selects the interpretation of the source bits.
            kind = {"zext": "uint", "sext": "sint"}.get(name, source.kind)
            result = self.make_type(kind if source.kind != "bits" else "bits", width)
            if name == "trunc" and width > source.width:
                raise self.error(f"trunc cannot widen {source} to {result}")
            if name != "trunc" and width < source.width:
                raise self.error(f"{name} cannot narrow {source} to {result}")
            return self.expr(name, result, value=value)
        if name == "extract" and self.accept("("):
            value = self.expression()
            self.expect(",")
            high = self.number()
            self.expect(",")
            low = self.number()
            self.expect(")")
            source = _type_of(value)
            if not 0 <= low <= high < source.width:
                raise self.error(f"extract requires 0 <= low <= high < {source.width}")
            return self.expr("extract", _Type("bits", high - low + 1),
                             value=value, high=high, low=low)
        if name == "concat" and self.accept("("):
            high = self.expression()
            self.expect(",")
            low = self.expression()
            self.expect(")")
            result = self.make_type("bits", _type_of(high).width + _type_of(low).width)
            return self.expr("concat", result, left=high, right=low)
        if self.token in ("(", "<"):
            raise self.error(f"unsupported ArchSem operation {name!r}")
        if name in self.locals:
            return self.expr("local", self.locals[name], name=name)
        if name in self.operands:
            operand = self.operands[name]
            if operand["kind"] == "reg":
                raise self.error(f"use read({name}) to read a register operand")
            if operand["direction"] not in ("in", "inout"):
                raise self.error(f"cannot read {operand['direction']} operand {name!r}")
            ty = self.make_type("sint" if operand["signed"] else "uint", operand["width"])
            return self.expr("immediate", ty, name=name)
        raise self.error(f"unbound name {name!r}")

    def expression(self, minimum: int = 1) -> dict:
        left = self.atom()
        while _PRECEDENCE.get(self.token, 0) >= minimum:
            op = self.token
            self.advance()
            right = self.expression(_PRECEDENCE[op] + 1)
            a, b = _type_of(left), _type_of(right)
            if op in ("<<", ">>"):
                if b.kind == "sint":
                    raise self.error("shift count must have type bits or uint")
                result = a
            else:
                if a.width != b.width or {a.kind, b.kind} == {"sint", "uint"}:
                    raise self.error(f"incompatible types for {op}: {a} and {b}")
                result = a if a == b else _Type("bits", a.width)
            left = self.expr(op, result, left=left, right=right)
        return left

    def write_register(self, name: str, *, call=False) -> dict:
        self.register(name, "write", ("out", "inout"))
        if call:
            self.expect(",")
        value = self.expression()
        if _type_of(value).width != self.word.width:
            raise self.error(f"write expects a {self.word.width}-bit value, got {_type_of(value)}")
        if call:
            self.expect(")")
        self.expect(";")
        self.writes[name] = None
        return {"op": "write", "name": name, "value": value}

    def write_state(self, name: str, *, call=False) -> dict:
        row = self.named_resource(self.scalars, name, "scalar state")
        if call:
            self.expect(",")
        value = self.expression()
        expected = self.declared_type(row, "type")
        if not _compatible_types(_type_of(value), expected):
            raise self.error(f"write_state({name}) expects {expected}, got {_type_of(value)}")
        if call:
            self.expect(")")
        self.expect(";")
        self.state_writes[name] = None
        return {"op": "write_state", "name": name, "value": value}

    def push(self) -> dict:
        if self.in_parallel:
            raise self.error("push is not permitted in a parallel block")
        self.expect("(")
        name = self.identifier()
        row = self.named_resource(self.stacks, name, "stack")
        self.expect(",")
        value = self.expression()
        expected = self.declared_type(row, "element_type")
        if not _compatible_types(_type_of(value), expected):
            raise self.error(f"push({name}) expects {expected}, got {_type_of(value)}")
        self.expect(")")
        self.expect(";")
        self.stack_writes[name] = None
        return {"op": "push", "name": name, "value": value}

    def store(self) -> dict:
        ty = self.memory_value_type()
        self.expect("(")
        name = self.identifier()
        self.named_resource(self.memories, name, "memory")
        self.expect(",")
        address = self.address_expression()
        self.expect(",")
        value = self.expression()
        if not _compatible_types(_type_of(value), ty):
            raise self.error(f"store<{ty}> expects a {ty} value, got {_type_of(value)}")
        self.expect(")")
        self.expect(";")
        self.memory_writes[name] = None
        return {"op": "store", "name": name, "address": address, "value": value}

    def statement(self, depth: int, parallel: bool) -> dict:
        self.count_node()
        if self.token_kind != "name":
            raise self.error(f"expected a semantic statement, found {self.token!r}")
        kind = self.identifier()
        if self.accept(":="):
            if kind in self.scalars:
                return self.write_state(kind)
            return self.write_register(kind)
        if kind == "let":
            name = self.identifier()
            if name in self.locals or name in self.operands or name in self.resources:
                raise self.error(f"duplicate binding {name!r}")
            annotation = self.annotation() if self.accept(":") else None
            self.expect("=")
            value = self.expression()
            ty = _type_of(value)
            if annotation is not None and annotation != ty:
                raise self.error(f"binding {name!r} expects {annotation}, got {ty}")
            self.expect(";")
            self.locals[name] = ty
            return {"op": "let", "name": name, "value": value}
        if kind == "write":
            self.expect("(")
            return self.write_register(self.identifier(), call=True)
        if kind == "write_state":
            self.expect("(")
            return self.write_state(self.identifier(), call=True)
        if kind == "push":
            return self.push()
        if kind == "store" and self.token == "<":
            return self.store()
        if kind in {"sequence", "parallel"}:
            if parallel:
                raise self.error(
                    "a parallel block may only contain let and write statements "
                    "(register/scalar writes and stores)"
                )
            if depth >= _MAX_BLOCK_DEPTH:
                raise self.error(f"semantic blocks are nested too deeply (limit {_MAX_BLOCK_DEPTH})")
            self.expect("{")
            return {"op": kind, "statements": self.block(depth + 1, kind == "parallel")}
        raise self.error(f"expected a semantic statement, found {kind!r}")

    def block(self, depth: int, parallel: bool) -> list[dict]:
        statements = []
        bindings = []
        previous_parallel = self.in_parallel
        self.in_parallel = parallel
        try:
            while self.token and not (depth and self.token == "}"):
                self.statement_location = self.source_location()
                statement = self.statement(depth, parallel)
                if statement["op"] == "let":
                    bindings.append(statement["name"])
                for root in (statement.get("value"), statement.get("address")):
                    if not isinstance(root, dict):
                        continue
                    # A left-associative expression can produce a deep IR even
                    # without deep parser recursion.
                    pending = [(root, 1)]
                    while pending:
                        expr, level = pending.pop()
                        if level > _MAX_EXPR_DEPTH:
                            raise self.error("semantic expression is nested too deeply")
                        for key in ("left", "right", "value", "address"):
                            if isinstance(child := expr.get(key), dict):
                                pending.append((child, level + 1))
                statements.append(statement)
            if depth:
                if not self.token:
                    self.statement_location = self.source_location()
                self.expect("}")
            return statements
        finally:
            self.in_parallel = previous_parallel
            for name in bindings:
                del self.locals[name]

    def compile(self) -> dict:
        try:
            statements = self.block(0, False)
        except RecursionError:
            raise self.error("semantic expression is nested too deeply") from None
        effects = {"reads": list(self.reads), "writes": list(self.writes)}
        if self.extended_state:
            effects.update({
                "state_reads": list(self.state_reads),
                "state_writes": list(self.state_writes),
                "stack_reads": list(self.stack_reads),
                "stack_writes": list(self.stack_writes),
                "memory_reads": list(self.memory_reads),
                "memory_writes": list(self.memory_writes),
            })
        return {
            "word_width": self.word.width,
            "statements": statements,
            "effects": effects,
        }


def compile_semantics(
    source: str,
    location: Location,
    operands: list[dict],
    word_width: int,
    types: dict[str, dict],
    *,
    parameters=None,
    state=None,
    address_width=None,
    endian="little",
) -> dict:
    """Compile a semantics body. ``location`` points just past its opening brace.

    Register reads and numeric literals are word-sized ``bits``. Bare
    immediates have their declared signedness and width. Let annotations
    must match exactly; register writes accept any word-width bit vector.
    Modular and bitwise operations accept like types, or same-width operands
    with at least one ``bits`` type, in which case the result is ``bits``.
    Right shift is arithmetic only for ``sint``. A shift by at least the
    value's width produces all fill bits.

    ``extract(value, high, low)`` selects an inclusive range with literal
    indices; ``concat(high, low)`` puts the first argument in the high bits.
    Both produce ``bits`` values. For a register operand, ``name := value;`` is
    shorthand for ``write(name, value);``; for named scalar state it is
    shorthand for ``write_state(name, value);``.

    ``peek`` and ``pop`` return a stack's element type. ``load<T>`` and
    ``store<T>`` require byte-aligned ``T`` and an unsigned, address-width
    address. Scalar, stack, and typed memory writes require compatible
    same-width values: ``bits`` is a neutral bit pattern, but ``sint`` and
    ``uint`` are not implicitly interchangeable.

    A ``sequence`` block can contain either kind of block. A ``parallel``
    block contains only ``let``, register/scalar writes, and memory stores;
    ordered stack mutation is rejected there. Blocks introduce lexical scopes:
    inherited bindings are visible, but locals never shadow another visible
    name or escape their block. Blocks have no trailing semicolon. SemIR is
    limited to 64 block levels, 128 expression levels and 65,536 total
    statement and expression nodes per program.
    """
    if address_width is None:
        address_width = word_width
    if endian not in {"little", "big"}:
        raise ArchonError("invalid architecture endianness", location)
    return _Compiler(
        source, location, operands, word_width, types, parameters or {},
        state, address_width, endian,
    ).compile()


def _signed(value: int, width: int) -> int:
    sign = 1 << (width - 1)
    return (value ^ sign) - sign


def execute_semantics(
    program: dict, operands: dict[str, int], registers: list[int], word_width: int, zero: int | None
) -> list[int]:
    """Execute compiled SemIR on a copy of the architectural register state.

    A parallel block reads from its entry state and commits only after all
    statements succeed. Writes to the same register coalesce if their
    evaluated bit vectors agree; different values raise ArchonError. Writes
    to a hardwired zero register are discarded (their RHS is still evaluated).
    Within a sequence, each completed write or parallel block is immediately
    visible to the next statement.
    """
    machine = {
        "registers": registers,
        "scalars": {},
        "stacks": {},
        "memories": {},
    }
    result = execute_machine_semantics(
        program, operands, machine, word_width, zero,
        {"scalars": [], "stacks": [], "memories": []},
        word_width, "little",
    )
    return result["registers"]


def execute_machine_semantics(
    program: dict,
    operands: dict[str, int],
    machine: dict,
    word_width: int,
    zero: int | None,
    state: dict,
    address_width: int,
    endian: str,
) -> dict:
    """Execute SemIR transactionally and return independent machine-state values.

    ``machine`` and the lists, mappings, and byte buffers it contains are never
    modified. Callers are expected to validate their public state object first;
    this boundary still rejects malformed direct inputs and malformed SemIR.
    """
    try:
        return _execute_machine(
            program, operands, machine, word_width, zero, state, address_width, endian,
        )
    except (KeyError, TypeError, ValueError, IndexError, RecursionError) as exc:
        raise ArchonError("malformed executable semantics") from exc


def _execute_machine(
    program, operands, machine, word_width, zero, state, address_width, endian,
):
    if type(word_width) is not int or not 1 <= word_width <= _MAX_WIDTH:
        raise ArchonError("invalid semantic register width")
    if type(address_width) is not int or not 1 <= address_width <= _MAX_WIDTH:
        raise ArchonError("invalid semantic address width")
    if endian not in {"little", "big"}:
        raise ArchonError("invalid semantic endianness")
    program_width = program["word_width"]
    if type(program_width) is not int or word_width != program_width:
        raise ArchonError("semantic program and register file word widths differ")
    scalar_specs = {row["name"]: row["type"] for row in state["scalars"]}
    stack_specs = {
        row["name"]: (row["element_type"], row["capacity"]) for row in state["stacks"]
    }
    memory_specs = {row["name"]: row["size"] for row in state["memories"]}
    updated = {
        "registers": list(machine["registers"]),
        "scalars": dict(machine["scalars"]),
        "stacks": {name: list(values) for name, values in machine["stacks"].items()},
        "memories": {
            name: bytearray(values) for name, values in machine["memories"].items()
        },
    }
    registers = updated["registers"]
    scalars = updated["scalars"]
    stacks = updated["stacks"]
    memories = updated["memories"]
    if scalars.keys() != scalar_specs.keys():
        raise ArchonError("machine scalar-state names do not match the architecture")
    if stacks.keys() != stack_specs.keys():
        raise ArchonError("machine stack names do not match the architecture")
    if memories.keys() != memory_specs.keys():
        raise ArchonError("machine memory names do not match the architecture")

    def mask(width):
        return (1 << width) - 1

    def checked_type(ty, label):
        kind, width = ty["kind"], ty["width"]
        if kind not in _KINDS or type(width) is not int or not 1 <= width <= _MAX_WIDTH:
            raise ArchonError(f"invalid {label} type")
        return kind, width

    def storage_compatible(actual, expected):
        actual_kind, actual_width = checked_type(actual, "semantic value")
        expected_kind, expected_width = checked_type(expected, "architectural state")
        return (
            actual_width == expected_width
            and (
                actual_kind == expected_kind
                or "bits" in {actual_kind, expected_kind}
            )
        )

    for name, spec in scalar_specs.items():
        _, width = checked_type(spec, f"scalar state {name!r}")
        value = scalars[name]
        if type(value) is not int or not 0 <= value <= mask(width):
            raise ArchonError(f"invalid value for scalar state {name!r}")
    for name, (spec, capacity) in stack_specs.items():
        _, width = checked_type(spec, f"stack {name!r}")
        values = stacks[name]
        if (type(capacity) is not int or not 1 <= capacity <= _MAX_STACK_CAPACITY
                or len(values) > capacity):
            raise ArchonError(f"invalid capacity or depth for stack {name!r}")
        if any(type(value) is not int or not 0 <= value <= mask(width) for value in values):
            raise ArchonError(f"invalid value in stack {name!r}")
    for name, size in memory_specs.items():
        if (type(size) is not int or not 1 <= size <= _MAX_MEMORY_BYTES
                or len(memories[name]) != size):
            raise ArchonError(f"invalid size for memory {name!r}")

    locals_: dict[str, int] = {}
    nodes = 0
    if zero is not None:
        if type(zero) is not int or not 0 <= zero < len(registers):
            raise ArchonError("zero register index is out of range")
        registers[zero] = 0

    def count_node() -> None:
        nonlocal nodes
        nodes += 1
        if nodes > _MAX_SEMIR_NODES:
            raise ArchonError(f"semantic program exceeds {_MAX_SEMIR_NODES} nodes")

    def operand(name: str) -> int:
        if name not in operands:
            raise ArchonError(f"missing semantic operand {name!r}")
        value = operands[name]
        if type(value) is not int:
            raise ArchonError(f"semantic operand {name!r} must be an integer")
        return value

    def register(name: str) -> int:
        index = operand(name)
        if not 0 <= index < len(registers):
            raise ArchonError(f"register index for operand {name!r} is out of range")
        return index

    def memory_range(name, address, width):
        if name not in memories:
            raise ArchonError(f"unknown semantic memory {name!r}")
        if width % 8:
            raise ArchonError("semantic memory access width is not byte-aligned")
        size = width // 8
        if address > len(memories[name]) - size:
            raise ArchonError(
                f"{size}-byte access at 0x{address:x} is out of bounds for memory {name!r}"
            )
        return size

    def evaluate_address(expr, depth, parallel):
        kind, width = checked_type(expr["type"], "memory address")
        if width != address_width or kind == "sint":
            raise ArchonError("invalid semantic memory address type")
        return evaluate(expr, depth, parallel=parallel)

    def evaluate(expr: dict, depth=1, *, parallel=False) -> int:
        count_node()
        if depth > _MAX_EXPR_DEPTH:
            raise ArchonError("semantic expression is nested too deeply")
        op = expr["op"]
        ty = expr["type"]
        width = ty["width"]
        if type(width) is not int or not 1 <= width <= _MAX_WIDTH:
            raise ArchonError("invalid semantic expression width")
        if ty["kind"] not in _KINDS:
            raise ArchonError("invalid semantic expression type kind")
        if op == "constant":
            value = expr["value"]
            if type(value) is not int:
                raise ArchonError("invalid semantic constant")
        elif op == "local":
            value = locals_[expr["name"]]
        elif op == "immediate":
            value = operand(expr["name"])
        elif op == "read":
            value = registers[register(expr["name"])]
        elif op == "read_state":
            name = expr["name"]
            spec = scalar_specs.get(name)
            if spec is None or ty != spec:
                raise ArchonError(f"invalid scalar-state read {name!r}")
            value = scalars[name]
        elif op in {"peek", "pop"}:
            name = expr["name"]
            row = stack_specs.get(name)
            if row is None or ty != row[0]:
                raise ArchonError(f"invalid stack read {name!r}")
            values = stacks[name]
            if op == "pop":
                if parallel:
                    raise ArchonError("pop is not permitted in a parallel block")
                if not values:
                    raise ArchonError(f"stack {name!r} underflow")
                value = values.pop()
            else:
                stack_depth = expr["depth"]
                if type(stack_depth) is not int or stack_depth < 0:
                    raise ArchonError("invalid stack peek depth")
                if stack_depth >= row[1]:
                    raise ArchonError("stack peek depth exceeds its declared capacity")
                if stack_depth >= len(values):
                    raise ArchonError(f"stack {name!r} underflow at peek depth {stack_depth}")
                value = values[-1 - stack_depth]
        elif op == "load":
            name = expr["name"]
            address = evaluate_address(expr["address"], depth + 1, parallel)
            size = memory_range(name, address, width)
            value = int.from_bytes(memories[name][address:address + size], endian)
        elif op in _CONVERSIONS or op == "~":
            arg = expr["value"]
            value = evaluate(arg, depth + 1, parallel=parallel)
            if op == "sext":
                value = _signed(value, arg["type"]["width"])
            elif op == "~":
                value = ~value
        elif op == "extract":
            arg = expr["value"]
            high, low = expr["high"], expr["low"]
            value = evaluate(arg, depth + 1, parallel=parallel)
            if (type(high) is not int or type(low) is not int
                    or not 0 <= low <= high < arg["type"]["width"]
                    or ty["kind"] != "bits" or width != high - low + 1):
                raise ArchonError("invalid semantic extract bounds or result type")
            value >>= low
        elif op == "concat":
            a, b = expr["left"], expr["right"]
            left = evaluate(a, depth + 1, parallel=parallel)
            right = evaluate(b, depth + 1, parallel=parallel)
            low_width = b["type"]["width"]
            if ty["kind"] != "bits" or width != a["type"]["width"] + low_width:
                raise ArchonError("invalid semantic concat result type")
            value = (left << low_width) | right
        elif op in _PRECEDENCE:
            left = evaluate(expr["left"], depth + 1, parallel=parallel)
            right = evaluate(expr["right"], depth + 1, parallel=parallel)
            if op == "+%":
                value = left + right
            elif op == "-%":
                value = left - right
            elif op == "*%":
                value = left * right
            elif op == "&":
                value = left & right
            elif op == "|":
                value = left | right
            elif op == "^":
                value = left ^ right
            elif op == "<<":
                value = 0 if right >= width else left << right
            else:
                if ty["kind"] == "sint":
                    left = _signed(left, width)
                value = left >> min(right, width)
        else:
            raise ArchonError(f"unknown semantic operation {op!r}")
        return value & mask(width)

    def run(statements: list[dict], depth: int, parallel: bool) -> None:
        if type(statements) is not list:
            raise ArchonError("malformed executable semantics: expected a statement list")
        if depth > _MAX_BLOCK_DEPTH:
            raise ArchonError(f"semantic blocks are nested too deeply (limit {_MAX_BLOCK_DEPTH})")
        bindings = []
        staged_registers: dict[int, int] = {}
        staged_scalars: dict[str, int] = {}
        staged_memory: dict[tuple[str, int], int] = {}

        def stage(target, key, value, message):
            if key in target and target[key] != value:
                raise ArchonError(message)
            target[key] = value

        for stmt in statements:
            count_node()
            op = stmt["op"]
            if op in ("sequence", "parallel"):
                if parallel:
                    raise ArchonError(
                        "a parallel block may only contain let and write statements "
                        "(register/scalar writes and stores)"
                    )
                run(stmt["statements"], depth + 1, op == "parallel")
            elif op == "let":
                name = stmt["name"]
                if name in locals_:
                    raise ArchonError(f"duplicate semantic binding {name!r}")
                locals_[name] = evaluate(stmt["value"], parallel=parallel)
                bindings.append(name)
            elif op == "write":
                index = register(stmt["name"])
                expr = stmt["value"]
                value = evaluate(expr, parallel=parallel)
                if expr["type"]["width"] != word_width:
                    raise ArchonError(f"semantic write expects a {word_width}-bit value")
                if index == zero:
                    continue
                if parallel:
                    # Nothing modifies the machine during this block, so all
                    # reads observe the entry state without copying it.
                    stage(
                        staged_registers, index, value,
                        f"conflicting parallel writes to register {index}",
                    )
                else:
                    registers[index] = value
            elif op == "write_state":
                name = stmt["name"]
                spec = scalar_specs.get(name)
                expr = stmt["value"]
                if spec is None or not storage_compatible(expr["type"], spec):
                    raise ArchonError(f"invalid scalar-state write {name!r}")
                value = evaluate(expr, parallel=parallel)
                if parallel:
                    stage(
                        staged_scalars, name, value,
                        f"conflicting parallel writes to scalar state {name!r}",
                    )
                else:
                    scalars[name] = value
            elif op == "push":
                if parallel:
                    raise ArchonError("push is not permitted in a parallel block")
                name = stmt["name"]
                row = stack_specs.get(name)
                expr = stmt["value"]
                if row is None or not storage_compatible(expr["type"], row[0]):
                    raise ArchonError(f"invalid stack push {name!r}")
                value = evaluate(expr)
                if len(stacks[name]) >= row[1]:
                    raise ArchonError(f"stack {name!r} overflow")
                stacks[name].append(value)
            elif op == "store":
                name = stmt["name"]
                address = evaluate_address(stmt["address"], 1, parallel)
                expr = stmt["value"]
                value = evaluate(expr, parallel=parallel)
                width = expr["type"]["width"]
                size = memory_range(name, address, width)
                data = value.to_bytes(size, endian)
                if parallel:
                    for offset, byte in enumerate(data):
                        key = name, address + offset
                        stage(
                            staged_memory, key, byte,
                            f"conflicting parallel stores to memory {name!r} "
                            f"at 0x{address + offset:x}",
                        )
                else:
                    memories[name][address:address + size] = data
            else:
                raise ArchonError(f"unknown semantic statement {op!r}")
        for name in bindings:
            del locals_[name]
        for index, value in staged_registers.items():
            registers[index] = value
        scalars.update(staged_scalars)
        for (name, address), value in staged_memory.items():
            memories[name][address] = value

    run(program["statements"], 0, False)
    return updated
