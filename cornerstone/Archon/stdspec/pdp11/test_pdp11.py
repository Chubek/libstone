"""Golden encodings and independent semantic checks for the PDP-11 subset."""

from pathlib import Path
import unittest

from archon import database
from archon.compiler import compile_file
from archon.diagnostics import ArchonError
from archon.runtime import Target


HERE = Path(__file__).resolve().parent
FLAGS = ("N", "Z", "V", "C")
ENCODING_ONLY = frozenset({"HALT", "WAIT", "RTI", "BPT", "IOT", "RESET"})
CONDITION_CODES = frozenset({
    "NOP", "CLC", "CLV", "CLZ", "CLN", "CCC",
    "SEC", "SEV", "SEZ", "SEN", "SCC",
})
WORD_UNARY = frozenset({
    "CLR", "COM", "INC", "DEC", "NEG", "ADC", "SBC",
    "TST", "ROR", "ROL", "ASR", "ASL",
})
BYTE_UNARY = frozenset(name + "B" for name in WORD_UNARY)
DOUBLE_OPERAND = frozenset({
    "MOV", "CMP", "BIT", "BIC", "BIS", "ADD", "SUB",
    "MOVB", "CMPB", "BITB", "BICB", "BISB",
})
EXECUTABLE = CONDITION_CODES | {"SWAB"} | WORD_UNARY | BYTE_UNARY | DOUBLE_OPERAND


def reference_execute(name, operands, registers, flags):
    """Small, deliberately non-ArchSem PDP-11 register-direct oracle."""
    registers = registers.copy()
    flags = flags.copy()

    def set_nz(value, width):
        value &= (1 << width) - 1
        flags["N"] = (value >> (width - 1)) & 1
        flags["Z"] = int(value == 0)

    if name in CONDITION_CODES:
        if name == "CCC":
            flags.update({flag: 0 for flag in FLAGS})
        elif name == "SCC":
            flags.update({flag: 1 for flag in FLAGS})
        elif name != "NOP":
            flag = name[-1]
            flags[flag] = int(name.startswith("SE"))
        return registers, flags

    if name == "SWAB":
        index = operands["dst"]
        value = registers[index]
        result = ((value & 0xff) << 8) | (value >> 8)
        registers[index] = result
        set_nz(result, 8)
        flags["V"] = flags["C"] = 0
        return registers, flags

    if name in WORD_UNARY or name in BYTE_UNARY:
        byte = name.endswith("B")
        operation = name[:-1] if byte else name
        width = 8 if byte else 16
        mask = (1 << width) - 1
        sign = 1 << (width - 1)
        index = operands["dst"]
        original_word = registers[index]
        value = original_word & mask
        carry_in = flags["C"]
        write_result = True

        if operation == "CLR":
            result = 0
            flags["V"] = flags["C"] = 0
        elif operation == "COM":
            result = (~value) & mask
            flags["V"], flags["C"] = 0, 1
        elif operation == "INC":
            result = (value + 1) & mask
            flags["V"] = int(value == sign - 1)
        elif operation == "DEC":
            result = (value - 1) & mask
            flags["V"] = int(value == sign)
        elif operation == "NEG":
            result = (-value) & mask
            flags["V"] = int(value == sign)
            flags["C"] = int(result != 0)
        elif operation == "ADC":
            total = value + carry_in
            result = total & mask
            flags["V"] = int(carry_in == 1 and value == sign - 1)
            flags["C"] = int(total > mask)
        elif operation == "SBC":
            result = (value - carry_in) & mask
            flags["V"] = int(carry_in == 1 and value == sign)
            flags["C"] = int(value < carry_in)
        elif operation == "TST":
            result = value
            flags["V"] = flags["C"] = 0
            write_result = False
        elif operation == "ROR":
            result = (value >> 1) | (carry_in << (width - 1))
            flags["C"] = value & 1
            flags["V"] = ((result >> (width - 1)) & 1) ^ flags["C"]
        elif operation == "ROL":
            result = ((value << 1) | carry_in) & mask
            flags["C"] = (value >> (width - 1)) & 1
            flags["V"] = ((result >> (width - 1)) & 1) ^ flags["C"]
        elif operation == "ASR":
            result = (value >> 1) | (value & sign)
            flags["C"] = value & 1
            flags["V"] = ((result >> (width - 1)) & 1) ^ flags["C"]
        elif operation == "ASL":
            result = (value << 1) & mask
            flags["C"] = (value >> (width - 1)) & 1
            flags["V"] = ((result >> (width - 1)) & 1) ^ flags["C"]
        else:  # pragma: no cover - guarded by the instruction sets above
            raise AssertionError(name)

        set_nz(result, width)
        if write_result:
            registers[index] = (
                (original_word & 0xff00) | result if byte else result
            )
        return registers, flags

    if name in DOUBLE_OPERAND:
        byte = name in {"MOVB", "CMPB", "BITB", "BICB", "BISB"}
        operation = name[:-1] if byte else name
        width = 8 if byte else 16
        mask = (1 << width) - 1
        sign = 1 << (width - 1)
        src_index, dst_index = operands["src"], operands["dst"]
        source = registers[src_index] & mask
        destination_word = registers[dst_index]
        destination = destination_word & mask

        if operation == "MOV":
            result = source
            if byte:
                registers[dst_index] = result | (0xff00 if result & sign else 0)
            else:
                registers[dst_index] = result
            set_nz(result, width)
            flags["V"] = 0
        elif operation == "CMP":
            result = (source - destination) & mask
            set_nz(result, width)
            flags["V"] = int(bool((source ^ destination) & (source ^ result) & sign))
            flags["C"] = int(source < destination)
        elif operation == "BIT":
            result = source & destination
            set_nz(result, width)
            flags["V"] = 0
        elif operation in {"BIC", "BIS"}:
            result = (
                destination & (~source & mask)
                if operation == "BIC"
                else destination | source
            )
            registers[dst_index] = (
                (destination_word & 0xff00) | result if byte else result
            )
            set_nz(result, width)
            flags["V"] = 0
        elif operation == "ADD":
            total = destination + source
            result = total & mask
            registers[dst_index] = result
            set_nz(result, width)
            flags["V"] = int(bool((~(destination ^ source) & (destination ^ result)) & sign))
            flags["C"] = int(total > mask)
        elif operation == "SUB":
            result = (destination - source) & mask
            registers[dst_index] = result
            set_nz(result, width)
            flags["V"] = int(bool((destination ^ source) & (destination ^ result) & sign))
            flags["C"] = int(destination < source)
        else:  # pragma: no cover - guarded by the instruction sets above
            raise AssertionError(name)
        return registers, flags

    raise AssertionError(f"missing reference semantics for {name}")


class PDP11SubsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.db = database.loads(database.dumps(compile_file(HERE / "root.arch")))
        cls.target = Target(cls.db)
        cls.names = {inst["name"] for inst in cls.db["instructions"]}
        cls.semantics = {
            inst["name"]: inst["semantics"] for inst in cls.db["instructions"]
        }
        lines = (HERE / "encoding-golden.tsv").read_text(encoding="ascii").splitlines()
        cls.vectors = [
            (bytes.fromhex(raw), asm)
            for raw, asm in (line.split("\t") for line in lines)
        ]

    def state(self, registers=None, flags=None, *, patterned_memory=False):
        memory = None
        if patterned_memory:
            memory = bytes((index * 37 + 11) & 0xff for index in range(1 << 16))
        return self.target.new_state(
            registers=[0] * 8 if registers is None else registers,
            scalars={flag: 0 for flag in FLAGS} if flags is None else flags,
            memories=None if memory is None else {"MainMemory": memory},
        )

    def assert_reference(self, name, operands, registers, flags):
        expected_registers, expected_flags = reference_execute(
            name, operands, registers, flags
        )
        initial = self.state(registers, flags)
        actual = self.target.execute_state(name, operands, initial)
        self.assertEqual(actual.registers, expected_registers)
        self.assertEqual(actual.scalars, expected_flags)
        self.assertEqual(actual.stacks, {})
        self.assertEqual(actual.memories, initial.memories)

    def test_golden_encodings(self):
        self.assertEqual(len({data for data, _ in self.vectors}), len(self.vectors))
        self.assertEqual({asm.split()[0].upper() for _, asm in self.vectors}, self.names)
        for data, assembly in self.vectors:
            with self.subTest(assembly=assembly):
                self.assertEqual(len(data), 2)
                inst = self.target.assemble(assembly)
                self.assertEqual(self.target.encode(*inst), data)
                self.assertEqual(self.target.decode(data), inst)
                self.assertEqual(self.target.disassemble(data), assembly)

    def test_exact_word_space(self):
        # Expand each independently checked golden word across register indices,
        # leaving all opcode and mode bits fixed. All other words must fail.
        expected = {}
        for data, assembly in self.vectors:
            word = int.from_bytes(data, "little")
            mnemonic, _, operands = assembly.partition(" ")
            name = mnemonic.upper()
            if "," in operands:
                for src in range(8):
                    for dst in range(8):
                        expected[(word & ~0o707) | (src << 6) | dst] = (
                            name, {"src": src, "dst": dst}
                        )
            elif operands:
                for dst in range(8):
                    expected[(word & ~0o7) | dst] = (name, {"dst": dst})
            else:
                expected[word] = (name, {})
        self.assertEqual(len(expected), 17 + 25 * 8 + 12 * 64)

        for word in range(1 << 16):
            data = word.to_bytes(2, "little")
            if word in expected:
                inst = expected[word]
                self.assertEqual(self.target.decode(data), inst, f"{word:06o}")
                self.assertEqual(self.target.encode(*inst), data, f"{word:06o}")
            else:
                with self.assertRaisesRegex(
                    ArchonError, "unknown instruction encoding", msg=f"{word:06o}"
                ):
                    self.target.decode(data)

    def test_assembly_scope_and_aliases(self):
        self.assertEqual(
            self.target.assemble("mov sp, pc"), ("MOV", {"src": 6, "dst": 7})
        )
        self.assertEqual(self.target.assemble("inc sp"), ("INC", {"dst": 6}))
        self.assertEqual(self.target.assemble("tstb pc"), ("TSTB", {"dst": 7}))
        self.assertEqual(self.target.disassemble(bytes.fromhex("8711")), "mov r6, r7")
        for assembly in (
            "mov r8, r0", "mov (r1), r2", "mov r1, (r2)+",
            "mov #1, r0", "mov r0, 1", "clr 2(r3)", "addb r0, r1",
            "jmp r0", "jsr r5, r0", "br 2", "xor r0, r1", "rtt",
            "nop r0", "clc clv", "MOV r0, r1",
        ):
            with self.subTest(assembly=assembly), self.assertRaises(ArchonError):
                self.target.assemble(assembly)
        for data in (b"", b"\xa0", b"\xa0\x00\xa0\x00"):
            with self.subTest(data=data), self.assertRaisesRegex(
                ArchonError, "expected 2 instruction bytes"
            ):
                self.target.decode(data)

    def test_every_executable_instruction_has_an_oracle(self):
        self.assertEqual(self.names, EXECUTABLE | ENCODING_ONLY)
        self.assertEqual(
            {name for name, semantics in self.semantics.items() if semantics is not None},
            EXECUTABLE,
        )
        for name in EXECUTABLE:
            effects = self.semantics[name]["effects"]
            self.assertEqual(effects["stack_reads"], [])
            self.assertEqual(effects["stack_writes"], [])
            self.assertEqual(effects["memory_reads"], [])
            self.assertEqual(effects["memory_writes"], [])

    def test_condition_code_instructions_against_reference(self):
        for name in sorted(CONDITION_CODES):
            for bits in range(16):
                flags = dict(zip(FLAGS, ((bits >> shift) & 1 for shift in range(4))))
                with self.subTest(name=name, flags=flags):
                    self.assert_reference(
                        name, {}, [0x1234, 1, 2, 3, 4, 5, 0x2000, 0x1000], flags
                    )

    def test_single_operand_word_and_byte_edges_against_reference(self):
        word_values = (
            0x0000, 0x0001, 0x0002, 0x7ffe, 0x7fff,
            0x8000, 0x8001, 0xfffe, 0xffff, 0x55aa,
        )
        byte_values = tuple(
            0xa500 | value
            for value in (0x00, 0x01, 0x02, 0x7e, 0x7f, 0x80, 0x81, 0xfe, 0xff, 0x55)
        )
        flag_cases = (
            {"N": 0, "Z": 0, "V": 0, "C": 0},
            {"N": 1, "Z": 1, "V": 1, "C": 1},
        )
        groups = (
            (sorted(WORD_UNARY | {"SWAB"}), word_values),
            (sorted(BYTE_UNARY), byte_values),
        )
        for names, values in groups:
            for name in names:
                for value in values:
                    for flags in flag_cases:
                        registers = [0x1111, 0x2222, 0x3333, value,
                                     0x5555, 0x6666, 0x7777, 0x8888]
                        with self.subTest(name=name, value=f"{value:04x}", flags=flags):
                            self.assert_reference(name, {"dst": 3}, registers, flags)

    def test_double_operand_edges_against_reference(self):
        pairs = (
            (0x0000, 0x0000),
            (0x0001, 0x0000),
            (0x0000, 0x0001),
            (0x7fff, 0x0001),
            (0x0001, 0x7fff),
            (0x8000, 0x0001),
            (0x0001, 0x8000),
            (0xffff, 0x0001),
            (0x0001, 0xffff),
            (0xffff, 0x7fff),
            (0x7fff, 0xffff),
            (0xffff, 0x8000),
            (0x8000, 0xffff),
            (0x00ff, 0xff00),
            (0xff00, 0x00ff),
            (0x55aa, 0xaa55),
            (0xaa55, 0x55aa),
        )
        flag_cases = (
            {"N": 0, "Z": 0, "V": 0, "C": 0},
            {"N": 1, "Z": 1, "V": 1, "C": 1},
        )
        for name in sorted(DOUBLE_OPERAND):
            for source, destination in pairs:
                for flags in flag_cases:
                    registers = [0x1111, source, destination, 0x3333,
                                 0x4444, 0x5555, 0x6666, 0x7777]
                    operands = {"src": 1, "dst": 2}
                    with self.subTest(
                        name=name, source=f"{source:04x}",
                        destination=f"{destination:04x}", flags=flags,
                    ):
                        self.assert_reference(name, operands, registers, flags)

    def test_same_register_and_sp_pc_alias_semantics(self):
        cases = (
            ("inc sp", [0, 1, 2, 3, 4, 5, 0x7fff, 0x1111]),
            ("mov pc, sp", [0, 1, 2, 3, 4, 5, 0x2222, 0x8001]),
            ("add sp, sp", [0, 1, 2, 3, 4, 5, 0x8000, 0x1111]),
            ("sub pc, pc", [0, 1, 2, 3, 4, 5, 0x2222, 0xabcd]),
            ("bicb sp, sp", [0, 1, 2, 3, 4, 5, 0xa5ff, 0x1111]),
            ("movb pc, pc", [0, 1, 2, 3, 4, 5, 0x2222, 0x0080]),
        )
        flags = {"N": 1, "Z": 0, "V": 1, "C": 1}
        for assembly, registers in cases:
            name, operands = self.target.assemble(assembly)
            with self.subTest(assembly=assembly):
                self.assert_reference(name, operands, registers, flags)

    def test_byte_high_byte_policy(self):
        preserve = sorted((BYTE_UNARY - {"TSTB"}) | {"BICB", "BISB"})
        flags = {"N": 1, "Z": 0, "V": 1, "C": 1}
        for name in preserve:
            registers = [0, 0, 0x0055, 0xa580, 0, 0, 0, 0]
            operands = (
                {"src": 2, "dst": 3} if name in {"BICB", "BISB"} else {"dst": 3}
            )
            with self.subTest(name=name):
                result = self.target.execute_state(name, operands, self.state(registers, flags))
                self.assertEqual(result.registers[3] & 0xff00, 0xa500)

        for source, expected in ((0x007f, 0x007f), (0x0080, 0xff80), (0x00ff, 0xffff)):
            registers = [0, source, 0xa555, 0, 0, 0, 0, 0]
            result = self.target.execute_state(
                "MOVB", {"src": 1, "dst": 2}, self.state(registers, flags)
            )
            self.assertEqual(result.registers[2], expected)

    def test_input_state_is_immutable_and_results_are_detached(self):
        initial = self.state(
            [0xffff, 1, 2, 3, 4, 5, 0x2000, 0x1000],
            {"N": 1, "Z": 0, "V": 1, "C": 1},
            patterned_memory=True,
        )
        snapshot = initial.copy()
        result = self.target.execute_state("ADCB", {"dst": 0}, initial)
        self.assertEqual(initial, snapshot)
        self.assertIsNot(result.registers, initial.registers)
        self.assertIsNot(result.scalars, initial.scalars)
        self.assertIsNot(result.memories["MainMemory"], initial.memories["MainMemory"])
        self.assertEqual(result.memories, initial.memories)

        result.registers[0] = 0
        result.scalars["C"] = 0
        result.memories["MainMemory"][0] ^= 0xff
        self.assertEqual(initial, snapshot)

    def test_unrepresentable_fixed_instructions_are_encoding_only_and_atomic(self):
        initial = self.state(
            [0xffff, 1, 2, 3, 4, 5, 0x2000, 0x1000],
            {"N": 1, "Z": 0, "V": 1, "C": 1},
            patterned_memory=True,
        )
        snapshot = initial.copy()
        for name in sorted(ENCODING_ONLY):
            with self.subTest(name=name), self.assertRaisesRegex(
                ArchonError, rf"^{name} has no executable semantics$"
            ):
                self.target.execute_state(name, {}, initial)
            self.assertEqual(initial, snapshot)

        program = (
            self.target.encode("INC", {"dst": 0})
            + self.target.encode("HALT", {})
        )
        with self.assertRaisesRegex(
            ArchonError,
            r"at byte address 0x102: HALT has no executable semantics",
        ):
            self.target.execute_program_state(program, initial, base_address=0x100)
        self.assertEqual(initial, snapshot)


if __name__ == "__main__":
    unittest.main()
