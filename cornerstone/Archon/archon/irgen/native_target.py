"""Fail-closed target binding for the native RV64IM GNU assembler provider.

The provider is not a retargetable ISA or ABI inference engine. Its reviewed
contract comes from RV64I 2.1 and M 2.0 of the RISC-V unprivileged ISA manual
and the integer LP64 convention of the RISC-V ELF psABI. The scalar calling
convention is supplied by the compiled ABI facts and qualified independently
against this provider's supported subset.

Arithmetic supported by ArchSem is matched against exact typed programs
(including register effects), and representable operand/encoding layouts are
checked. For the remaining operations the manifest binds the native behavior
to one canonical, validated stdspec ArchDB. No generic execution semantics are
added to the DB for these instructions.
"""

from copy import deepcopy
from dataclasses import dataclass
import hashlib
from typing import NoReturn

from .. import database
from ..diagnostics import ArchonError, Location
from ..semantics import compile_semantics


_NAME = "RISCV64IM"
_PARAMETERS = {"XLEN": 64, "SHAMT": 6}
# SHA-256 of the canonical ArchDB with just the independently reviewed
# abi_facts member removed. Keep every other member, including unknown ones.
# Updating the pin requires re-reviewing all native instructions and the DB
# configuration; matching names or assembly spellings are not evidence.
_REVIEWED_ISA_DB = "3e5dc650a0a5df6d05bb780535e8fe4e3f01aff85aff30e834a466734e530e1e"

# Exact provider qualification, not generation data or a fallback. The
# RISC-V ELF psABI integer register convention preserves sp and s0-s11;
# zero, gp and tp are fixed, and the ABI also fixes sp's purpose. The native
# provider supports only a single scalar result in a0 (not a0/a1 pairs).
_REVIEWED_ABI_V1 = {
    "schema_version": 1,
    "name": "LP64",
    "register_file": "IntegerRegisters",
    "args": list(range(10, 18)),
    "result": 10,
    "stack_pointer": 2,
    "return_address": 1,
    "stack_align": 16,
    "stack_slot_bytes": 8,
    "stack_arg_offset": 0,
    "stack_growth": "downward",
    "caller_saved": [1, 5, 6, 7, *range(10, 18), 28, 29, 30, 31],
    "callee_saved": [2, 8, 9, *range(18, 28)],
    "unallocatable": [0, 2, 3, 4],
}


@dataclass(frozen=True)
class _Form:
    # Operands: (name, direction, kind, logical width, signed).
    operands: tuple
    # Fields: (operand, encoding lsb, width, operand lsb). ArchDB v1 can
    # represent only a single field spanning each entire logical operand.
    fields: tuple
    syntax: str
    in_db: bool = True

    def db_operands(self):
        return [
            dict(name=name, direction=direction, kind=kind, width=width, signed=signed)
            for name, direction, kind, width, signed in self.operands
        ]

    def db_fields(self):
        return [
            dict(operand=name, lsb=lsb, width=width)
            for name, lsb, width, _ in self.fields
        ]

    def mask(self):
        variable = 0
        for _, lsb, width, _ in self.fields:
            variable |= ((1 << width) - 1) << lsb
        return 0xFFFFFFFF ^ variable


_RD = ("rd", "out", "reg", 5, False)
_RS1 = ("rs1", "in", "reg", 5, False)
_RS2 = ("rs2", "in", "reg", 5, False)
_IMM12 = ("imm", "in", "imm", 12, True)
_RD_BITS = ("rd", 7, 5, 0)
_RS1_BITS = ("rs1", 15, 5, 0)
_RS2_BITS = ("rs2", 20, 5, 0)
_FORMS = {
    "R": _Form(
        (_RD, _RS1, _RS2), (_RD_BITS, _RS1_BITS, _RS2_BITS), " {rd}, {rs1}, {rs2}"
    ),
    "I": _Form(
        (_RD, _RS1, _IMM12), (_RD_BITS, _RS1_BITS, ("imm", 20, 12, 0)),
        " {rd}, {rs1}, {imm}",
    ),
    "SHIFT": _Form(
        (_RD, _RS1, ("shamt", "in", "imm", 6, False)),
        (_RD_BITS, _RS1_BITS, ("shamt", 20, 6, 0)), " {rd}, {rs1}, {shamt}",
    ),
    "ADDR": _Form(
        (_RD, _RS1, _IMM12), (_RD_BITS, _RS1_BITS, ("imm", 20, 12, 0)),
        " {rd}, {imm}({rs1})",
    ),
    "SYS": _Form((), (), ""),
    "U": _Form(
        (_RD, ("imm", "in", "imm", 20, False)), (_RD_BITS, ("imm", 12, 20, 0)),
        " {rd}, {imm}",
    ),
    # SD, conditional branches and JAL have scattered immediates. Keep their
    # reviewed layouts separate rather than inventing an incorrect DB encoding.
    "S": _Form(
        (_RS2, _RS1, _IMM12),
        (("imm", 7, 5, 0), _RS1_BITS, _RS2_BITS, ("imm", 25, 7, 5)),
        " {rs2}, {imm}({rs1})", in_db=False,
    ),
    "B": _Form(
        (_RS1, _RS2, ("imm", "in", "imm", 13, True)),
        (("imm", 7, 1, 11), ("imm", 8, 4, 1), _RS1_BITS, _RS2_BITS,
         ("imm", 25, 6, 5), ("imm", 31, 1, 12)),
        " {rs1}, {rs2}, {imm}", in_db=False,
    ),
    "J": _Form(
        (_RD, ("imm", "in", "imm", 21, True)),
        (_RD_BITS, ("imm", 12, 8, 12), ("imm", 20, 1, 11),
         ("imm", 21, 10, 1), ("imm", 31, 1, 20)),
        " {rd}, {imm}", in_db=False,
    ),
}


def _binary(operator):
    return f"write(rd, read(rs1) {operator} read(rs2));"


# Stem: (operand format, fixed 32-bit encoding, exact ArchSem witness or None).
# The manifest's encoding-only rows must not be interpreted as ArchSem proof:
#
# * DIV[U]/REM[U] are 64-bit ISA operations, not checked division. Zero divisor
#   gives an all-ones quotient and the dividend as remainder; signed MIN/-1
#   gives MIN and 0 respectively. The native legalizer owns extra MIR checks.
# * LD/SD transfer 64 bits using rs1 + sign_extend(imm), in process memory
#   subject to the execution environment's access/alignment behavior.
# * BEQ/BNE compare registers; JAL links with PC+4. Their displacement is
#   PC-relative; its low bit is implicitly zero. Native emitted labels are
#   four-byte aligned. JALR links with PC+4 and jumps to
#   (rs1 + sign_extend(imm)) & ~1.
# * AUIPC adds a sign-extended (imm20 << 12) to PC. The assembler/linker own
#   PC-relative relocation pairs and call relocation handling, not ArchDB v1.
# * EBREAK raises the breakpoint exception, not a generic ArchSem trap.
_INSTRUCTIONS = {
    "ADD": ("R", 0x00000033, _binary("+%")),
    "SUB": ("R", 0x40000033, _binary("-%")),
    "MUL": ("R", 0x02000033, _binary("*%")),
    "DIV": ("R", 0x02004033, None),
    "REM": ("R", 0x02006033, None),
    "DIVU": ("R", 0x02005033, None),
    "REMU": ("R", 0x02007033, None),
    "AND": ("R", 0x00007033, _binary("&")),
    "OR": ("R", 0x00006033, _binary("|")),
    "XOR": ("R", 0x00004033, _binary("^")),
    "SLL": ("R", 0x00001033, "write(rd, read(rs1) << trunc<6>(read(rs2)));"),
    "SRL": ("R", 0x00005033, "write(rd, read(rs1) >> trunc<6>(read(rs2)));"),
    "SRA": ("R", 0x40005033, """
        let value = read(rs1);
        let count = trunc<6>(read(rs2));
        let sign_fill = 0 -% (value >> ~trunc<6>(0));
        write(rd, ((value ^ sign_fill) >> count) ^ sign_fill);
    """),
    "SLT": ("R", 0x00002033, """
        let a = read(rs1);
        let b = read(rs2);
        let diff = a ^ b;
        write(rd, ((diff & a) | (~diff & (a -% b))) >> 63);
    """),
    "SLTU": ("R", 0x00003033, """
        let a = read(rs1);
        let b = read(rs2);
        write(rd, ((~a & b) | (~(a ^ b) & (a -% b))) >> 63);
    """),
    "ADDI": ("I", 0x00000013, "write(rd, read(rs1) +% sext<64>(imm));"),
    "SLLI": ("SHIFT", 0x00001013, "write(rd, read(rs1) << shamt);"),
    "LD": ("ADDR", 0x00003003, None),
    "SD": ("S", 0x00003023, None),
    "JALR": ("ADDR", 0x00000067, None),
    "EBREAK": ("SYS", 0x00100073, None),
    "BEQ": ("B", 0x00000063, None),
    "BNE": ("B", 0x00001063, None),
    "JAL": ("J", 0x0000006F, None),
    "AUIPC": ("U", 0x00000017, None),
}


def _reject(reason: str) -> NoReturn:
    raise ArchonError(f"native target requires the reviewed RV64IM LP64 ArchDB: {reason}")


def resolve_native_target(db: dict) -> dict:
    """Return fresh provider facts for the one qualified ArchDB, or raise ArchonError."""
    try:
        serialized = database.dumps(db)
    except ArchonError as exc:
        _reject(exc.message)

    if db["architecture"] != _NAME:
        _reject(f"architecture is {db['architecture']!r}, expected {_NAME}")
    if db["endian"] != "little" or db["address_width"] != 64:
        _reject("expected little-endian 64-bit addresses")
    if db["features"] != ["I", "M"] or db["parameters"] != _PARAMETERS:
        _reject("expected I and M with XLEN=64 and SHAMT=6")

    regs = db["registers"]
    if (regs is None or regs["count"] != 32 or regs["width"] != 64
            or regs["zero"] != 0 or regs["prefix"] != "x"):
        _reject("expected 32 64-bit GPRs with x0 hardwired to zero")
    abi = db.get("abi_facts")
    if abi is None:
        _reject("missing declarative abi_facts")
    for key, expected in _REVIEWED_ABI_V1.items():
        if abi[key] != expected:
            _reject(f"abi_facts.{key} does not match the reviewed LP64 scalar ABI")

    alias_requirements = {
        "zero": 0, "sp": abi["stack_pointer"], "ra": abi["return_address"],
        "gp": 3, "tp": 4, "fp": 8,
        **{f"a{i}": register for i, register in enumerate(abi["args"])},
        **{f"s{i}": register for i, register in enumerate((8, 9, *range(18, 28)))},
        **{f"t{i}": register for i, register in enumerate((5, 6, 7, 28, 29, 30, 31))},
    }
    if any(regs["aliases"].get(name) != index for name, index in alias_requirements.items()):
        _reject("GPR aliases do not match the provider's LP64 register numbering")

    rows = {row["name"]: row for row in db["instructions"]}
    if len(rows) != len(db["instructions"]):
        _reject("duplicate instruction names")

    mnemonics = {}
    facts = {}
    for stem, (fmt, fixed, witness) in _INSTRUCTIONS.items():
        form = _FORMS[fmt]
        mnemonic = stem.lower()
        assembly = mnemonic + form.syntax
        mask = form.mask()
        operands = form.db_operands()
        if form.in_db:
            row = rows.get(stem)
            if row is None:
                _reject(f"missing native instruction {stem}")
            if (row["id"] != f"{_NAME}.{stem}" or row["width"] != 32 or row["mask"] != mask
                    or row["value"] != fixed or row["operands"] != operands
                    or row["fields"] != form.db_fields() or row["assembly"] != assembly):
                _reject(f"{stem} does not match the reviewed operand/encoding/assembly layout")
            expected = None if witness is None else compile_semantics(
                witness, Location("<native RV64IM ArchSem witness>", 1, 1), operands, 64, {},
            )
            if row["semantics"] != expected:
                _reject(f"{stem} does not match the reviewed ArchSem program")

        mnemonics[stem] = mnemonic
        facts[stem] = {
            "assembly": assembly,
            "mask": mask,
            "value": fixed,
            "operands": operands,
            "fields": [
                dict(operand=name, lsb=lsb, width=width, operand_lsb=operand_lsb)
                for name, lsb, width, operand_lsb in form.fields
            ],
            "in_db": form.in_db,
            "evidence": "exact-archsem" if witness is not None else "reviewed-isa-manifest",
        }

    isa_db = {key: value for key, value in db.items() if key != "abi_facts"}
    isa_fingerprint = hashlib.sha256(database.dumps(isa_db).encode("ascii")).hexdigest()
    if isa_fingerprint != _REVIEWED_ISA_DB:
        _reject("database fingerprint (excluding ABI facts) differs from the reviewed "
                "stdspec/riscv/rv64im.arch")

    return {
        "name": _NAME, "abi": abi["name"], "instructions": mnemonics,
        "abi_facts": deepcopy(abi), "instruction_facts": facts,
        "fingerprint": hashlib.sha256(serialized.encode("ascii")).hexdigest(),
        # These are GNU assembler expansions, not native opcodes or a DB
        # relocation evaluator. PCREL_LO12_I refers to the anchor on AUIPC.
        "gas_symbol_sequences": {
            "jump": ("AUIPC", "JALR"),
            "call": ("AUIPC", "JALR"),
            "lla": ("AUIPC", "ADDI"),
        },
    }
