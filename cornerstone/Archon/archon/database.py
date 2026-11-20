"""Read and write deterministic ASIR JSON databases."""

import json
from pathlib import Path
import re

from .diagnostics import ArchonError, Location
from .semantics import (
    _CONVERSIONS, _KINDS, _MAX_BLOCK_DEPTH, _MAX_EXPR_DEPTH, _MAX_SEMIR_NODES,
    _MAX_MEMORY_BYTES, _MAX_STACK_CAPACITY, _MAX_WIDTH, _PRECEDENCE,
)


SCHEMA_VERSION = 1
_MAX_REGISTERS = 65536
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _expect(value, kind, key):
    if type(value) is not kind:
        labels = {dict: "object", list: "array", int: "integer", str: "string", bool: "boolean"}
        raise ArchonError(f"invalid ArchDB: {key} must be a JSON {labels[kind]}")
    return value


def _member(obj, key, kind, prefix=""):
    path = f"{prefix}.{key}" if prefix else key
    if key not in obj:
        raise ArchonError(f"invalid ArchDB: missing {path}")
    return _expect(obj[key], kind, path)


def _width(obj, prefix="", *, key="width"):
    width = _member(obj, key, int, prefix)
    if not 1 <= width <= _MAX_WIDTH:
        path = f"{prefix}.{key}" if prefix else key
        raise ArchonError(f"invalid ArchDB: {path} must be between 1 and {_MAX_WIDTH}")
    return width


def _semantic_type(value, path):
    ty = _expect(value, dict, path)
    kind = _member(ty, "kind", str, path)
    if kind not in _KINDS:
        raise ArchonError(f"invalid ArchDB: {path}.kind has invalid type kind {kind!r}")
    return kind, _width(ty, path)


def _compatible_types(actual, expected):
    return (
        actual[1] == expected[1]
        and (actual[0] == expected[0] or "bits" in {actual[0], expected[0]})
    )


def _state_model(db, address_width):
    """Validate optional schema-v1 extended state and return indexed rows."""
    empty = {"scalars": {}, "stacks": {}, "memories": {}}
    if "state" not in db:
        return empty
    state = _member(db, "state", dict)
    result = {}
    names = set()

    def name(row, path):
        value = _member(row, "name", str, path)
        if not _IDENTIFIER.fullmatch(value):
            raise ArchonError(f"invalid ArchDB: {path}.name must be an identifier")
        if value in names:
            raise ArchonError(f"invalid ArchDB: duplicate architectural state name {value!r}")
        names.add(value)
        return value

    scalars = {}
    for index, row in enumerate(_member(state, "scalars", list, "state")):
        path = f"state.scalars[{index}]"
        _expect(row, dict, path)
        row_name = name(row, path)
        _semantic_type(_member(row, "type", dict, path), f"{path}.type")
        scalars[row_name] = row
    result["scalars"] = scalars

    stacks = {}
    for index, row in enumerate(_member(state, "stacks", list, "state")):
        path = f"state.stacks[{index}]"
        _expect(row, dict, path)
        row_name = name(row, path)
        _semantic_type(_member(row, "element_type", dict, path), f"{path}.element_type")
        capacity = _member(row, "capacity", int, path)
        if not 1 <= capacity <= _MAX_STACK_CAPACITY:
            raise ArchonError(
                f"invalid ArchDB: {path}.capacity must be between 1 and {_MAX_STACK_CAPACITY}"
            )
        stacks[row_name] = row
    result["stacks"] = stacks

    memories = {}
    for index, row in enumerate(_member(state, "memories", list, "state")):
        path = f"state.memories[{index}]"
        _expect(row, dict, path)
        row_name = name(row, path)
        size = _member(row, "size", int, path)
        if not 1 <= size <= _MAX_MEMORY_BYTES:
            raise ArchonError(
                f"invalid ArchDB: {path}.size must be between 1 and {_MAX_MEMORY_BYTES}"
            )
        if size > 1 << address_width:
            raise ArchonError(
                f"invalid ArchDB: {path}.size exceeds the {address_width}-bit address space"
            )
        memories[row_name] = row
    result["memories"] = memories
    return result


def _semantics(program, path, operands, register_width, state, address_width):
    if program is None:
        return
    _expect(program, dict, path)
    if _width(program, path, key="word_width") != register_width:
        raise ArchonError(f"invalid ArchDB: {path}.word_width must match the register width")

    word_type = ("bits", register_width)
    locals_ = {}
    reads, writes = set(), set()
    state_reads, state_writes = set(), set()
    stack_reads, stack_writes = set(), set()
    memory_reads, memory_writes = set(), set()
    resources = state["scalars"].keys() | state["stacks"].keys() | state["memories"].keys()
    nodes = 0

    def bad(at, message):
        return ArchonError(f"invalid ArchDB: {message} at {at}")

    def count_node(at):
        nonlocal nodes
        nodes += 1
        if nodes > _MAX_SEMIR_NODES:
            raise bad(at, f"semantic program exceeds {_MAX_SEMIR_NODES} nodes")

    def operand(at, name, kind, directions):
        row = operands.get(name)
        if row is None or row["kind"] != kind:
            raise bad(at, f"invalid operand {name!r}")
        # Earlier schema-v1 readers did not require a direction field.
        if "direction" in row and row["direction"] not in directions:
            raise bad(at, f"invalid operand direction for {name!r}")
        return row

    def expression(expr, path, depth=1, *, parallel=False):
        count_node(path)
        if depth > _MAX_EXPR_DEPTH:
            raise bad(path, "semantic expression is nested too deeply")
        op = _member(expr, "op", str, path)
        ty = _member(expr, "type", dict, path)
        kind = _member(ty, "kind", str, f"{path}.type")
        if kind not in _KINDS:
            raise bad(f"{path}.type.kind", "invalid type kind")
        width = _width(ty, f"{path}.type")
        actual = (kind, width)

        def child(key):
            return expression(
                _member(expr, key, dict, path), f"{path}.{key}", depth + 1,
                parallel=parallel,
            )

        if op == "constant":
            value = _member(expr, "value", int, path)
            if value.bit_length() > width or value < (-(1 << (width - 1)) if kind == "sint" else 0):
                raise bad(f"{path}.value", "constant does not fit its type")
            expected = actual
        elif op == "local":
            name = _member(expr, "name", str, path)
            if name not in locals_:
                raise bad(f"{path}.name", f"unknown local {name!r}")
            expected = locals_[name]
        elif op in {"read", "immediate"}:
            name = _member(expr, "name", str, path)
            row = operand(f"{path}.name", name, "reg" if op == "read" else "imm", ("in", "inout"))
            if op == "read":
                reads.add(name)
                expected = word_type
            else:
                expected = ("sint" if row["signed"] else "uint", row["width"])
        elif op == "read_state":
            name = _member(expr, "name", str, path)
            row = state["scalars"].get(name)
            if row is None:
                raise bad(f"{path}.name", f"unknown scalar state {name!r}")
            state_reads.add(name)
            expected = _semantic_type(row["type"], f"state scalar {name!r}.type")
        elif op in {"peek", "pop"}:
            name = _member(expr, "name", str, path)
            row = state["stacks"].get(name)
            if row is None:
                raise bad(f"{path}.name", f"unknown stack {name!r}")
            if op == "pop" and parallel:
                raise bad(path, "pop is not permitted in a parallel block")
            if op == "peek":
                peek_depth = _member(expr, "depth", int, path)
                if not 0 <= peek_depth < row["capacity"]:
                    raise bad(
                        f"{path}.depth",
                        f"peek depth must be within stack capacity {row['capacity']}",
                    )
            stack_reads.add(name)
            if op == "pop":
                stack_writes.add(name)
            expected = _semantic_type(
                row["element_type"], f"state stack {name!r}.element_type",
            )
        elif op == "load":
            name = _member(expr, "name", str, path)
            if name not in state["memories"]:
                raise bad(f"{path}.name", f"unknown memory {name!r}")
            address = child("address")
            if address[1] != address_width or address[0] == "sint":
                raise bad(f"{path}.address", "invalid memory address type")
            if width % 8:
                raise bad(f"{path}.type", "memory access width must be byte-aligned")
            memory_reads.add(name)
            expected = actual
        elif op == "~":
            expected = child("value")
        elif op in _CONVERSIONS:
            source_kind, source_width = child("value")
            if (op == "trunc" and width > source_width) or (op != "trunc" and width < source_width):
                raise bad(f"{path}.type", f"invalid width for {op}")
            expected_kind = (
                "bits" if source_kind == "bits"
                else {"zext": "uint", "sext": "sint"}.get(op, source_kind)
            )
            expected = (expected_kind, width)
        elif op == "extract":
            _, source_width = child("value")
            high = _member(expr, "high", int, path)
            low = _member(expr, "low", int, path)
            if not 0 <= low <= high < source_width:
                raise bad(path, "invalid extract bounds")
            expected = ("bits", high - low + 1)
        elif op == "concat":
            a, b = child("left"), child("right")
            expected = ("bits", a[1] + b[1])
        elif op in _PRECEDENCE:
            a, b = child("left"), child("right")
            if op in {"<<", ">>"}:
                if b[0] == "sint":
                    raise bad(f"{path}.right.type", "invalid shift count type")
                expected = a
            else:
                if a[1] != b[1] or {a[0], b[0]} == {"sint", "uint"}:
                    raise bad(path, f"incompatible operand types for {op}")
                expected = a if a == b else ("bits", a[1])
        else:
            raise bad(f"{path}.op", f"unknown expression operation {op!r}")
        if actual != expected:
            raise bad(f"{path}.type", f"invalid result type for {op}")
        return actual

    def block(obj, path, depth=0, parallel=False):
        if depth > _MAX_BLOCK_DEPTH:
            raise bad(path, f"semantic blocks are nested too deeply (limit {_MAX_BLOCK_DEPTH})")
        bindings = []
        for i, statement in enumerate(_member(obj, "statements", list, path)):
            stmt_path = f"{path}.statements[{i}]"
            count_node(stmt_path)
            _expect(statement, dict, stmt_path)
            op = _member(statement, "op", str, stmt_path)
            if op in ("sequence", "parallel"):
                if parallel:
                    raise bad(stmt_path, "a parallel block may only contain let and write statements")
                block(statement, stmt_path, depth + 1, op == "parallel")
                continue
            if op not in ("let", "write", "write_state", "push", "store"):
                raise bad(f"{stmt_path}.op", f"unknown statement operation {op!r}")
            name = _member(statement, "name", str, stmt_path)
            ty = expression(
                _member(statement, "value", dict, stmt_path), f"{stmt_path}.value",
                parallel=parallel,
            )
            if op == "let":
                if name in locals_ or name in operands or name in resources:
                    raise bad(f"{stmt_path}.name", f"duplicate binding {name!r}")
                locals_[name] = ty
                bindings.append(name)
            elif op == "write":
                operand(f"{stmt_path}.name", name, "reg", ("out", "inout"))
                if ty[1] != register_width:
                    raise bad(f"{stmt_path}.value", f"write expects a {register_width}-bit value")
                writes.add(name)
            elif op == "write_state":
                row = state["scalars"].get(name)
                if row is None:
                    raise bad(f"{stmt_path}.name", f"unknown scalar state {name!r}")
                expected = _semantic_type(row["type"], f"state scalar {name!r}.type")
                if not _compatible_types(ty, expected):
                    raise bad(f"{stmt_path}.value", f"write_state({name}) has the wrong type")
                state_writes.add(name)
            elif op == "push":
                if parallel:
                    raise bad(stmt_path, "push is not permitted in a parallel block")
                row = state["stacks"].get(name)
                if row is None:
                    raise bad(f"{stmt_path}.name", f"unknown stack {name!r}")
                expected = _semantic_type(
                    row["element_type"], f"state stack {name!r}.element_type",
                )
                if not _compatible_types(ty, expected):
                    raise bad(f"{stmt_path}.value", f"push({name}) has the wrong type")
                stack_writes.add(name)
            else:
                if name not in state["memories"]:
                    raise bad(f"{stmt_path}.name", f"unknown memory {name!r}")
                address = expression(
                    _member(statement, "address", dict, stmt_path),
                    f"{stmt_path}.address", parallel=parallel,
                )
                if address[1] != address_width or address[0] == "sint":
                    raise bad(f"{stmt_path}.address", "invalid memory address type")
                if ty[1] % 8:
                    raise bad(f"{stmt_path}.value", "memory access width must be byte-aligned")
                memory_writes.add(name)
        for name in bindings:
            del locals_[name]

    block(program, path)
    effects = _member(program, "effects", dict, path)
    derived_effects = [("reads", reads), ("writes", writes)]
    if resources:
        derived_effects.extend([
            ("state_reads", state_reads),
            ("state_writes", state_writes),
            ("stack_reads", stack_reads),
            ("stack_writes", stack_writes),
            ("memory_reads", memory_reads),
            ("memory_writes", memory_writes),
        ])
    for key, derived in derived_effects:
        key_path = f"{path}.effects.{key}"
        names = set()
        for value in _member(effects, key, list, f"{path}.effects"):
            name = _expect(value, str, key_path)
            if name not in derived or name in names:
                raise bad(key_path, f"invalid or duplicate effect {name!r}")
            names.add(name)
        if names != derived:
            raise bad(key_path, "effects do not match semantic statements")


def _validate(db):
    _expect(db, dict, "database")
    version = _member(db, "version", int)
    if version != SCHEMA_VERSION:
        raise ArchonError(
            f"unsupported ArchDB schema version {version} (expected {SCHEMA_VERSION}); "
            "recompile the .arch source with this version of Archon"
        )

    _member(db, "architecture", str)
    address_width = _width(db, key="address_width")
    if _member(db, "endian", str) not in {"little", "big"}:
        raise ArchonError("invalid ArchDB: endian must be 'little' or 'big'")
    for key, value in _member(db, "parameters", dict).items():
        _expect(value, int, f"parameters.{key}")
    for i, feature in enumerate(_member(db, "features", list)):
        _expect(feature, str, f"features[{i}]")

    if "registers" not in db:
        raise ArchonError("invalid ArchDB: missing registers")
    regs = db["registers"]
    if regs is None:
        count = 0
        register_width = address_width
    else:
        _expect(regs, dict, "registers")
        _member(regs, "name", str, "registers")
        _member(regs, "prefix", str, "registers")
        count = _member(regs, "count", int, "registers")
        if not 1 <= count <= _MAX_REGISTERS:
            raise ArchonError(
                f"invalid ArchDB: registers.count must be between 1 and {_MAX_REGISTERS}"
            )
        register_width = _width(regs, "registers")
        if "zero" not in regs:
            raise ArchonError("invalid ArchDB: missing registers.zero")
        if regs["zero"] is not None:
            _reg_index(regs["zero"], count, "registers.zero")
        for alias, index in _member(regs, "aliases", dict, "registers").items():
            _reg_index(index, count, f"registers.aliases.{alias}")

    state = _state_model(db, address_width)

    for i, inst in enumerate(_member(db, "instructions", list)):
        path = f"instructions[{i}]"
        _expect(inst, dict, path)
        _member(inst, "id", str, path)
        _member(inst, "name", str, path)
        width = _width(inst, path)
        if width % 8 != 0:
            raise ArchonError(f"invalid ArchDB: {path}.width must be a multiple of eight")
        for key in ("mask", "value"):
            bits = _member(inst, key, int, path)
            if bits < 0 or bits.bit_length() > width:
                raise ArchonError(f"invalid ArchDB: {path}.{key} must fit in {width} bits")
        _member(inst, "assembly", str, path)
        operands = {}
        for j, row in enumerate(_member(inst, "operands", list, path)):
            operand_path = f"{path}.operands[{j}]"
            _expect(row, dict, operand_path)
            name = _member(row, "name", str, operand_path)
            kind = _member(row, "kind", str, operand_path)
            _member(row, "signed", bool, operand_path)
            _width(row, operand_path)
            if kind not in {"reg", "imm"}:
                raise ArchonError(f"invalid ArchDB: invalid operand kind at {operand_path}.kind")
            if kind == "reg" and regs is None:
                raise ArchonError(
                    f"invalid ArchDB: register operand at {operand_path} without a register file"
                )
            operands[name] = row
        for j, row in enumerate(_member(inst, "fields", list, path)):
            field_path = f"{path}.fields[{j}]"
            _expect(row, dict, field_path)
            _member(row, "operand", str, field_path)
            _member(row, "lsb", int, field_path)
            _width(row, field_path)
        if "semantics" not in inst:
            raise ArchonError(f"invalid ArchDB: missing {path}.semantics")
        _semantics(
            inst["semantics"], f"{path}.semantics", operands,
            register_width, state, address_width,
        )


def _reg_index(value, count, key):
    if not 0 <= _expect(value, int, key) < count:
        raise ArchonError(f"invalid ArchDB: {key} must index one of the {count} registers")


def _object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ArchonError(f"invalid ArchDB: duplicate JSON key {key!r}")
        obj[key] = value
    return obj


def _invalid_constant(value):
    raise ArchonError(f"invalid ArchDB: non-JSON number {value}")


def dumps(db: dict) -> str:
    """Serialize a compiled ASIR database with sorted object keys and a final newline."""
    try:
        text = json.dumps(db, indent=2, sort_keys=True, allow_nan=False) + "\n"
    except (TypeError, ValueError, RecursionError) as exc:
        raise ArchonError(f"cannot serialize ArchDB: {exc}") from exc
    _validate(db)
    return text


def loads(text: str, *, source: str = "<ArchDB>") -> dict:
    """Parse ASIR JSON and reject malformed or unsupported databases."""
    try:
        db = json.loads(text, object_pairs_hook=_object, parse_constant=_invalid_constant)
        _validate(db)
        return db
    except json.JSONDecodeError as exc:
        raise ArchonError(
            f"malformed ArchDB JSON: {exc.msg}", Location(source, exc.lineno, exc.colno)
        ) from exc
    except ArchonError as exc:
        raise ArchonError(f"{source}: {exc.message}") from exc
    except (ValueError, RecursionError) as exc:
        raise ArchonError(f"{source}: malformed ArchDB JSON: {exc}") from exc


def save(db: dict, path: str | Path) -> None:
    """Write ASIR JSON to a path in UTF-8."""
    text = dumps(db)
    try:
        Path(path).write_text(text, encoding="utf-8", newline="\n")
    except (OSError, UnicodeError) as exc:
        raise ArchonError(f"cannot write ArchDB {str(path)!r}: {exc}") from exc


def load(path: str | Path) -> dict:
    """Read an ArchDB produced by this schema version of Archon."""
    try:
        text = Path(path).read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise ArchonError(f"cannot read ArchDB {str(path)!r}: {exc}") from exc
    return loads(text, source=str(path))
