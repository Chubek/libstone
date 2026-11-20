"""Table-driven assembly, encoding and execution of compiled Archon targets."""

from dataclasses import dataclass
import re
from string import Formatter

from .database import _operand_register_range
from .diagnostics import ArchonError, Location
from .semantics import (
    _KINDS, _MAX_MEMORY_BYTES, _MAX_STACK_CAPACITY,
    execute_machine_semantics,
)


# Match the frontend's bounds for ASIR values used in shifts and allocations.
_MAX_WIDTH = 4096
_MAX_REGISTERS = 65536
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_MNEMONIC = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*")
_DECIMAL = r"[0-9]+"
_MAGNITUDE = rf"(?:0[xX][0-9a-fA-F]+|0[bB][01]+|0[oO][0-7]+|{_DECIMAL})"
_IMMEDIATE = rf"[+-]?{_MAGNITUDE}"
_LABEL_DEFINITION = re.compile(rf"({_IDENTIFIER.pattern}):[ \t]*")
_SYMBOL = rf"@?{_IDENTIFIER.pattern}(?:[ \t]*[+-][ \t]*{_MAGNITUDE})?"
_SYMBOL_PARTS = re.compile(rf"(@?)({_IDENTIFIER.pattern})(?:[ \t]*([+-])[ \t]*({_MAGNITUDE}))?")
_PUNCTUATION = ",()[]"
_LITERAL_PART = re.compile(r"[ \t]+|[,()\[\]]|[^ \t,()\[\]]+")


def _invalid(message):
    return ArchonError(f"invalid ArchDB: {message}")


def _object(value, label):
    if not isinstance(value, dict):
        raise _invalid(f"{label} must be an object")
    return value


def _array(value, label):
    if not isinstance(value, list):
        raise _invalid(f"{label} must be an array")
    return value


def _text(value, label, *, allow_empty=False):
    if not isinstance(value, str) or (not allow_empty and not value):
        raise _invalid(f"{label} must be a {'string' if allow_empty else 'nonempty string'}")
    return value


def _uint(value, label, *, minimum=0, maximum=None):
    if type(value) is not int or value < minimum or (maximum is not None and value > maximum):
        raise _invalid(f"{label} is out of range or not an integer")
    return value


def _fits(value: int, width: int, signed: bool) -> bool:
    if signed:
        return (value if value >= 0 else ~value).bit_length() < width
    return value >= 0 and value.bit_length() <= width


def _assembly_line(text: str) -> bool:
    return all(c == "\t" or c.isprintable() for c in text)


def _register_spelling(text: str) -> bool:
    return all(c.isprintable() and not c.isspace() and c not in ",{}()" for c in text)


def _base_address(value: int) -> None:
    if type(value) is not int or value < 0:
        raise ArchonError("base address must be a nonnegative integer")


@dataclass(frozen=True)
class _Operand:
    name: str
    kind: str
    width: int
    signed: bool
    register_range: tuple[int, int] | None
    lsb: int

    @property
    def mask(self) -> int:
        return (1 << self.width) - 1


@dataclass(frozen=True)
class _Instruction:
    name: str
    mask: int
    value: int
    operands: dict[str, _Operand]
    template: str
    placeholders: tuple[str, ...]
    pattern: re.Pattern[str]
    program_pattern: re.Pattern[str]
    semantics: dict | None


@dataclass
class MachineState:
    """Concrete architectural state accepted and returned by :class:`Target`.

    Buffers and collections are intentionally inspectable. Execution validates
    and copies every component, so mutating either the input or returned state
    never changes another state object as a side effect.
    """

    registers: list[int]
    scalars: dict[str, int]
    stacks: dict[str, list[int]]
    memories: dict[str, bytearray | bytes]

    def copy(self) -> "MachineState":
        return MachineState(
            self.registers.copy(),
            self.scalars.copy(),
            {name: values.copy() for name, values in self.stacks.items()},
            {name: bytearray(values) for name, values in self.memories.items()},
        )


@dataclass(frozen=True)
class _ValueType:
    kind: str
    width: int

    @property
    def mask(self) -> int:
        return (1 << self.width) - 1


@dataclass(frozen=True)
class _Stack:
    element_type: _ValueType
    capacity: int


def _compile_assembly(template, operands, name, reg_pattern):
    """Compile a format template, keeping mnemonic separators mandatory."""
    template = template.strip(" \t")
    if not _assembly_line(template):
        raise _invalid(f"{name} assembly must be one line")
    head = template.split(maxsplit=1)
    if not head or not _MNEMONIC.fullmatch(head[0]):
        raise _invalid(f"{name} assembly must start with a mnemonic")
    try:
        parts = list(Formatter().parse(head[1] if len(head) == 2 else ""))
    except ValueError:
        raise _invalid(f"{name} has an invalid assembly template") from None

    pattern = [re.escape(head[0])]
    if len(head) == 2:
        pattern.append(r"[ \t]+")
    placeholders = []
    immediates = []
    space = False
    previous = None

    def append(kind, expression):
        nonlocal space, previous
        if previous is not None:
            if kind == "punctuation" or previous == "punctuation":
                pattern.append(r"[ \t]*")
            elif space:
                pattern.append(r"[ \t]+")
            elif kind == previous == "operand":
                raise _invalid(f"{name} assembly operands must have a separator")
        pattern.append(expression)
        space = False
        previous = kind

    for literal, field, spec, conversion in parts:
        for part in _LITERAL_PART.findall(literal):
            if part[0] in " \t":
                space = True
            else:
                kind = "punctuation" if part in _PUNCTUATION else "literal"
                append(kind, re.escape(part))
        if field is not None:
            if field not in operands or spec or conversion is not None:
                raise _invalid(f"{name} has an invalid assembly placeholder {field!r}")
            if field in placeholders:
                raise _invalid(f"{name} repeats assembly placeholder {field!r}")
            placeholders.append(field)
            expression = reg_pattern if operands[field].kind == "reg" else _IMMEDIATE
            append("operand", f"({expression})")
            if operands[field].kind == "imm":
                immediates.append(len(pattern) - 1)
    if set(placeholders) != operands.keys():
        raise _invalid(f"{name} assembly does not name every operand")

    single = re.compile("".join(pattern))
    for index in immediates:
        pattern[index] = f"({_IMMEDIATE}|{_SYMBOL})"
    # Match comments after the template, so a literal '#' in assembly syntax
    # (e.g. an immediate prefix) is not mistaken for a comment.
    pattern.append(r"[ \t]*(?:#.*)?")
    return tuple(placeholders), single, re.compile("".join(pattern))


class Target:
    """Load an ASIR database for a single, fixed-width instruction set."""

    def __init__(self, db: dict):
        db = _object(db, "database")
        if type(db.get("version")) is not int or db["version"] != 1:
            raise _invalid("unsupported version (expected 1)")
        raw_address_width = db.get("address_width")
        self._address_width = (
            None
            if raw_address_width is None
            else _uint(raw_address_width, "address width", minimum=1, maximum=_MAX_WIDTH)
        )
        self._endian = db.get("endian")
        if self._endian not in ("little", "big"):
            raise _invalid("endian must be 'little' or 'big'")

        if "registers" not in db:
            raise _invalid("missing registers")
        registers = db["registers"]
        if registers is None:
            if self._address_width is None:
                raise _invalid("address width is required without a register file")
            self._register_count = 0
            self._register_width = self._address_width
            self._zero = None
            self._prefix = ""
            self._aliases = {}
            reg_pattern = r"(?!)"
        else:
            registers = _object(registers, "registers")
            self._register_count = _uint(registers.get("count"), "register count",
                                         minimum=1, maximum=_MAX_REGISTERS)
            self._register_width = _uint(registers.get("width"), "register width",
                                         minimum=1, maximum=_MAX_WIDTH)
            # Early schema-v1 runtime tables omitted address_width. They had
            # only register semantics, so the register width is the compatible
            # default at this raw-Target boundary.
            if self._address_width is None:
                self._address_width = self._register_width
            self._zero = registers.get("zero")
            if self._zero is not None:
                _uint(self._zero, "zero register", maximum=self._register_count - 1)
            self._prefix = _text(registers.get("prefix"), "register prefix", allow_empty=True)
            if not _register_spelling(self._prefix):
                raise _invalid("register prefix contains whitespace or an assembly delimiter")
            self._aliases = {}
            for alias, index in _object(registers.get("aliases"), "register aliases").items():
                _text(alias, "register alias")
                if not _register_spelling(alias):
                    raise _invalid(
                        f"register alias {alias!r} contains whitespace or an assembly delimiter"
                    )
                index = _uint(
                    index, f"register alias {alias!r}", maximum=self._register_count - 1,
                )
                if re.fullmatch(re.escape(self._prefix) + _DECIMAL, alias):
                    suffix = alias[len(self._prefix):].lstrip("0") or "0"
                    if suffix != str(index):
                        raise _invalid(
                            f"register alias {alias!r} conflicts with a numbered register"
                        )
                self._aliases[alias] = index
            spellings = [re.escape(alias) for alias in self._aliases]
            spellings.append(re.escape(self._prefix) + _DECIMAL)
            reg_pattern = "(?:" + "|".join(spellings) + ")"

        self._load_state(db.get("state", {
            "scalars": [], "stacks": [], "memories": [],
        }))

        self._instructions: dict[str, _Instruction] = {}
        self._size: int | None = None
        for entry in _array(db.get("instructions"), "instructions"):
            inst = self._load_instruction(entry, reg_pattern)
            if inst.name in self._instructions:
                raise _invalid(f"duplicate instruction {inst.name!r}")
            for other in self._instructions.values():
                if (inst.value ^ other.value) & (inst.mask & other.mask) == 0:
                    raise _invalid(f"overlapping encodings for {other.name} and {inst.name}")
            self._instructions[inst.name] = inst

    def _value_type(self, raw, label):
        raw = _object(raw, label)
        kind = raw.get("kind")
        if kind not in _KINDS:
            raise _invalid(f"{label} has an invalid kind")
        width = _uint(raw.get("width"), f"{label} width", minimum=1, maximum=_MAX_WIDTH)
        return _ValueType(kind, width)

    def _load_state(self, raw):
        raw = _object(raw, "state")
        self._scalars: dict[str, _ValueType] = {}
        self._stacks: dict[str, _Stack] = {}
        self._memories: dict[str, int] = {}
        names = set()

        def resource_name(row, label):
            name = _text(row.get("name"), f"{label} name")
            if not _IDENTIFIER.fullmatch(name):
                raise _invalid(f"{label} name must be an identifier")
            if name in names:
                raise _invalid(f"duplicate architectural state name {name!r}")
            names.add(name)
            return name

        for index, item in enumerate(_array(raw.get("scalars"), "state scalars")):
            label = f"state scalar {index}"
            row = _object(item, label)
            name = resource_name(row, label)
            self._scalars[name] = self._value_type(row.get("type"), f"{label} type")
        for index, item in enumerate(_array(raw.get("stacks"), "state stacks")):
            label = f"state stack {index}"
            row = _object(item, label)
            name = resource_name(row, label)
            element = self._value_type(row.get("element_type"), f"{label} element type")
            capacity = _uint(
                row.get("capacity"), f"{label} capacity",
                minimum=1, maximum=_MAX_STACK_CAPACITY,
            )
            self._stacks[name] = _Stack(element, capacity)
        for index, item in enumerate(_array(raw.get("memories"), "state memories")):
            label = f"state memory {index}"
            row = _object(item, label)
            name = resource_name(row, label)
            size = _uint(
                row.get("size"), f"{label} size", minimum=1, maximum=_MAX_MEMORY_BYTES,
            )
            if size > 1 << self._address_width:
                raise _invalid(
                    f"{label} size exceeds the {self._address_width}-bit address space"
                )
            self._memories[name] = size
        self._state_ir = {
            "scalars": [
                {"name": name, "type": {"kind": ty.kind, "width": ty.width}}
                for name, ty in self._scalars.items()
            ],
            "stacks": [
                {
                    "name": name,
                    "element_type": {
                        "kind": stack.element_type.kind,
                        "width": stack.element_type.width,
                    },
                    "capacity": stack.capacity,
                }
                for name, stack in self._stacks.items()
            ],
            "memories": [
                {"name": name, "size": size} for name, size in self._memories.items()
            ],
        }

    def _load_instruction(self, entry, reg_pattern):
        entry = _object(entry, "instruction")
        name = _text(entry.get("name"), "instruction name")
        width = _uint(entry.get("width"), f"{name} encoding width", minimum=1, maximum=_MAX_WIDTH)
        size, remainder = divmod(width, 8)
        if remainder:
            raise _invalid(f"{name} encoding width must be a multiple of 8 bits")
        if self._size is not None and size != self._size:
            raise _invalid("mixed instruction widths are not supported")
        self._size = size

        mask = _uint(entry.get("mask"), f"{name} mask")
        value = _uint(entry.get("value"), f"{name} value")
        if mask.bit_length() > width or value & ~mask:
            raise _invalid(f"{name} mask and value do not describe fixed bits within its encoding")

        descriptors = {}
        for raw in _array(entry.get("operands"), f"{name} operands"):
            raw = _object(raw, f"{name} operand")
            operand = _text(raw.get("name"), f"{name} operand name")
            if not _IDENTIFIER.fullmatch(operand):
                raise _invalid(f"{name} operand {operand!r} is not an identifier")
            if operand in descriptors:
                raise _invalid(f"{name} has duplicate operand {operand!r}")
            kind = raw.get("kind")
            signed = raw.get("signed")
            if kind not in ("reg", "imm") or type(signed) is not bool or (kind == "reg" and signed):
                raise _invalid(f"{name} operand {operand!r} has an invalid kind or signedness")
            if kind == "reg" and self._register_count == 0:
                raise _invalid(f"{name} has a register operand but no register file")
            direction = raw.get("direction")
            if direction not in ("in", "out", "inout") or (kind == "imm" and direction != "in"):
                raise _invalid(f"{name} operand {operand!r} has an invalid direction")
            operand_width = _uint(raw.get("width"), f"{name} operand {operand!r} width",
                                  minimum=1, maximum=_MAX_WIDTH)
            bounds = _operand_register_range(
                raw, self._register_count, operand_width, f"{name} operand {operand!r}",
            )
            descriptors[operand] = (kind, operand_width, signed, bounds)

        used = mask
        fields = {}
        for raw in _array(entry.get("fields"), f"{name} fields"):
            raw = _object(raw, f"{name} field")
            operand = _text(raw.get("operand"), f"{name} field operand")
            if operand not in descriptors or operand in fields:
                raise _invalid(f"{name} has an unknown or duplicate field for {operand!r}")
            lsb = _uint(raw.get("lsb"), f"{name} field {operand!r} lsb", maximum=width - 1)
            field_width = _uint(raw.get("width"), f"{name} field {operand!r} width",
                                minimum=1, maximum=width - lsb)
            if field_width != descriptors[operand][1]:
                raise _invalid(f"{name} field {operand!r} width differs from its operand")
            bits = ((1 << field_width) - 1) << lsb
            if used & bits:
                raise _invalid(f"{name} field {operand!r} overlaps another field or fixed bits")
            used |= bits
            fields[operand] = lsb
        if fields.keys() != descriptors.keys():
            raise _invalid(f"{name} needs an encoding field for every operand")
        if used.bit_count() != width:
            raise _invalid(f"{name} fields and mask do not cover its entire encoding")
        operands = {op: _Operand(op, *props, fields[op]) for op, props in descriptors.items()}

        template = _text(entry.get("assembly"), f"{name} assembly")
        placeholders, pattern, program_pattern = _compile_assembly(template, operands, name, reg_pattern)
        semantics = entry.get("semantics")
        if semantics is not None:
            _object(semantics, f"{name} semantics")
        return _Instruction(name, mask, value, operands, template, placeholders, pattern, program_pattern, semantics)

    def _instruction(self, name):
        if not isinstance(name, str) or name not in self._instructions:
            raise ArchonError(f"unknown instruction {name!r}")
        return self._instructions[name]

    def _validate_operands(self, inst, values):
        if not isinstance(values, dict) or values.keys() != inst.operands.keys():
            names = ", ".join(inst.operands) or "none"
            raise ArchonError(f"{inst.name} expects operands: {names}")
        for operand in inst.operands.values():
            value = values[operand.name]
            label = f"{inst.name} operand {operand.name!r}"
            if type(value) is not int:
                raise ArchonError(f"{label} must be an integer")
            if operand.register_range is not None:
                first, last = operand.register_range
                if not 0 <= value < self._register_count:
                    raise ArchonError(f"{label} is not a valid register index")
                if not first <= value <= last:
                    raise ArchonError(f"{label} is outside its register class range [{first}..{last}]")
            if not _fits(value, operand.width, operand.signed):
                signedness = "signed" if operand.signed else "unsigned"
                raise ArchonError(f"{label} does not fit in {operand.width} {signedness} bits")

    def _parse_operand(self, operand, text, labels, address):
        try:
            if operand.kind == "reg":
                if text in self._aliases:
                    return self._aliases[text]
                return int(text[len(self._prefix):], 10)
            symbol = _SYMBOL_PARTS.fullmatch(text) if labels is not None else None
            if symbol is not None:
                relative, label, sign, addend = symbol.groups()
                if label not in labels:
                    raise ArchonError(f"unknown label {label!r}")
                value = labels[label][0]
                if relative:
                    value -= address
                if sign is None:
                    return value
                text = sign + addend
            else:
                value = 0
            digits = text.lstrip("+-")
            base = 0 if digits[:2].lower() in ("0x", "0b", "0o") else 10
            return value + int(text, base)
        except ValueError:
            raise ArchonError(f"invalid integer for operand {operand.name!r}") from None

    def assemble(self, text: str) -> tuple[str, dict[str, int]]:
        if not isinstance(text, str) or not _assembly_line(text):
            raise ArchonError("assembly input must be a single line of text")
        return self._assemble_line(text)

    def _assemble_line(self, text, *, labels=None, address=0):
        if not self._instructions:
            raise ArchonError("target has no enabled instructions")
        text = text.strip(" \t")
        result = None
        error = None
        for inst in self._instructions.values():
            pattern = inst.pattern if labels is None else inst.program_pattern
            match = pattern.fullmatch(text)
            if match is None:
                continue
            try:
                values = {op: self._parse_operand(inst.operands[op], value, labels, address)
                          for op, value in zip(inst.placeholders, match.groups())}
                self._validate_operands(inst, values)
            except ArchonError as exc:
                error = exc
                continue
            if result is not None:
                raise ArchonError(f"ambiguous assembly: both {result[0]} and {inst.name} match")
            result = inst.name, values
        if result is not None:
            return result
        if error is not None:
            raise error
        raise ArchonError("assembly does not match any instruction")

    def assemble_program(
        self, text: str, *, base_address: int = 0, filename: str = "<assembly>"
    ) -> bytes:
        """Assemble a source file to raw bytes, resolving local labels in immediates.

        Blank lines and ``#`` comments are ignored. Case-sensitive ``label:``
        definitions may stand alone or precede an instruction.
        ``label`` denotes its absolute byte address; ``@label`` subtracts the
        address of the containing instruction. Either may have a single
        ``+ number`` or ``- number`` addend. A label at EOF denotes the byte
        address immediately after the last instruction.
        """
        _base_address(base_address)
        if not isinstance(text, str):
            raise ArchonError("assembly program must be text")
        labels = {}
        instructions = []
        next_address = base_address
        # Fixed width lets us record every address before resolving operands.
        for line_number, line in enumerate(re.split(r"\r\n?|\n", text), 1):
            pos = len(line) - len(line.lstrip(" \t"))
            while (match := _LABEL_DEFINITION.match(line, pos)) is not None:
                label = match[1]
                if label in labels:
                    raise ArchonError(
                        f"duplicate label {label!r} (first defined on line {labels[label][1]})",
                        Location(filename, line_number, pos + 1),
                    )
                labels[label] = next_address, line_number
                pos = match.end()
            if pos == len(line) or line[pos] == "#":
                continue
            location = Location(filename, line_number, pos + 1)
            if self._size is None:
                raise ArchonError("target has no enabled instructions", location)
            instructions.append((location, next_address, line[pos:]))
            next_address += self._size

        result = bytearray()
        for location, address, instruction in instructions:
            try:
                name, operands = self._assemble_line(instruction, labels=labels, address=address)
                result.extend(self.encode(name, operands))
            except ArchonError as exc:
                raise ArchonError(exc.message, location) from exc
        return bytes(result)

    def encode(self, name: str, operands: dict[str, int]) -> bytes:
        inst = self._instruction(name)
        self._validate_operands(inst, operands)
        word = inst.value
        for operand in inst.operands.values():
            field = operands[operand.name] & operand.mask
            word |= field << operand.lsb
        return word.to_bytes(self._size, self._endian)

    def decode(self, data: bytes) -> tuple[str, dict[str, int]]:
        if not isinstance(data, bytes):
            raise ArchonError("encoded instruction must be bytes")
        if self._size is None:
            raise ArchonError("target has no enabled instructions")
        if len(data) != self._size:
            raise ArchonError(f"expected {self._size} instruction bytes, got {len(data)}")
        word = int.from_bytes(data, self._endian)
        for inst in self._instructions.values():
            if word & inst.mask != inst.value:
                continue
            values = {}
            for operand in inst.operands.values():
                field = (word >> operand.lsb) & operand.mask
                if operand.signed and field >> (operand.width - 1):
                    field -= 1 << operand.width
                values[operand.name] = field
            self._validate_operands(inst, values)
            return inst.name, values
        raise ArchonError("unknown instruction encoding")

    def disassemble(self, data: bytes) -> str:
        name, values = self.decode(data)
        inst = self._instructions[name]
        return inst.template.format_map({
            op: f"{self._prefix}{value}" if inst.operands[op].kind == "reg" else value
            for op, value in values.items()
        })

    def _program_chunks(self, data, base_address):
        _base_address(base_address)
        if not isinstance(data, bytes):
            raise ArchonError("encoded program must be bytes")
        if not data:
            return
        size = self._size
        if size is None:
            raise ArchonError("target has no enabled instructions")
        if remainder := len(data) % size:
            address = base_address + len(data) - remainder
            raise ArchonError(f"at byte address 0x{address:x}: truncated instruction: "
                              f"expected {size} bytes, got {remainder}")
        for offset in range(0, len(data), size):
            yield base_address + offset, data[offset:offset + size]

    def disassemble_program(self, data: bytes, *, base_address: int = 0) -> str:
        """Return canonical numeric assembly with one newline per instruction."""
        lines = []
        for address, chunk in self._program_chunks(data, base_address):
            try:
                lines.append(self.disassemble(chunk))
            except ArchonError as exc:
                raise ArchonError(f"at byte address 0x{address:x}: {exc}") from exc
        return "".join(line + "\n" for line in lines)

    def new_state(
        self, *,
        registers=None,
        scalars=None,
        stacks=None,
        memories=None,
    ) -> MachineState:
        """Create validated machine state, applying optional named overrides.

        Scalar, stack, and memory mappings may omit names; omitted components
        are zero, empty, and zero-filled respectively. A supplied memory buffer
        must have exactly the declared byte size.
        """
        register_values = (
            [0] * self._register_count
            if registers is None
            else registers.copy() if isinstance(registers, list) else registers
        )
        scalar_values = {name: 0 for name in self._scalars}
        stack_values = {name: [] for name in self._stacks}
        memory_values = {
            name: bytearray(size) for name, size in self._memories.items()
        }

        def overrides(values, supplied, label):
            if supplied is None:
                return
            if not isinstance(supplied, dict):
                raise ArchonError(f"{label} overrides must be a mapping")
            unknown = supplied.keys() - values.keys()
            if unknown:
                raise ArchonError(f"unknown {label} {sorted(unknown)[0]!r}")
            for key, value in supplied.items():
                values[key] = value

        overrides(scalar_values, scalars, "scalar state")
        overrides(stack_values, stacks, "stack")
        overrides(memory_values, memories, "memory")
        result = MachineState(register_values, scalar_values, stack_values, memory_values)
        self._validate_machine_state(result)
        return result.copy()

    def _validate_machine_state(self, state):
        if not isinstance(state, MachineState):
            raise ArchonError("expected a MachineState")
        self._validate_registers(state.registers)
        if not isinstance(state.scalars, dict) or state.scalars.keys() != self._scalars.keys():
            raise ArchonError("machine scalar-state names do not match the architecture")
        for name, ty in self._scalars.items():
            value = state.scalars[name]
            if type(value) is not int or not 0 <= value <= ty.mask:
                raise ArchonError(f"invalid value for scalar state {name!r}")
        if not isinstance(state.stacks, dict) or state.stacks.keys() != self._stacks.keys():
            raise ArchonError("machine stack names do not match the architecture")
        for name, descriptor in self._stacks.items():
            values = state.stacks[name]
            if not isinstance(values, list) or len(values) > descriptor.capacity:
                raise ArchonError(f"invalid depth for stack {name!r}")
            if any(
                type(value) is not int
                or not 0 <= value <= descriptor.element_type.mask
                for value in values
            ):
                raise ArchonError(f"invalid value in stack {name!r}")
        if not isinstance(state.memories, dict) or state.memories.keys() != self._memories.keys():
            raise ArchonError("machine memory names do not match the architecture")
        for name, size in self._memories.items():
            value = state.memories[name]
            if not isinstance(value, (bytes, bytearray)) or len(value) != size:
                raise ArchonError(f"memory {name!r} must contain exactly {size} bytes")

    def execute_state(
        self, name: str, operands: dict[str, int], state: MachineState,
    ) -> MachineState:
        """Execute one instruction transactionally against full machine state."""
        inst = self._instruction(name)
        self._validate_operands(inst, operands)
        if inst.semantics is None:
            raise ArchonError(f"{name} has no executable semantics")
        self._validate_machine_state(state)
        try:
            result = execute_machine_semantics(
                inst.semantics,
                operands,
                {
                    "registers": state.registers,
                    "scalars": state.scalars,
                    "stacks": state.stacks,
                    "memories": state.memories,
                },
                self._register_width,
                self._zero,
                self._state_ir,
                self._address_width,
                self._endian,
            )
        except (KeyError, TypeError, ValueError, IndexError) as exc:
            raise _invalid(f"{name} has malformed executable semantics") from exc
        return MachineState(
            result["registers"], result["scalars"], result["stacks"], result["memories"],
        )

    def execute(self, name: str, operands: dict[str, int], registers: list[int]) -> list[int]:
        """Backward-compatible register-list execution wrapper."""
        state = self.new_state(registers=registers)
        return self.execute_state(name, operands, state).registers

    def _validate_registers(self, registers):
        if not isinstance(registers, list) or len(registers) != self._register_count:
            raise ArchonError(f"expected a list of {self._register_count} registers")
        for index, value in enumerate(registers):
            if type(value) is not int or not _fits(value, self._register_width, False):
                raise ArchonError(f"invalid value for register {self._prefix}{index}")

    def execute_program(self, data: bytes, registers: list[int], *, base_address: int = 0) -> list[int]:
        """Backward-compatible register-list wrapper for linear program execution."""
        state = self.new_state(registers=registers)
        return self.execute_program_state(data, state, base_address=base_address).registers

    def execute_program_state(
        self, data: bytes, state: MachineState, *, base_address: int = 0,
    ) -> MachineState:
        """Execute encoded instructions in file order against full machine state.

        Program layout remains linear; PC state has no implicit fetch or
        increment behavior. Instructions may update a declared PC explicitly.
        """
        self._validate_machine_state(state)
        result = state.copy()
        for address, chunk in self._program_chunks(data, base_address):
            try:
                name, operands = self.decode(chunk)
                result = self.execute_state(name, operands, result)
            except ArchonError as exc:
                raise ArchonError(f"at byte address 0x{address:x}: {exc}") from exc
        return result
