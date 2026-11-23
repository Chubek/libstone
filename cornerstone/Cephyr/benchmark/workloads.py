"""Deterministic integer workloads and independently computed check values."""

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Workload:
    name: str
    source: str
    expected_mir_changes: dict[str, bool]
    driver: str | None = None
    removed_integer_ops: tuple[str, int] | None = None


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
        {"canonicalize": kind in ("dense", "sparse"), "strength_reduce": kind == "strength",
         "blackstone": kind in ("dense", "sparse")},
    )


def integer_generated(kind: str, width: int, steps: int) -> Workload:
    """Compile only the kernels with the selected pass; check against Python."""
    def operation(index: int, step: int, x: str, seed: int, value: int) -> tuple[str, int]:
        shift = (8, 16, 32, 40)[(index + step) % 4]
        if kind == "remainder":
            return f"({x} % (1UL << {shift}))", value % (1 << shift)
        if kind == "remainder-high":
            return f"({x} % (1UL << 63))", value % (1 << 63)
        if kind == "remainder-one":
            return f"({x} % 1UL)", 0
        if kind == "remainder-near":
            if step % 2:
                divisor = (seed >> 61) | 1
                return f"({x} % divisor)", value % divisor
            return f"({x} % ((1UL << {shift}) - 1UL))", value % ((1 << shift) - 1)
        if kind == "signed-near":
            signed = (value >> 1) - (1 << 62)
            remainder = abs(signed) % (1 << shift)
            return (f"((unsigned long)(((long)({x} >> 1) - (1L << 62)) % (1L << {shift})))",
                    -remainder if signed < 0 else remainder)
        if kind == "mask":
            if step % 2:
                return f"(({x} | 255UL) & ({x} & 255UL))", value & 255
            return f"(({x} & 255UL) & 65535UL)", value & 255
        if kind == "mask-near":
            return f"(({x} & 65535UL) & 32767UL)", value & 32767
        if kind == "mixed":
            return f"((({x} + 0UL) * 2UL) % (1UL << {shift}))", ((value * 2) & MASK) % (1 << shift)
        raise ValueError(f"unknown integer workload: {kind}")

    functions = []
    declarations = []
    checks = []
    for index in range(width):
        seeds = [0, 1, (1 << 63) - 1, 1 << 63, MASK, (0x9E3779B97F4A7C15 * (index + 1)) & MASK]
        values = seeds.copy()
        sums = [0] * len(seeds)
        body = ["    unsigned long divisor = (x >> 61) | 1UL;"] if kind == "remainder-near" else []
        for step in range(steps):
            # Mix high and low bits so a previous remainder doesn't constrain
            # the next dividend to its reduced range.
            salt = (0xD1B54A32D192ED03 * (step + 1) * (index + 1)) & MASK
            if kind == "mask-near":
                # Force bits outside both masks to be set after the XOR, so
                # neither mask becomes redundant in a later iteration.
                salt |= (1 << 63) | (1 << 15)
            results = [operation(index, step, "x", seed, value) for seed, value in zip(seeds, values)]
            expr = results[0][0]
            values = [(result ^ salt) & MASK for _, result in results]
            sums = [(total + value) & MASK for total, value in zip(sums, values)]
            body.append(f"    x = {expr} ^ 0x{salt:016x}UL;")
            # Make every step observable even when later masks drop bits.
            body.append("    sum += x;" if step else "    unsigned long sum = x;")
        name = f"kernel_{index}"
        declarations.append(f"unsigned long {name}(unsigned long x);")
        functions.append(f"unsigned long {name}(unsigned long x) {{\n"
                         + "\n".join(body) + "\n    return sum;\n}")
        for seed, value in zip(seeds, sums):
            checks.append(f"    if ({name}(0x{seed:016x}UL) != 0x{value:016x}UL) "
                          f"return {len(checks) + 1};")
    assert len(checks) <= 255
    mixed = kind == "mixed"
    integer = not kind.endswith("-near")
    removed = None
    if kind in ("remainder", "remainder-high", "remainder-one", "mixed"):
        removed = ("rem_u", width * steps)
    elif kind == "mask":
        removed = ("and", width * steps)
    return Workload(
        f"ir-{kind}-{width}x{steps}", "\n\n".join(functions) + "\n",
        {"canonicalize": mixed, "strength_reduce": mixed,
         "canonicalize+strength_reduce": mixed, "integer_reduce": integer,
         "all": integer},
        "\n".join(declarations) + "\n\nint main(void) {\n" +
        "\n".join(checks) + "\n    return 0;\n}\n",
        removed,
    )


def workloads(suite: str) -> list[Workload]:
    if suite == "integer-reduce":
        return [integer_generated(kind, 4, 48) for kind in (
            "remainder", "remainder-high", "remainder-one", "remainder-near", "signed-near",
            "mask", "mask-near", "mixed",
        )]
    root = Path(__file__).resolve().parent / "fixtures"
    result = [Workload(name, (root / (name + ".c")).read_text(encoding="ascii"),
                       {"canonicalize": expected, "blackstone": expected})
              for name, expected in FIXTURES.items()]
    result.extend(generated(kind, width, steps)
                  for kind in ("dense", "sparse", "inert", "strength")
                  for width, steps in ((1, 24), (2, 48), (4, 80), (8, 112)))
    if suite == "blackstone":
        seed = 0x61F059B2
        expected = seed
        for _ in range(1000 * 1000):
            expected = ((expected + 17) * 3) & MASK
        driver = ("unsigned long kernel(unsigned long, unsigned long);\n"
                  f"int main(void) {{ unsigned long x = {seed}UL;\n"
                  "    for (int i = 0; i < 1000; ++i) x = kernel(x, (unsigned long)i);\n"
                  f"    return x != 0x{expected:016x}UL;\n}}\n")
        result.append(Workload("blackstone-loop", (root / "blackstone-loop.c").read_text(encoding="ascii"),
                               {"blackstone": True}, driver))
    return result
