"""Generate the optional, in-process Gated SSA to native backend adapter."""

from pathlib import Path

from .gated import _inventory
from .scheduler import _instruction_primitive

_SOURCE_DIR = Path(__file__).with_name("native")

# Only these exact ArchSem projections agree with generic MIR on all 64-bit
# input bit patterns. In particular, MIR masks shift counts and ArchSem need
# not; a shift or other nested expression is not admitted on name alone.
_ARITIES = {"COPY": 1, "ADD": 2, "SUB": 2, "MUL": 2, "AND": 2, "OR": 2, "XOR": 2}
_MAX_PRIMITIVE_STATEMENTS = 128


def _primitive(instruction: dict) -> tuple[str, int] | None:
    # The scheduler admits inout and may ignore unreferenced declarations.
    # This adapter has only ordinary value inputs and one independent output.
    # Bound local alias resolution in the exact matcher for non-native ArchDBs.
    semantics = instruction["semantics"]
    operands = instruction["operands"]
    directions = [row.get("direction") for row in operands]
    if (
        semantics is None
        or len(semantics["statements"]) > _MAX_PRIMITIVE_STATEMENTS
        or any(row["kind"] != "reg" for row in operands)
        or directions.count("out") != 1
        or directions.count("in") != len(operands) - 1
    ):
        return None
    match = _instruction_primitive(instruction)
    if match is None:
        return None
    primitive, width, kinds, widths = match
    arity = _ARITIES.get(primitive)
    if (
        arity is None
        or width != 64
        or kinds != ("reg",) * arity
        or widths != (64,) * arity
        or len(directions) != arity + 1
    ):
        return None
    return primitive, arity


def emit_gated_adapter(context: dict) -> dict[str, str]:
    """Emit the bridge for the same validated context that generates Gated IR."""
    _, opcodes = _inventory(context)
    instructions = (
        instruction
        for db in context["architectures"]
        for instruction in sorted(
            db["instructions"], key=lambda row: (row["name"], str(row.get("id", "")))
        )
    )
    cases = []
    # Match by inventory position, not display name: distinct names may
    # normalize to the same C identifier before the Gated emitter adds suffixes.
    for (enum_name, _, _), instruction in zip(opcodes, instructions, strict=True):
        match = _primitive(instruction)
        if match is not None:
            primitive, arity = match
            cases.append(f"    case {enum_name}: *arity = {arity}; return ARCHON_MIR_{primitive};")
    source = (_SOURCE_DIR / "GatedBackend.c.in").read_text(encoding="utf-8")
    return {
        "GatedBackend.c": source.replace("@GATED_PRIMITIVE_CASES@", "\n".join(cases)),
        "GatedBackend.h": (_SOURCE_DIR / "GatedBackend.h").read_text(encoding="utf-8"),
    }
