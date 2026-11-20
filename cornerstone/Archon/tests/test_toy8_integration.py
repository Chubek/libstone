"""Smoke tests across the compiler, database and runtime for the Toy8 example."""

from pathlib import Path
import unittest

from archon.compiler import compile_file
from archon.database import dumps, loads
from archon.runtime import Target


EXAMPLES = Path(__file__).resolve().parents[1] / "examples"


class Toy8Integration(unittest.TestCase):
    def setUp(self):
        self.db = loads(dumps(compile_file(EXAMPLES / "toy8.arch")))
        self.target = Target(self.db)

    def test_template_emits_normal_instruction(self):
        inst = next(i for i in self.db["instructions"] if i["name"] == "ADD")
        self.assertEqual(inst["id"], "Toy8.ADD")
        self.assertEqual(inst["value"], 0x0200)
        name, args = self.target.assemble("add r1, r2, r3")
        self.assertEqual(self.target.encode(name, args), bytes.fromhex("53 02"))
        state = [0, 0, 255, 2, 0, 0, 0, 0]
        self.assertEqual(self.target.execute(name, args, state)[1], 1)

    def test_swap_executes_after_serialization(self):
        inst = next(i for i in self.db["instructions"] if i["name"] == "SWAP")
        effects = inst["semantics"]["effects"]
        self.assertEqual(set(effects["reads"]), {"lhs", "rhs"})
        self.assertEqual(set(effects["writes"]), {"lhs", "rhs"})

        code = bytes.fromhex("0a 04")  # SWAP r1,r2
        name, args = self.target.decode(code)
        old = [0, 5, 9, 0, 0, 0, 0, 0]
        self.assertEqual(self.target.execute(name, args, old)[1:3], [9, 5])
        self.assertEqual(old[1:3], [5, 9])
        name, args = self.target.assemble("swap r1, zero")
        self.assertEqual(self.target.execute(name, args, old)[:2], [0, 0])

    def test_labeled_program_end_to_end(self):
        path = EXAMPLES / "toy8.s"
        source = path.read_text(encoding="utf-8")
        data = self.target.assemble_program(source, filename=str(path))
        self.assertEqual(data, bytes.fromhex("06 81 fe 82 0a 04 ca 02"))
        self.assertEqual(
            self.target.assemble_program(source, base_address=16),
            bytes.fromhex("16 81 fe 82 0a 04 ca 02"),
        )
        disassembled = self.target.disassemble_program(data)
        self.assertEqual(disassembled.splitlines(), [
            "movi r1, 6", "movi r2, -2", "swap r1, r2", "add r3, r1, r2",
        ])
        self.assertEqual(self.target.assemble_program(disassembled), data)
        state = [0] * 8
        self.assertEqual(self.target.execute_program(data, state)[1:4], [254, 6, 4])
        self.assertEqual(state, [0] * 8)


if __name__ == "__main__":
    unittest.main()
