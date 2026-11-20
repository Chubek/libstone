"""End-to-end tests from standard .arch source through concrete execution."""

from pathlib import Path
from random import Random
import unittest

from archon.compiler import compile_file
from archon.runtime import Target


STDSPEC = Path(__file__).resolve().parents[1] / "stdspec"
TARGETS = (
    "riscv/rv32i.arch", "riscv/rv64i.arch", "aarch64/root.arch", "mips32/root.arch",
    "wasm/root.arch", "amd64/root.arch", "jvm/root.arch", "clr/root.arch", "pdp11/root.arch",
)


class StandardSpecifications(unittest.TestCase):
    def test_golden_encodings(self):
        for spec in TARGETS:
            path = STDSPEC / spec
            target = Target(compile_file(path))
            goldens = path.with_name("encoding-golden.tsv").read_text(encoding="ascii")
            for line in goldens.splitlines():
                if not line or line.startswith("#"):
                    continue
                hex_bytes, assembly = line.split("\t")
                with self.subTest(spec=spec, assembly=assembly):
                    name, operands = target.assemble(assembly)
                    data = bytes.fromhex(hex_bytes)
                    self.assertEqual(target.encode(name, operands), data)
                    self.assertEqual(target.decode(data), (name, operands))

    def test_every_instruction_round_trips(self):
        rng = Random(2026)
        for spec in TARGETS:
            db = compile_file(STDSPEC / spec)
            target = Target(db)
            for inst in db["instructions"]:
                with self.subTest(spec=spec, instruction=inst["name"]):
                    bounds = {}
                    for op in inst["operands"]:
                        if op["kind"] == "reg":
                            low, high = op.get("register_range", (0, db["registers"]["count"] - 1))
                        elif op["signed"]:
                            low, high = -(1 << (op["width"] - 1)), (1 << (op["width"] - 1)) - 1
                        else:
                            low, high = 0, (1 << op["width"]) - 1
                        bounds[op["name"]] = (low, high)
                    samples = (
                        (lambda low, high: low),
                        (lambda low, high: high),
                        *(rng.randint for _ in range(12)),
                    )
                    for sample in samples:
                        ops = {name: sample(*limits) for name, limits in bounds.items()}
                        data = target.encode(inst["name"], ops)
                        self.assertEqual(int.from_bytes(data, db["endian"]) & inst["mask"], inst["value"])
                        self.assertEqual(target.decode(data), (inst["name"], ops))
                        self.assertEqual(target.assemble(target.disassemble(data)), (inst["name"], ops))

    def test_riscv(self):
        for width in (32, 64):
            with self.subTest(width=width):
                db = compile_file(STDSPEC / "riscv" / f"rv{width}i.arch")
                target = Target(db)
                self.assertEqual(db["registers"]["width"], width)

                name, operands = target.assemble("add x1, x2, x3")
                data = bytes.fromhex("b3003100")
                self.assertEqual(target.encode(name, operands), data)
                self.assertEqual(target.decode(data), (name, operands))
                self.assertEqual(target.assemble(target.disassemble(data)), (name, operands))

                regs = [0] * 32
                regs[2:4] = [(1 << width) - 1, 2]
                self.assertEqual(target.execute(name, operands, regs)[1], 1)
                self.assertEqual(regs[1], 0)
                add_zero, zero_ops = target.assemble("add x0, x2, x3")
                self.assertEqual(target.execute(add_zero, zero_ops, regs)[0], 0)

                name, operands = target.assemble("addi x1, x0, -1")
                data = bytes.fromhex("9300f0ff")
                self.assertEqual(target.encode(name, operands), data)
                self.assertEqual(target.decode(data), (name, operands))
                self.assertEqual(target.execute(name, operands, regs)[1], (1 << width) - 1)

    def test_aarch64(self):
        db = compile_file(STDSPEC / "aarch64" / "root.arch")
        target = Target(db)
        name, operands = target.assemble("add x0, x1, x2")
        data = bytes.fromhex("2000028b")
        self.assertEqual(target.encode(name, operands), data)
        self.assertEqual(target.decode(data), (name, operands))
        regs = [0] * 32
        regs[1:3] = [7, 13]
        self.assertEqual(target.execute(name, operands, regs)[0], 20)

    def test_mips32(self):
        db = compile_file(STDSPEC / "mips32" / "root.arch")
        target = Target(db)
        data = (0x012A4021).to_bytes(4, db["endian"])  # addu $8,$9,$10
        name, operands = target.decode(data)
        self.assertEqual(name.upper(), "ADDU")
        self.assertEqual(target.encode(name, operands), data)
        self.assertEqual(target.assemble(target.disassemble(data)), (name, operands))
        regs = [0] * 32
        regs[9:11] = [0xFFFFFFFF, 2]
        self.assertEqual(target.execute(name, operands, regs)[8], 1)


if __name__ == "__main__":
    unittest.main()
