"""Lower validated SemIR to standalone C99 instruction bodies."""

import re

from .diagnostics import ArchonError


_BINARY_OPERATORS = {"+%": "+", "-%": "-", "*%": "*", "&": "&", "|": "|", "^": "^"}
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def _uint(value: int) -> str:
    return f"UINTMAX_C(0x{value:x})"


def _empty_state() -> dict:
    return {"scalars": [], "stacks": [], "memories": []}


def lower_instruction(
    inst: dict,
    prefix: str,
    index: int,
    word_width: int,
    zero: int | None,
    *,
    state: dict | None = None,
    endian: str = "little",
) -> str:
    """Emit a static semantic function for one validated instruction.

    The original two-argument C function is retained when ``state`` is omitted,
    which keeps this helper compatible with register-only callers.  The emulator
    generator passes even an empty state model and receives a function taking a
    third, private ``struct <prefix>_emu_state *`` argument.
    """
    if word_width > 64:
        width_kind = "register" if state is None else "semantic word"
        raise ArchonError(
            f"C99 emulator cannot lower {inst['name']!r}: {width_kind} width "
            f"{word_width} exceeds 64 bits"
        )
    if endian not in {"little", "big"}:
        raise ArchonError(f"C99 emulator cannot lower {inst['name']!r}: invalid endianness")
    stateful_signature = state is not None
    lowerer = _Lowerer(inst, prefix, zero, state or _empty_state(), endian)
    parameters = "const uint64_t *operands, uint64_t *regs"
    if stateful_signature:
        parameters += f", struct {prefix}_emu_state *state"
    lowerer.lines.append(
        f"static enum {prefix}_emu_status {prefix}_emu_sem_{index}({parameters}) {{"
    )
    # Empty programs and immediate-only lets need not use every parameter.
    lowerer.emit("(void)operands;")
    lowerer.emit("(void)regs;")
    if stateful_signature:
        lowerer.emit("(void)state;")
    lowerer.block(inst["semantics"]["statements"])
    lowerer.emit(f"return {lowerer.status}_OK;")
    lowerer.lines.append("}")
    return "\n".join(lowerer.lines) + "\n"


class _Lowerer:
    def __init__(self, inst: dict, prefix: str, zero: int | None, state: dict, endian: str):
        self.name = inst["name"]
        self.status = f"{prefix.upper()}_EMU"
        self.zero = zero
        self.endian = endian
        self.operands = {operand["name"]: index for index, operand in enumerate(inst["operands"])}
        self.scalars = self._resources(state, "scalars")
        self.stacks = self._resources(state, "stacks")
        self.memories = self._resources(state, "memories")
        self.locals: dict[str, str] = {}
        self.lines: list[str] = []
        self.indent = 1
        self.next_temp = 0

    def _resources(self, state: dict, key: str) -> dict[str, tuple[int, dict]]:
        try:
            rows = state[key]
        except (KeyError, TypeError):
            raise ArchonError(
                f"C99 emulator cannot lower {self.name!r}: malformed architectural state"
            ) from None
        result = {}
        for index, row in enumerate(rows):
            try:
                name = row["name"]
            except (KeyError, TypeError):
                raise ArchonError(
                    f"C99 emulator cannot lower {self.name!r}: malformed architectural state"
                ) from None
            if not isinstance(name, str) or not _IDENTIFIER.fullmatch(name) or name in result:
                raise ArchonError(
                    f"C99 emulator cannot lower {self.name!r}: invalid {key} name {name!r}"
                )
            result[name] = (index, row)
        return result

    def resource(self, resources: dict, name: str, kind: str) -> tuple[int, dict]:
        try:
            return resources[name]
        except KeyError:
            raise ArchonError(
                f"C99 emulator cannot lower {self.name!r}: unknown {kind} {name!r}"
            ) from None

    def fresh(self, stem: str) -> str:
        name = f"{stem}{self.next_temp}"
        self.next_temp += 1
        return name

    def emit(self, line: str) -> None:
        self.lines.append("    " * self.indent + line)

    def open(self, header: str = "") -> None:
        self.emit(f"{header} {{" if header else "{")
        self.indent += 1

    def close(self) -> None:
        self.indent -= 1
        self.emit("}")

    def operand(self, name: str) -> str:
        return f"operands[{self.operands[name]}]"

    @staticmethod
    def scalar_member(name: str) -> str:
        return f"state->scalar_{name}"

    @staticmethod
    def stack_member(name: str) -> str:
        return f"state->stack_{name}"

    @staticmethod
    def memory_member(name: str) -> str:
        return f"state->memory_{name}"

    def expression(self, expr: dict) -> str:
        op = expr["op"]
        width = expr["type"]["width"]
        if width > 64:
            raise ArchonError(
                f"C99 emulator cannot lower {self.name!r}: SemIR {op!r} width "
                f"{width} exceeds 64 bits"
            )
        mask = (1 << width) - 1
        if op == "local":
            return self.locals[expr["name"]]
        if op == "constant":
            value = _uint(expr["value"] & mask)
        elif op == "read":
            value = f"regs[{self.operand(expr['name'])}]"
        elif op == "immediate":
            value = self.operand(expr["name"])
        elif op == "read_state":
            self.resource(self.scalars, expr["name"], "scalar state")
            value = self.scalar_member(expr["name"])
        elif op in {"peek", "pop"}:
            _, row = self.resource(self.stacks, expr["name"], "stack")
            stack = self.stack_member(expr["name"])
            capacity = row["capacity"]
            if op == "peek":
                depth = expr["depth"]
                if type(depth) is not int or depth < 0:
                    raise ArchonError(
                        f"C99 emulator cannot lower {self.name!r}: invalid stack depth"
                    )
                if depth >= capacity:
                    self.emit(f"return {self.status}_STACK_UNDERFLOW;")
                    value = "UINTMAX_C(0)"
                else:
                    self.emit(f"if ({stack}.size <= {depth}u)")
                    self.emit(f"    return {self.status}_STACK_UNDERFLOW;")
                    value = f"{stack}.values[{stack}.size - {depth + 1}u]"
            else:
                self.emit(f"if ({stack}.size == 0)")
                self.emit(f"    return {self.status}_STACK_UNDERFLOW;")
                self.emit(f"--{stack}.size;")
                value = f"{stack}.values[{stack}.size]"
        elif op == "load":
            _, row = self.resource(self.memories, expr["name"], "memory")
            address = self.expression(expr["address"])
            byte_count = width // 8
            if width % 8 or byte_count == 0:
                raise ArchonError(
                    f"C99 emulator cannot lower {self.name!r}: memory load width "
                    f"{width} is not byte-aligned"
                )
            if byte_count > row["size"]:
                self.emit(f"return {self.status}_MEMORY_OUT_OF_BOUNDS;")
                value = "UINTMAX_C(0)"
            else:
                self.emit(
                    f"if ((uintmax_t){address} > UINTMAX_C({row['size'] - byte_count}))"
                )
                self.emit(f"    return {self.status}_MEMORY_OUT_OF_BOUNDS;")
                raw = self.fresh("loaded")
                byte = self.fresh("byte")
                self.emit(f"uintmax_t {raw} = UINTMAX_C(0);")
                self.open(f"for (size_t {byte} = 0; {byte} < {byte_count}u; ++{byte})")
                shift = (
                    f"8u * {byte}"
                    if self.endian == "little"
                    else f"8u * ({byte_count - 1}u - {byte})"
                )
                self.emit(
                    f"{raw} |= (uintmax_t){self.memory_member(expr['name'])}"
                    f"[(size_t){address} + {byte}] << ({shift});"
                )
                self.close()
                value = raw
        elif op in {"zext", "sext", "trunc", "~", "extract"}:
            arg = self.expression(expr["value"])
            if op == "zext":
                return arg
            if op == "sext":
                sign = _uint(1 << (expr["value"]["type"]["width"] - 1))
                value = f"({arg} ^ {sign}) - {sign}"
            elif op == "extract":
                value = f"{arg} >> {expr['low']}u"
            elif op == "~":
                value = f"~{arg}"
            else:  # trunc
                value = arg
        elif op in _BINARY_OPERATORS or op in {"concat", "<<", ">>"}:
            left = self.expression(expr["left"])
            right = self.expression(expr["right"])
            if op == "concat":
                value = f"({left} << {expr['right']['type']['width']}u) | {right}"
            elif op == ">>" and expr["type"]["kind"] == "sint":
                # Flip the sign bit, then shift the nonnegative part and the
                # sign bias separately. Subtraction is unsigned throughout.
                sign = _uint(1 << (width - 1))
                value = (
                    f"{right} < {width}u ? (({left} ^ {sign}) >> {right}) - ({sign} >> {right})"
                    f" : UINTMAX_C(0) - ({left} >> {width - 1}u)"
                )
            elif op in {"<<", ">>"}:
                value = f"{right} < {width}u ? {left} {op} {right} : UINTMAX_C(0)"
            else:
                value = f"{left} {_BINARY_OPERATORS[op]} {right}"
        else:
            raise ArchonError(f"C99 emulator cannot lower {self.name!r}: unknown SemIR operation {op!r}")

        # uintmax_t stays unsigned under integer promotion. Even if it is
        # wider than 64 bits, the mask recovers the exact SemIR bit vector.
        temp = self.fresh("v")
        self.emit(f"const uintmax_t {temp} = ({value}) & {_uint(mask)};")
        return temp

    def write_register(self, operand: str, value: str, stage_slot: int | None) -> None:
        if stage_slot is not None or self.zero is not None:
            self.open()
            self.emit(f"const uint64_t dst = {operand};")
            if self.zero is not None:
                self.open(f"if (dst != {_uint(self.zero)})")
        if stage_slot is not None:
            self.emit("size_t slot;")
            self.open("for (slot = 0; slot < pending_register_count; ++slot)")
            self.emit("if (pending_register[slot].active && pending_register[slot].index == dst)")
            self.emit("    break;")
            self.close()
            self.open("if (slot == pending_register_count)")
            self.emit(f"pending_register[{stage_slot}].index = dst;")
            self.emit(f"pending_register[{stage_slot}].value = (uint64_t){value};")
            self.emit(f"pending_register[{stage_slot}].active = 1;")
            self.close()
            self.open(f"else if (pending_register[slot].value != (uint64_t){value})")
            self.emit(f"return {self.status}_CONFLICTING_WRITES;")
            self.close()
        else:
            self.emit(f"regs[{'dst' if self.zero is not None else operand}] = (uint64_t){value};")
        if self.zero is not None:
            self.close()
        if stage_slot is not None or self.zero is not None:
            self.close()

    def write_state(self, name: str, value: str, stage_slot: int | None) -> None:
        self.resource(self.scalars, name, "scalar state")
        if stage_slot is None:
            self.emit(f"{self.scalar_member(name)} = (uint64_t){value};")
            return
        self.open(f"if (!pending_state[{stage_slot}].active)")
        self.emit(f"pending_state[{stage_slot}].value = (uint64_t){value};")
        self.emit(f"pending_state[{stage_slot}].active = 1;")
        self.close()
        self.open(f"else if (pending_state[{stage_slot}].value != (uint64_t){value})")
        self.emit(f"return {self.status}_CONFLICTING_WRITES;")
        self.close()

    def push(self, name: str, value: str) -> None:
        _, row = self.resource(self.stacks, name, "stack")
        stack = self.stack_member(name)
        self.emit(f"if ({stack}.size == {row['capacity']}u)")
        self.emit(f"    return {self.status}_STACK_OVERFLOW;")
        self.emit(f"{stack}.values[{stack}.size++] = (uint64_t){value};")

    def store(self, statement: dict, *, parallel: bool) -> None:
        name = statement["name"]
        memory_index, row = self.resource(self.memories, name, "memory")
        address = self.expression(statement["address"])
        value = self.expression(statement["value"])
        width = statement["value"]["type"]["width"]
        byte_count = width // 8
        if width % 8 or byte_count == 0:
            raise ArchonError(
                f"C99 emulator cannot lower {self.name!r}: memory store width "
                f"{width} is not byte-aligned"
            )
        if byte_count > row["size"]:
            self.emit(f"return {self.status}_MEMORY_OUT_OF_BOUNDS;")
            return
        self.emit(f"if ((uintmax_t){address} > UINTMAX_C({row['size'] - byte_count}))")
        self.emit(f"    return {self.status}_MEMORY_OUT_OF_BOUNDS;")
        byte = self.fresh("byte")
        self.open(f"for (size_t {byte} = 0; {byte} < {byte_count}u; ++{byte})")
        shift = (
            f"8u * {byte}"
            if self.endian == "little"
            else f"8u * ({byte_count - 1}u - {byte})"
        )
        octet = f"(uint8_t)((uintmax_t){value} >> ({shift}))"
        location = f"(size_t){address} + {byte}"
        if parallel:
            slot = self.fresh("store_slot")
            self.emit(f"size_t {slot};")
            self.open(f"for ({slot} = 0; {slot} < pending_memory_used; ++{slot})")
            self.emit(
                f"if (pending_memory[{slot}].resource == {memory_index}u && "
                f"pending_memory[{slot}].address == {location})"
            )
            self.emit("    break;")
            self.close()
            self.open(f"if ({slot} == pending_memory_used)")
            self.emit(f"pending_memory[{slot}].resource = {memory_index}u;")
            self.emit(f"pending_memory[{slot}].address = {location};")
            self.emit(f"pending_memory[{slot}].value = {octet};")
            self.emit(
                f"pending_memory[{slot}].memory = {self.memory_member(name)};"
            )
            self.emit("++pending_memory_used;")
            self.close()
            self.open(f"else if (pending_memory[{slot}].value != {octet})")
            self.emit(f"return {self.status}_CONFLICTING_WRITES;")
            self.close()
        else:
            self.emit(f"{self.memory_member(name)}[{location}] = {octet};")
        self.close()

    def block(self, statements: list[dict], *, parallel: bool = False) -> None:
        # Register destinations need dynamic alias coalescing. Scalar names
        # have fixed slots, while memory writes are staged byte by byte so
        # overlapping stores can coalesce or report a conflict.
        register_slots = {}
        state_slots = {}
        memory_capacity = 0
        if parallel:
            register_names = dict.fromkeys(
                stmt["name"] for stmt in statements if stmt["op"] == "write"
            )
            state_names = dict.fromkeys(
                stmt["name"] for stmt in statements if stmt["op"] == "write_state"
            )
            register_slots = {name: index for index, name in enumerate(register_names)}
            state_slots = {name: index for index, name in enumerate(state_names)}
            memory_capacity = sum(
                stmt["value"]["type"]["width"] // 8
                for stmt in statements
                if stmt["op"] == "store"
            )
        if register_slots:
            self.emit(f"enum {{ pending_register_count = {len(register_slots)} }};")
            self.emit(
                "struct { uint64_t index, value; int active; } "
                "pending_register[pending_register_count] = {{0, 0, 0}};"
            )
        if state_slots:
            self.emit(f"enum {{ pending_state_count = {len(state_slots)} }};")
            self.emit(
                "struct { uint64_t value; int active; } "
                "pending_state[pending_state_count] = {{0, 0}};"
            )
        if memory_capacity:
            self.emit(f"enum {{ pending_memory_capacity = {memory_capacity} }};")
            self.emit(
                "struct { uint8_t *memory, value; size_t address; unsigned resource; } "
                "pending_memory[pending_memory_capacity];"
            )
            self.emit("size_t pending_memory_used = 0;")

        bindings = []
        for stmt in statements:
            op = stmt["op"]
            if op in {"sequence", "parallel"}:
                self.open()
                self.block(stmt["statements"], parallel=op == "parallel")
                self.close()
            elif op == "let":
                value = self.expression(stmt["value"])
                self.locals[stmt["name"]] = value
                bindings.append(stmt["name"])
                self.emit(f"(void){value};")  # A let may have no uses.
            elif op == "write":
                value = self.expression(stmt["value"])
                self.write_register(
                    self.operand(stmt["name"]), value, register_slots.get(stmt["name"])
                )
            elif op == "write_state":
                value = self.expression(stmt["value"])
                self.write_state(stmt["name"], value, state_slots.get(stmt["name"]))
            elif op == "push":
                value = self.expression(stmt["value"])
                self.push(stmt["name"], value)
            elif op == "store":
                self.store(stmt, parallel=parallel)
            else:
                raise ArchonError(
                    f"C99 emulator cannot lower {self.name!r}: unknown SemIR statement {op!r}"
                )
        for name in bindings:
            del self.locals[name]

        if register_slots:
            self.open("for (size_t slot = 0; slot < pending_register_count; ++slot)")
            self.emit("if (pending_register[slot].active)")
            self.emit("    regs[pending_register[slot].index] = pending_register[slot].value;")
            self.close()
        if state_slots:
            for name, slot in state_slots.items():
                self.emit(f"if (pending_state[{slot}].active)")
                self.emit(
                    f"    {self.scalar_member(name)} = pending_state[{slot}].value;"
                )
        if memory_capacity:
            self.open("for (size_t slot = 0; slot < pending_memory_used; ++slot)")
            self.emit(
                "pending_memory[slot].memory[pending_memory[slot].address] = "
                "pending_memory[slot].value;"
            )
            self.close()
