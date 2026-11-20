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
ABI_FACTS_SCHEMA_VERSION = 1
ABI_FACTS_V2_SCHEMA_VERSION = 2
_MAX_REGISTERS = 65536
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_ABI_STACK_BOUND = 1 << 30
_ABI_COMMON_KEYS = {
    "schema_version", "name", "register_file", "args", "result", "caller_saved",
    "callee_saved", "unallocatable", "stack_pointer", "return_address",
    "stack_align", "stack_slot_bytes", "stack_arg_offset", "stack_growth",
}


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


def _abi_header(value, regs, path, expected_version):
    facts = _expect(value, dict, path)
    version = _member(facts, "schema_version", int, path)
    if version != expected_version:
        raise ArchonError(f"unsupported ArchDB {path} schema version {version} "
                          f"(expected {expected_version})")
    if not _IDENTIFIER.fullmatch(_member(facts, "name", str, path)):
        raise ArchonError(f"invalid ArchDB: {path}.name must be an identifier")
    file = _member(facts, "register_file", str, path)
    if regs is None or file != regs["name"]:
        raise ArchonError(f"invalid ArchDB: {path}.register_file must name the register file")
    return facts


def _abi_roles(facts, path, count):
    _reg_index(_member(facts, "result", int, path), count, f"{path}.result")
    roles = {}
    for key in ("args", "caller_saved", "callee_saved", "unallocatable"):
        indices = _member(facts, key, list, path)
        seen = set()
        for i, index in enumerate(indices):
            _reg_index(index, count, f"{path}.{key}[{i}]")
            if index in seen:
                raise ArchonError(f"invalid ArchDB: duplicate register in {path}.{key}")
            seen.add(index)
        if key != "args" and indices != sorted(seen):
            raise ArchonError(f"invalid ArchDB: {path}.{key} must be sorted by register index")
        roles[key] = seen
    return roles


def _check_abi_roles(facts, regs, roles, return_index, path):
    caller = roles["caller_saved"]
    callee = roles["callee_saved"]
    fixed = roles["unallocatable"]
    if caller & (callee | fixed):
        raise ArchonError(f"invalid ArchDB: {path}.caller_saved overlaps preserved or fixed registers")
    if regs["zero"] is not None and regs["zero"] not in fixed - callee:
        raise ArchonError(f"invalid ArchDB: {path} must classify the zero register as fixed only")
    if len(caller | callee | fixed) != regs["count"]:
        raise ArchonError(f"invalid ArchDB: {path} must classify every register")
    if not roles["args"] <= caller - {return_index} or facts["result"] not in caller - {return_index}:
        raise ArchonError(f"invalid ArchDB: {path} argument and result registers must be "
                          "caller-saved and distinct from the return address")


def _abi_stack_layout(facts, path):
    for key in ("stack_slot_bytes", "stack_align"):
        size = _member(facts, key, int, path)
        if not 1 <= size <= _ABI_STACK_BOUND or size & (size - 1):
            raise ArchonError(f"invalid ArchDB: {path}.{key} must be a positive power of two "
                              "no larger than 2^30")
    if facts["stack_align"] < facts["stack_slot_bytes"]:
        raise ArchonError(f"invalid ArchDB: {path}.stack_align is smaller than a stack slot")
    if _member(facts, "stack_growth", str, path) not in {"downward", "upward"}:
        raise ArchonError(f"invalid ArchDB: {path}.stack_growth must be downward or upward")


def _abi_keys(value, keys, path):
    extra = value.keys() - keys
    if extra:
        raise ArchonError(f"invalid ArchDB: unknown {path} member(s): "
                          f"{', '.join(sorted(map(repr, extra)))}")


def _validate_abi_facts(value, regs):
    """Validate the unchanged version-1 integer-register ABI subsection."""
    path = "abi_facts"
    facts = _abi_header(value, regs, path, ABI_FACTS_SCHEMA_VERSION)
    for key in ("stack_pointer", "return_address"):
        _reg_index(_member(facts, key, int, path), regs["count"], f"{path}.{key}")
    roles = _abi_roles(facts, path, regs["count"])
    sp, ra = facts["stack_pointer"], facts["return_address"]
    if sp not in roles["callee_saved"] & roles["unallocatable"] or ra not in roles["caller_saved"]:
        raise ArchonError("invalid ArchDB: abi_facts requires a preserved, fixed stack pointer "
                          "and a caller-saved return address")
    _check_abi_roles(facts, regs, roles, ra, path)
    _abi_stack_layout(facts, path)
    offset = _member(facts, "stack_arg_offset", int, path)
    if not 0 <= offset <= _ABI_STACK_BOUND or offset % facts["stack_slot_bytes"]:
        raise ArchonError("invalid ArchDB: abi_facts.stack_arg_offset must be a nonnegative, "
                          "slot-aligned byte offset no larger than 2^30")
    _abi_keys(facts, _ABI_COMMON_KEYS, path)


def _validate_abi_facts_v2(value, regs, scalars, address_width):
    """Validate tagged entry locations without requiring an executable call ISA."""
    path = "abi_facts_v2"
    facts = _abi_header(value, regs, path, ABI_FACTS_V2_SCHEMA_VERSION)
    roles = _abi_roles(facts, path, regs["count"])
    _abi_stack_layout(facts, path)
    slot_size, align = facts["stack_slot_bytes"], facts["stack_align"]
    address_space_size = 1 << address_width
    if align > address_space_size:
        raise ArchonError(f"invalid ArchDB: {path}.stack_align exceeds the address space")
    remainder = _member(facts, "stack_entry_remainder", int, path)
    if not 0 <= remainder < align:
        raise ArchonError(
            f"invalid ArchDB: {path}.stack_entry_remainder must be in [0, stack_align)"
        )

    def offset(obj, key, prefix, size):
        distance = _member(obj, key, int, prefix)
        if (abs(distance) > _ABI_STACK_BOUND
                or not -address_space_size < distance <= address_space_size - size):
            raise ArchonError(
                f"invalid ArchDB: {prefix}.{key} is outside the supported byte offset range"
            )
        return distance

    def location(key, payloads):
        loc = _member(facts, key, dict, path)
        prefix = f"{path}.{key}"
        kind = _member(loc, "kind", str, prefix)
        if kind not in payloads:
            raise ArchonError(f"invalid ArchDB: invalid location kind {kind!r} for {prefix}")
        _abi_keys(loc, {"kind"} | payloads[kind], prefix)
        return kind, loc, prefix

    def register(loc, prefix):
        index = _member(loc, "index", int, prefix)
        _reg_index(index, regs["count"], f"{prefix}.index")
        if regs["width"] != address_width:
            raise ArchonError(f"invalid ArchDB: {prefix} must use an address-width register")
        return index

    first_arg = offset(facts, "stack_arg_offset", path, slot_size)
    if (remainder + first_arg) % slot_size:
        raise ArchonError(f"invalid ArchDB: {path}.stack_arg_offset must give a slot-aligned "
                          "address at entry")

    kind, sp, prefix = location("stack_pointer", {"register": {"index"}, "state": {"name"}})
    if kind == "register":
        if register(sp, prefix) not in roles["callee_saved"] & roles["unallocatable"]:
            raise ArchonError(f"invalid ArchDB: {prefix} must be a preserved, fixed register")
    else:
        name = _member(sp, "name", str, prefix)
        if name not in scalars:
            raise ArchonError(f"invalid ArchDB: {prefix}.name must name a declared scalar state cell")
        ty = scalars[name]["type"]
        if ty["width"] != address_width or ty["kind"] == "sint":
            raise ArchonError(f"invalid ArchDB: {prefix} must use address-width bits or uint state")

    kind, ra, prefix = location("return_address", {
        "register": {"index"}, "stack_slot": {"offset", "size_bytes"},
    })
    return_index = None
    if kind == "register":
        return_index = register(ra, prefix)
        if return_index not in roles["caller_saved"]:
            raise ArchonError(f"invalid ArchDB: {prefix} must be a caller-saved register")
    else:
        size = _member(ra, "size_bytes", int, prefix)
        if size != (address_width + 7) // 8:
            raise ArchonError(f"invalid ArchDB: {prefix}.size_bytes must store one architectural "
                              "address (ceil(address_width / 8) bytes)")
        start = offset(ra, "offset", prefix, size)
        return_end = start + size
        first_arg_end = first_arg + slot_size
        outside_args = (
            return_end <= first_arg if facts["stack_growth"] == "downward"
            else start >= first_arg_end
        )
        if not outside_args:
            raise ArchonError(f"invalid ArchDB: {prefix} stack slot must be outside the "
                              "incoming scalar argument area")
        if max(return_end, first_arg_end) - min(start, first_arg) > address_space_size:
            raise ArchonError(f"invalid ArchDB: {prefix} and the first argument slot "
                              "cannot fit in the address space together")
    _check_abi_roles(facts, regs, roles, return_index, path)
    _abi_keys(facts, _ABI_COMMON_KEYS | {"stack_entry_remainder"}, path)


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
    if "abi_facts" in db and "abi_facts_v2" in db:
        raise ArchonError("invalid ArchDB: cannot contain both abi_facts and abi_facts_v2")
    if "abi_facts" in db:
        _validate_abi_facts(db["abi_facts"], regs)
    if "abi_facts_v2" in db:
        _validate_abi_facts_v2(db["abi_facts_v2"], regs, state["scalars"], address_width)

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
        scoped = set()
        for j, row in enumerate(_member(inst, "operands", list, path)):
            operand_path = f"{path}.operands[{j}]"
            _expect(row, dict, operand_path)
            name = _member(row, "name", str, operand_path)
            kind = _member(row, "kind", str, operand_path)
            _member(row, "signed", bool, operand_path)
            operand_width = _width(row, operand_path)
            if kind not in {"reg", "imm"}:
                raise ArchonError(f"invalid ArchDB: invalid operand kind at {operand_path}.kind")
            if kind == "reg" and regs is None:
                raise ArchonError(
                    f"invalid ArchDB: register operand at {operand_path} without a register file"
                )
            if name in operands:
                raise ArchonError(f"invalid ArchDB: duplicate operand {name!r} at {operand_path}")
            _operand_register_range(row, count, operand_width, operand_path)
            if "register_range" in row:
                scoped.add(name)
            operands[name] = row
        scoped_fields = set()
        for j, row in enumerate(_member(inst, "fields", list, path)):
            field_path = f"{path}.fields[{j}]"
            _expect(row, dict, field_path)
            name = _member(row, "operand", str, field_path)
            lsb = _member(row, "lsb", int, field_path)
            field_width = _width(row, field_path)
            if name in scoped:
                if name in scoped_fields:
                    raise ArchonError(f"invalid ArchDB: multiple fields for scoped operand "
                                      f"{name!r} at {field_path}")
                if field_width != operands[name]["width"]:
                    raise ArchonError(f"invalid ArchDB: {field_path} width differs from its "
                                      "scoped register operand")
                if not 0 <= lsb <= width - field_width:
                    raise ArchonError(f"invalid ArchDB: {field_path} is outside the instruction encoding")
                scoped_fields.add(name)
        if scoped_fields != scoped:
            raise ArchonError(f"invalid ArchDB: missing encoding field for scoped operand(s) "
                              f"{', '.join(sorted(scoped - scoped_fields))} at {path}")
        if "semantics" not in inst:
            raise ArchonError(f"invalid ArchDB: missing {path}.semantics")
        _semantics(
            inst["semantics"], f"{path}.semantics", operands,
            register_width, state, address_width,
        )


def _reg_index(value, count, key):
    if not 0 <= _expect(value, int, key) < count:
        raise ArchonError(f"invalid ArchDB: {key} must index one of the {count} registers")


def _operand_register_range(row, count, width, path):
    """Return inclusive operand bounds, or None for an immediate."""
    key = f"{path}.register_range"
    if row["kind"] != "reg":
        if "register_range" in row:
            raise ArchonError(f"invalid ArchDB: {key} is only valid on register operands")
        return None
    if "register_range" not in row:
        if (count - 1).bit_length() > width:
            raise ArchonError(f"invalid ArchDB: {path} cannot encode the register file")
        return 0, count - 1
    bounds = _member(row, "register_range", list, path)
    if len(bounds) != 2:
        raise ArchonError(f"invalid ArchDB: {key} must have two inclusive register indices")
    first, last = bounds
    _reg_index(first, count, f"{key}[0]")
    _reg_index(last, count, f"{key}[1]")
    if first > last:
        raise ArchonError(f"invalid ArchDB: {key} must be an ordered range")
    if last.bit_length() > width:
        raise ArchonError(f"invalid ArchDB: {key} does not fit the operand's encoding width")
    if row["signed"]:
        raise ArchonError(f"invalid ArchDB: a scoped register operand cannot be signed at {path}")
    return first, last


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
