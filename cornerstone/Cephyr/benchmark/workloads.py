"""Deterministic integer workloads and independently computed check values."""

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Workload:
    name: str
    source: str
    rewrite_expected: bool


FIXTURES = {
    "identity": True,
    "integer-promotions": True,
    "branch-join": True,
    "loops": True,
    "memory-calls": True,
    "no-match": False,
}
MASK = (1 << 64) - 1


def generated(kind: str, width: int, steps: int) -> Workload:
    """Build separate SSA-producing functions at controlled match densities."""
    functions = []
    checks = []
    for index in range(width):
        value = (0x9E3779B97F4A7C15 * (index + 1)) & MASK
        seed = value
        body = []
        for step in range(steps):
            if kind == "strength":
                k = (index + 1) * (step + 19)
                expressions = [(f"(2UL * x) ^ {k}UL", (2 * value) ^ k),
                               (f"(x * 2UL) + {k}UL", (value * 2) + k)]
            elif kind == "dense" or (kind == "sparse" and step % 8 == 0):
                expressions = [
                    ("(x + 0UL) ^ 0UL", value),
                    ("(1UL * x) - 0UL", value),
                    ("(x | 0UL) + 0UL", value),
                    ("(x ^ 0UL) * 1UL", value),
                ]
            else:
                k = (index + 1) * (step + 17)
                expressions = [
                    (f"(x + {k}UL) ^ 51UL", (value + k) ^ 51),
                    (f"x * 3UL - {k}UL", value * 3 - k),
                    (f"(x ^ {k}UL) + 37UL", (value ^ k) + 37),
                    (f"(x >> 1) | {k}UL", (value >> 1) | k),
                ]
            expression, value = expressions[(index + step) % len(expressions)]
            value &= MASK
            body.append(f"    x = {expression};")
        functions.append(
            f"static unsigned long f{index}(unsigned long x) {{\n"
            + "\n".join(body) + "\n    return x;\n}")
        checks.append(f"    if (f{index}(0x{seed:016x}UL) != 0x{value:016x}UL) return {index + 1};")
    return Workload(
        f"{kind}-{width}x{steps}",
        "\n\n".join(functions) + "\n\nint main(void) {\n" +
        "\n".join(checks) + "\n    return 0;\n}\n",
        kind in ("dense", "sparse"),
    )


def workloads() -> list[Workload]:
    root = Path(__file__).resolve().parent / "fixtures"
    result = [Workload(name, (root / (name + ".c")).read_text(encoding="ascii"), expected)
              for name, expected in FIXTURES.items()]
    result.extend(generated(kind, width, steps)
                  for kind in ("dense", "sparse", "inert", "strength")
                  for width, steps in ((1, 24), (2, 48), (4, 80), (8, 112)))
    return result
