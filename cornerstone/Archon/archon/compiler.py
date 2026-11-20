"""Load Archon modules and elaborate the fixed-width ISA subset into ASIR."""

from collections.abc import Mapping
from pathlib import Path
import re

from . import database
from .archdef import elaborate_instructions
from .diagnostics import ArchonError, Location
from .frontend import parse
from .semantics import _MAX_MEMORY_BYTES, _MAX_STACK_CAPACITY, compile_semantics


_IDENTIFIER = r"[A-Za-z_][A-Za-z0-9_]*"
_IMPORT = re.compile(rf"{_IDENTIFIER}(?:\.{_IDENTIFIER})*")
_PLACEHOLDER = re.compile(rf"\{{({_IDENTIFIER})\}}")
_ADJACENT_PLACEHOLDERS = re.compile(rf"\{{{_IDENTIFIER}\}}\{{{_IDENTIFIER}\}}")
_MNEMONIC = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*(?=$|[ \t,()\[\]])")
_MAX_WIDTH = 4096
_MAX_REGISTERS = 65536
_DECLARATIONS = {
    "setting", "parameter", "feature", "type", "register_file", "register_class",
    "abi_facts", "abi_facts_v2", "state", "stack", "memory", "format", "instruction",
    "template", "def",
}


def _fail(node, message):
    raise ArchonError(message, node["loc"])


def _load(path):
    """Flatten the unqualified declarations of a graph of source files."""
    declarations = []
    loaded = set()
    active = []

    def visit(path, imported_at=None):
        location = imported_at or Location(str(path), 1, 1)
        try:
            canonical = path.resolve()
        except (OSError, RuntimeError) as exc:
            raise ArchonError(f"cannot resolve {path}: {exc}", location) from exc
        if canonical in active:
            cycle = active[active.index(canonical):] + [canonical]
            chain = " -> ".join(str(entry) for entry in cycle)
            raise ArchonError(f"import cycle: {chain}", location)
        if canonical in loaded:
            return None

        try:
            source = canonical.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise ArchonError(f"cannot read {canonical}: {exc}", location) from exc
        unit = parse(source, str(canonical))
        expected = "architecture" if imported_at is None else "module"
        if unit["kind"] != expected:
            article = "an" if expected == "architecture" else "a"
            _fail(unit, f"expected {article} {expected} declaration in {canonical}")

        active.append(canonical)
        for decl in unit["declarations"]:
            if decl["kind"] != "import":
                declarations.append(decl)
                continue
            dotted = decl["path"]
            if not isinstance(dotted, str) or not _IMPORT.fullmatch(dotted):
                _fail(decl, f"invalid dotted import path {dotted!r}")
            relative = Path(*dotted.split(".")).with_suffix(".arch")
            visit(canonical.parent / relative, decl["loc"])
        active.pop()
        loaded.add(canonical)
        return unit

    root = visit(Path(path))
    return root, declarations


def _collect(declarations):
    groups = {kind: {} for kind in _DECLARATIONS}
    symbols = {}
    for decl in declarations:
        kind, name = decl["kind"], decl["name"]
        if kind not in groups:
            _fail(decl, f"unsupported declaration {kind!r}")
        scope = groups[kind] if kind == "setting" else symbols
        if name in scope:
            previous = scope[name]["loc"]
            _fail(decl, f"duplicate declaration {name!r} (first declared at {previous})")
        scope[name] = decl
        groups[kind][name] = decl
    return {kind: dict(sorted(group.items())) for kind, group in groups.items()}


def _parameters(declarations, overrides, root):
    overrides = {} if overrides is None else overrides
    if not isinstance(overrides, Mapping):
        _fail(root, "parameters must be a mapping from names to natural numbers")
    for name in overrides:
        if name not in declarations:
            _fail(root, f"unknown parameter {name!r}")
    result = {}
    for name, decl in declarations.items():
        values = decl["values"]
        if not values or any(type(value) is not int or value < 0 for value in values):
            _fail(decl, f"parameter {name!r} must have a nonempty domain of natural numbers")
        if len(values) != len(set(values)):
            _fail(decl, f"parameter {name!r} has duplicate allowed values")
        if type(decl["default"]) is not int or decl["default"] not in values:
            _fail(decl, f"default for parameter {name!r} is outside its domain")
        value = overrides.get(name, decl["default"])
        if type(value) is not int or value < 0 or value not in values:
            _fail(decl, f"value {value!r} for parameter {name!r} is outside its domain")
        result[name] = value
    return result


def _requirements(decl, features):
    seen = set()
    for name in decl["requires"]:
        if name not in features:
            _fail(decl, f"unknown required feature {name!r}")
        if name in seen:
            _fail(decl, f"duplicate required feature {name!r}")
        seen.add(name)
    return seen


def _features(declarations, requested, root):
    graph = {name: _requirements(decl, declarations)
             for name, decl in declarations.items()}
    done = set()
    active = []

    def visit(name):
        if name in active:
            cycle = active[active.index(name):] + [name]
            _fail(declarations[active[-1]], f"feature dependency cycle: {' -> '.join(cycle)}")
        if name not in done:
            active.append(name)
            for dependency in sorted(graph[name]):
                visit(dependency)
            active.pop()
            done.add(name)

    for name in graph:
        visit(name)
    if requested is None:
        enabled = {name for name, decl in declarations.items() if decl["enabled"]}
    else:
        if not isinstance(requested, (list, set, frozenset, tuple)) or any(
            not isinstance(name, str) for name in requested
        ):
            _fail(root, "features must be a list or set of feature names")
        enabled = set(requested)
    for name in sorted(enabled):
        if name not in declarations:
            _fail(root, f"unknown feature {name!r}")
        missing = graph[name] - enabled
        if missing:
            required = ", ".join(sorted(missing))
            _fail(declarations[name], f"feature {name!r} requires enabled feature(s): {required}")
    return enabled


def _natural(value, parameters, node, subject, maximum):
    if isinstance(value, str):
        if value not in parameters:
            _fail(node, f"unknown parameter {value!r} for {subject}")
        value = parameters[value]
    if type(value) is not int or not 1 <= value <= maximum:
        _fail(node, f"{subject} must be between 1 and {maximum}")
    return value


def _settings(declarations, parameters, root):
    for name, decl in declarations.items():
        if name in {"description", "version"}:
            if not isinstance(decl["value"], str):
                _fail(decl, f"setting {name!r} must be a string")
        elif name not in {"address_width", "endian"}:
            _fail(decl, f"unsupported architecture setting {name!r}")
    for name in ("address_width", "endian"):
        if name not in declarations:
            _fail(root, f"missing required architecture setting {name!r}")
    address = declarations["address_width"]
    width = _natural(address["value"], parameters, address, "address width", _MAX_WIDTH)
    endian = declarations["endian"]
    if endian["value"] not in ("little", "big"):
        _fail(endian, "endian must be 'little' or 'big'")
    return width, endian["value"]


class _Types:
    def __init__(self, declarations, parameters):
        self.declarations = declarations
        self.parameters = parameters
        self.resolved = {}
        self.active = []

    def resolve(self, spec, node):
        kind, arg = spec["kind"], spec["arg"]
        if kind == "named":
            if arg not in self.declarations:
                _fail(node, f"unknown type {arg!r}")
            if arg in self.active:
                cycle = self.active[self.active.index(arg):] + [arg]
                _fail(node, f"type alias cycle: {' -> '.join(cycle)}")
            if arg not in self.resolved:
                decl = self.declarations[arg]
                self.active.append(arg)
                self.resolved[arg] = self.resolve(decl["spec"], decl)
                self.active.pop()
            return self.resolved[arg]
        if kind not in {"bits", "uint", "sint"}:
            _fail(node, f"unsupported type {kind!r}")
        width = _natural(arg, self.parameters, node, f"{kind} width", _MAX_WIDTH)
        return {"kind": kind, "width": width}

    def resolve_all(self):
        for name, decl in self.declarations.items():
            self.resolve({"kind": "named", "arg": name}, decl)
        return dict(sorted(self.resolved.items()))


def _registers(files, classes, parameters):
    if not files:
        if classes:
            _fail(next(iter(classes.values())), "a register_class requires a register file")
        return None
    if len(files) != 1:
        _fail(list(files.values())[1], "multiple register files are not supported")
    name, decl = next(iter(files.items()))
    count = _natural(decl["count"], parameters, decl, "register count", _MAX_REGISTERS)
    width = _natural(decl["width"], parameters, decl, "register width", _MAX_WIDTH)
    if decl["start"] != 0 or decl["end"] != count - 1:
        _fail(decl, f"register range must be contiguous and zero-based (0..{count - 1})")
    prefix = decl["prefix"]
    if not isinstance(prefix, str) or any(ch.isspace() or ch in ",{}()" for ch in prefix):
        _fail(decl, f"invalid register prefix {prefix!r}")
    zero = decl["zero"]
    if zero is not None and (type(zero) is not int or not 0 <= zero < count):
        _fail(decl, f"zero register index {zero!r} is out of range")
    aliases = dict(sorted(decl["aliases"].items()))
    for alias, index in aliases.items():
        if type(index) is not int or not 0 <= index < count:
            _fail(decl, f"register alias {alias!r} has an out-of-range index")
        suffix = alias[len(prefix):]
        if (alias.startswith(prefix) and re.fullmatch(r"[0-9]+", suffix)
                and (suffix.lstrip("0") or "0") != str(index)):
            _fail(decl, f"register alias {alias!r} conflicts with a numbered register")
    for register_class in classes.values():
        if register_class["file"] != name:
            _fail(register_class, f"unknown register file {register_class['file']!r}")
        if "register_range" in register_class:
            first, last = register_class["register_range"]
            if not 0 <= first <= last < count:
                _fail(register_class, f"register class range [{first}..{last}] must be an "
                                      f"ordered range within 0..{count - 1}")
    return {"name": name, "prefix": prefix, "count": count, "width": width,
            "zero": zero, "aliases": aliases}


def _abi_facts(section, declarations, registers, state, address_width):
    if not declarations:
        return None
    if len(declarations) != 1:
        _fail(list(declarations.values())[1], f"only one {section} declaration is supported")
    name, decl = next(iter(declarations.items()))
    facts = {"name": name, **decl["members"]}
    v2 = section == "abi_facts_v2"
    version = database.ABI_FACTS_V2_SCHEMA_VERSION if v2 else database.ABI_FACTS_SCHEMA_VERSION
    if facts["schema_version"] != version:
        _fail(decl, f"unsupported {section} schema version {facts['schema_version']} "
                    f"(expected {version})")
    if registers is None or facts["register_file"] != registers["name"]:
        _fail(decl, f"unknown {section} register file {facts['register_file']!r}")
    aliases = registers["aliases"]

    def resolve(spelling, key):
        if spelling in aliases:
            return aliases[spelling]
        prefix = registers["prefix"]
        suffix = spelling[len(prefix):]
        if spelling.startswith(prefix) and re.fullmatch(r"[0-9]+", suffix):
            digits = suffix.lstrip("0") or "0"
            if len(digits) <= len(str(registers["count"] - 1)):
                index = int(digits)
                if index < registers["count"]:
                    return index
        _fail(decl, f"unknown register alias or numbered register {spelling!r} "
                    f"for {section}.{key}")

    facts["result"] = resolve(facts["result"], "result")
    for key in ("stack_pointer", "return_address"):
        if not v2:
            facts[key] = resolve(facts[key], key)
        elif facts[key]["kind"] == "register":
            facts[key] = {"kind": "register", "index": resolve(facts[key]["name"], key)}
    facts["args"] = [resolve(alias, "args") for alias in facts["args"]]
    for key in ("caller_saved", "callee_saved", "unallocatable"):
        facts[key] = sorted(resolve(alias, key) for alias in facts[key])
    try:
        if v2:
            scalars = {row["name"]: row for row in state["scalars"]}
            database._validate_abi_facts_v2(facts, registers, scalars, address_width)
        else:
            database._validate_abi_facts(facts, registers)
    except ArchonError as exc:
        _fail(decl, exc.message.removeprefix("invalid ArchDB: "))
    return facts


def _machine_state(scalars, stacks, memories, resolver, parameters, address_width):
    """Normalize explicitly bounded non-register architectural state."""
    scalar_rows = []
    for name, decl in scalars.items():
        scalar_rows.append({"name": name, "type": resolver.resolve(decl["spec"], decl)})

    stack_rows = []
    for name, decl in stacks.items():
        stack_rows.append({
            "name": name,
            "element_type": resolver.resolve(decl["spec"], decl),
            "capacity": _natural(
                decl["capacity"], parameters, decl, f"stack {name!r} capacity",
                _MAX_STACK_CAPACITY,
            ),
        })

    memory_rows = []
    for name, decl in memories.items():
        size = _natural(
            decl["size"], parameters, decl, f"memory {name!r} size",
            _MAX_MEMORY_BYTES,
        )
        if size > 1 << address_width:
            _fail(
                decl,
                f"memory {name!r} size exceeds the {address_width}-bit address space",
            )
        memory_rows.append({
            "name": name,
            "size": size,
        })
    return {"scalars": scalar_rows, "stacks": stack_rows, "memories": memory_rows}


def _bitmask(width):
    return (1 << width) - 1


def _formats(declarations, types, parameters):
    formats = {}
    for name, decl in declarations.items():
        width = _natural(decl["width"], parameters, decl, "instruction width", _MAX_WIDTH)
        if width % 8:
            _fail(decl, "instruction width must be a multiple of eight bits")
        fields = {}
        occupied = 0
        for field in decl["fields"]:
            field_name = field["name"]
            if field_name in fields:
                _fail(field, f"duplicate format field {field_name!r}")
            msb, lsb = field["msb"], field["lsb"]
            if not 0 <= lsb <= msb < width:
                _fail(field, f"field {field_name!r} bit range [{msb}:{lsb}] is out of bounds")
            spec = types.resolve(field["spec"], field)
            field_width = msb - lsb + 1
            if spec["width"] != field_width:
                _fail(field, f"field {field_name!r} type width does not match its bit range")
            mask = _bitmask(field_width) << lsb
            if occupied & mask:
                _fail(field, f"field {field_name!r} overlaps another format field")
            occupied |= mask
            fields[field_name] = (lsb, field_width, spec["kind"] == "sint")
        if occupied != _bitmask(width):
            _fail(decl, f"format {name!r} does not cover all {width} bits")
        formats[name] = (width, fields)
    return formats


def _assembly(decl, operands):
    template = decl["assembly"]
    if any(ch != "\t" and not ch.isprintable() for ch in template):
        _fail(decl, "assembly template must be one printable line")
    template = template.strip(" \t")
    mnemonic = _MNEMONIC.match(template)
    if not mnemonic:
        _fail(decl, "assembly template must start with a mnemonic")
    remaining = _PLACEHOLDER.sub("", template)
    if "{" in remaining or "}" in remaining:
        _fail(decl, "invalid assembly placeholder")
    if _ADJACENT_PLACEHOLDERS.search(template):
        _fail(decl, "assembly placeholders must have a separator")
    seen = set()
    for match in _PLACEHOLDER.finditer(template):
        name = match[1]
        if name not in operands:
            _fail(decl, f"unknown assembly placeholder {name!r}")
        if name in seen:
            _fail(decl, f"duplicate assembly placeholder {name!r}")
        seen.add(name)
    if operands.keys() - seen:
        missing = ", ".join(sorted(operands.keys() - seen))
        _fail(decl, f"missing assembly placeholder(s): {missing}")
    return template, mnemonic[0]


def _instruction(
    decl, architecture, formats, classes, registers, state, address_width, endian, resolver,
):
    name = decl["name"]
    operands = {}
    for operand in decl["operands"]:
        operand_name = operand["name"]
        if operand_name in operands:
            _fail(operand, f"duplicate operand {operand_name!r}")
        direction = operand["direction"]
        if direction not in ("in", "out", "inout"):
            _fail(operand, f"invalid operand direction {direction!r}")
        normalized = {"name": operand_name, "direction": direction}
        spec = operand["type"]
        if isinstance(spec, str):
            if spec not in classes:
                _fail(operand, f"unknown register class {spec!r}")
            if registers is None:
                _fail(operand, "a register operand requires a register file")
            normalized.update(kind="reg", signed=False)
            if "register_range" in classes[spec]:
                normalized["register_range"] = classes[spec]["register_range"].copy()
        else:
            if direction != "in":
                _fail(operand, "an immediate operand must have 'in' direction")
            resolved = resolver.resolve(spec, operand)
            normalized.update(kind="imm", width=resolved["width"],
                              signed=resolved["kind"] == "sint")
        operands[operand_name] = normalized

    format_name = decl["format"]
    if format_name not in formats:
        _fail(decl, f"unknown instruction format {format_name!r}")
    width, fields = formats[format_name]
    encoding = decl["encoding"]
    if encoding.keys() - fields.keys():
        names = ", ".join(sorted(encoding.keys() - fields.keys()))
        _fail(decl, f"unknown encoding field(s): {names}")
    if fields.keys() - encoding.keys():
        names = ", ".join(sorted(fields.keys() - encoding.keys()))
        _fail(decl, f"missing encoding field(s): {names}")

    mask = value = 0
    variable_fields = []
    encoded_operands = set()
    for field_name, (lsb, field_width, signed) in sorted(fields.items(),
                                                        key=lambda item: item[1][0]):
        binding = encoding[field_name]
        field_mask = _bitmask(field_width)
        if type(binding) is int:
            lower = -(1 << (field_width - 1)) if signed else 0
            upper = -lower - 1 if signed else field_mask
            if not lower <= binding <= upper:
                _fail(decl, f"constant {binding} does not fit encoding field {field_name!r}")
            mask |= field_mask << lsb
            value |= (binding & field_mask) << lsb
            continue

        if binding not in operands:
            _fail(decl, f"unknown operand {binding!r} in encoding field {field_name!r}")
        if binding in encoded_operands:
            _fail(decl, f"operand {binding!r} is encoded by multiple fields")
        operand = operands[binding]
        if operand["kind"] == "reg":
            _, last = operand.get("register_range", (0, registers["count"] - 1))
            if signed or last > field_mask:
                _fail(decl, f"field {field_name!r} cannot encode register operand {binding!r}")
            operand["width"] = field_width
        elif operand["width"] != field_width or operand["signed"] != signed:
            _fail(decl, f"field {field_name!r} does not match the width and signedness "
                        f"of immediate operand {binding!r}")
        encoded_operands.add(binding)
        variable_fields.append({"operand": binding, "lsb": lsb, "width": field_width})

    if operands.keys() - encoded_operands:
        missing = ", ".join(sorted(operands.keys() - encoded_operands))
        _fail(decl, f"missing encoding for operand(s): {missing}")

    template, mnemonic = _assembly(decl, operands)
    operands = list(operands.values())
    semantics = decl["semantics"]
    program = None if semantics is None else compile_semantics(
        semantics["source"], semantics["loc"], operands,
        registers["width"] if registers is not None else address_width,
        resolver.resolved, parameters=resolver.parameters, state=state,
        address_width=address_width, endian=endian,
    )
    return {
        "id": f"{architecture}.{name}", "name": name, "width": width, "mask": mask,
        "value": value, "operands": operands, "fields": variable_fields,
        "assembly": template, "semantics": program,
    }, mnemonic


def compile_file(path, *, parameters=None, features=None) -> dict:
    """Compile an architecture and its relative imports into a JSON-ready ASIR.

    Modules contribute unqualified names to one architecture-wide namespace.
    Parameter values select from declared domains; an explicit feature set
    replaces the defaults and must explicitly list all prerequisites. Template
    instances are validated and emitted like ordinary instructions.
    """
    root, declarations = _load(path)
    groups = _collect(declarations)
    parameters = _parameters(groups["parameter"], parameters, root)
    enabled = _features(groups["feature"], features, root)
    width, endian = _settings(groups["setting"], parameters, root)
    resolver = _Types(groups["type"], parameters)
    types = resolver.resolve_all()
    registers = _registers(groups["register_file"], groups["register_class"], parameters)
    state = _machine_state(
        groups["state"], groups["stack"], groups["memory"], resolver, parameters, width,
    )
    abi_section = "abi_facts_v2" if groups["abi_facts_v2"] else "abi_facts"
    if groups["abi_facts"] and groups["abi_facts_v2"]:
        _fail(next(iter(groups["abi_facts_v2"].values())),
              "an architecture cannot declare both abi_facts and abi_facts_v2")
    abi_facts = _abi_facts(abi_section, groups[abi_section], registers, state, width)
    formats = _formats(groups["format"], resolver, parameters)

    instructions = []
    mnemonics = {}
    for decl in elaborate_instructions(groups, max_width=_MAX_WIDTH):
        instance = decl.get("instantiation")
        try:
            required = _requirements(decl, groups["feature"])
            instruction, mnemonic = _instruction(
                decl, root["name"], formats, groups["register_class"], registers,
                state, width, endian, resolver,
            )
        except ArchonError as exc:
            if instance is None:
                raise
            raise ArchonError(
                f"{exc.message} (instantiating {decl['name']!r} at {instance['loc']})",
                exc.location,
            ) from None
        if not required <= enabled:
            continue
        site = instance if instance is not None else decl
        if instructions and instruction["width"] != instructions[0]["width"]:
            _fail(site, "mixed instruction widths are not supported for enabled instructions")
        if mnemonic in mnemonics:
            _fail(site, f"duplicate assembly mnemonic {mnemonic!r} "
                        f"in instructions {mnemonics[mnemonic]!r} and {decl['name']!r}")
        for other in instructions:
            common = instruction["mask"] & other["mask"]
            if ((instruction["value"] ^ other["value"]) & common) == 0:
                _fail(site, f"overlapping encodings for instructions "
                            f"{other['name']!r} and {decl['name']!r}")
        mnemonics[mnemonic] = decl["name"]
        instructions.append(instruction)

    db = {
        "version": 1, "architecture": root["name"], "address_width": width, "endian": endian,
        "parameters": parameters, "features": sorted(enabled), "types": types,
        "registers": registers, "state": state, "instructions": instructions,
    }
    if abi_facts is not None:
        db[abi_section] = abi_facts
    return db
