"""Fail-closed target binding for reviewed native lowering providers.

The providers are not retargetable ISA or ABI inference engines. The RV64
contract comes from RV64I 2.1 and M 2.0 of the RISC-V unprivileged ISA manual
and the integer LP64 convention of the RISC-V ELF psABI. The scalar calling
convention is supplied by the compiled ABI facts and qualified independently
against this provider's supported subset.

Arithmetic supported by ArchSem is matched against exact typed programs
(including register effects), and representable operand/encoding layouts are
checked. For the remaining operations the manifest binds the native behavior
to canonical, validated stdspec ArchDBs. No generic execution semantics are
added to the DB for these instructions.

The AMD64 and AArch64 GAS-only providers bind to exact, validated stdspec
ArchDBs including their independently qualified scalar ABI v2 facts. The
bundled DBs are not complete ISA descriptions; the additional GAS sequences
come from the baseline instruction sets, not inferred ArchSem or encodings.
"""

from copy import deepcopy
from dataclasses import dataclass
import hashlib
from typing import NoReturn

from .. import database
from ..diagnostics import ArchonError, Location
from ..semantics import compile_semantics


_RV64_PROVIDER = "riscv64-elf-lp64"
_PARAMETERS = {"XLEN": 64, "SHAMT": 6}
_M_STEMS = frozenset({"MUL", "DIV", "REM", "DIVU", "REMU"})


@dataclass(frozen=True)
class _RV64Profile:
    isa: str
    features: tuple[str, ...]
    stdspec: str
    # SHA-256 of the canonical ArchDB with only the independently reviewed
    # ABI facts removed. Updating a pin requires re-reviewing the ISA and DB.
    isa_sha256: str
    manifest_only: frozenset[str] = frozenset()


_RV64_PROFILES = {
    "RISCV64IM": _RV64Profile(
        "RV64IM", ("I", "M"), "stdspec/riscv/rv64im.arch",
        "4c1b040339ebeb2252a47c27347c61847a2cfba83da5ca5d0c19edb61ed70342",
    ),
    "RISCV64": _RV64Profile(
        "RV64I", ("I",), "stdspec/riscv/rv64i.arch",
        "5d0cbb001e67ff1394288ae5b33dbbd1f6fdfcbac1b6d355e42f399bddbd1c96",
        # This DB describes the common integer ALU subset, not all of RV64I.
        # These encodings are taken from the RV64I 2.1 ISA, not fabricated DB
        # rows or inferred from a lookalike name. Do not claim ArchSem checks
        # for syntax/semantics that this exact DB does not contain.
        frozenset({
            "SLT", "SLTU", "SLLI", "LB", "LBU", "LH", "LHU", "LW", "LWU",
            "LD", "JALR", "EBREAK", "AUIPC",
        }),
    ),
}

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
# * Loads/stores use rs1 + sign_extend(imm) in process memory, subject to the
#   execution environment's access/alignment behavior. LB/LH/LW sign-extend;
#   LBU/LHU/LWU zero-extend; stores retain the low 8/16/32/64 source bits.
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
    "LB": ("ADDR", 0x00000003, None),
    "LBU": ("ADDR", 0x00004003, None),
    "LH": ("ADDR", 0x00001003, None),
    "LHU": ("ADDR", 0x00005003, None),
    "LW": ("ADDR", 0x00002003, None),
    "LWU": ("ADDR", 0x00006003, None),
    "LD": ("ADDR", 0x00003003, None),
    "SB": ("S", 0x00000023, None),
    "SH": ("S", 0x00001023, None),
    "SW": ("S", 0x00002023, None),
    "SD": ("S", 0x00003023, None),
    "JALR": ("ADDR", 0x00000067, None),
    "EBREAK": ("SYS", 0x00100073, None),
    "BEQ": ("B", 0x00000063, None),
    "BNE": ("B", 0x00001063, None),
    "JAL": ("J", 0x0000006F, None),
    "AUIPC": ("U", 0x00000017, None),
}


def _resolve_rv64(db: dict, fingerprint: str) -> dict:
    name = db["architecture"]
    profile = _RV64_PROFILES[name]

    def _reject(reason: str) -> NoReturn:
        raise ArchonError(
            f"native target requires the reviewed {profile.isa} LP64 ArchDB: {reason}"
        )

    if db["endian"] != "little" or db["address_width"] != 64:
        _reject("expected little-endian 64-bit addresses")
    if tuple(db["features"]) != profile.features or db["parameters"] != _PARAMETERS:
        _reject(f"expected {' and '.join(profile.features)} with XLEN=64 and SHAMT=6")

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
        if stem in _M_STEMS and "M" not in profile.features:
            continue
        form = _FORMS[fmt]
        mnemonic = stem.lower()
        assembly = mnemonic + form.syntax
        mask = form.mask()
        operands = form.db_operands()
        in_db = form.in_db and stem not in profile.manifest_only
        if in_db:
            row = rows.get(stem)
            if row is None:
                _reject(f"missing native instruction {stem}")
            if (row["id"] != f"{name}.{stem}" or row["width"] != 32 or row["mask"] != mask
                    or row["value"] != fixed or row["operands"] != operands
                    or row["fields"] != form.db_fields() or row["assembly"] != assembly):
                _reject(f"{stem} does not match the reviewed operand/encoding/assembly layout")
            expected = None if witness is None else compile_semantics(
                witness, Location(f"<native {profile.isa} ArchSem witness>", 1, 1),
                operands, 64, {},
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
            "in_db": in_db,
            "evidence": "exact-archsem" if in_db and witness is not None else "reviewed-isa-manifest",
        }

    isa_db = {key: value for key, value in db.items() if key != "abi_facts"}
    isa_fingerprint = hashlib.sha256(database.dumps(isa_db).encode("ascii")).hexdigest()
    if isa_fingerprint != profile.isa_sha256:
        _reject(f"database fingerprint (excluding ABI facts) differs from the reviewed "
                f"{profile.stdspec}")

    return {
        "name": name, "abi": abi["name"], "instructions": mnemonics,
        "abi_facts": deepcopy(abi), "instruction_facts": facts,
        "fingerprint": fingerprint,
        "provider": _RV64_PROVIDER, "isa": profile.isa, "isa_features": profile.features,
        # These are GNU assembler expansions, not native opcodes or a DB
        # relocation evaluator. PCREL_LO12_I refers to the anchor on AUIPC.
        # Under `.option pic`, `la` uses a GOT_HI20/PCREL_LO12_I pair whose
        # second instruction is LD; the static default remains `lla`.
        "gas_symbol_sequences": {
            "jump": ("AUIPC", "JALR"),
            "call": ("AUIPC", "JALR"),
            "lla": ("AUIPC", "ADDI"),
            "la-pic": ("AUIPC", "LD"),
        },
    }


# An architecture key selects only a qualification routine, never a backend
# by itself. The routine still has to qualify the full database and ABI.
_LP64_GAS_PROFILES = {
    "AMD64": (
        "amd64-sysv-elf-lp64", "amd64",
        "530c92fee80327e1c9a4923144e4ee3996fb6b7226319294f74c9d6ce86f2ede",
        {
            "name": "SysVAMD64", "args": [7, 6, 2, 1, 8, 9],
            "stack_pointer": {"kind": "register", "index": 4},
            "return_address": {"kind": "stack_slot", "offset": 0, "size_bytes": 8},
            "stack_entry_remainder": 8, "stack_arg_offset": 8,
            "caller_saved": [0, 1, 2, 6, 7, 8, 9, 10, 11],
            "callee_saved": [3, 4, 5, 12, 13, 14, 15], "unallocatable": [4],
        },
    ),
    "AArch64": (
        "aarch64-aapcs64-elf-lp64", "aarch64",
        "c6006b2abb10948243e27ea5b5cc00d0cd4e8ab9308c76a16e112610dbcd54ac",
        {
            "name": "AAPCS64", "args": list(range(8)),
            "stack_pointer": {"kind": "state", "name": "SP"},
            "return_address": {"kind": "register", "index": 30},
            "stack_entry_remainder": 0, "stack_arg_offset": 0,
            "caller_saved": [*range(18), 30],
            "callee_saved": list(range(19, 30)), "unallocatable": [18, 31],
        },
    ),
}


def _resolve_lp64_gas(db: dict, fingerprint: str) -> dict:
    name = db["architecture"]
    provider, directory, pin, reviewed = _LP64_GAS_PROFILES[name]

    def reject(reason: str) -> NoReturn:
        raise ArchonError(
            f"native target requires the reviewed {name}/{reviewed['name']} LP64 ArchDB: {reason}"
        )

    abi = db.get("abi_facts_v2")
    if abi is None:
        reject("missing declarative abi_facts_v2")
    expected = {
        "schema_version": 2, "register_file": "IntegerRegisters", "result": 0,
        "stack_align": 16, "stack_slot_bytes": 8, "stack_growth": "downward",
        **reviewed,
    }
    for key, value in expected.items():
        if abi[key] != value:
            reject(f"abi_facts_v2.{key} does not match the reviewed scalar ABI")
    if fingerprint != pin:
        reject(f"database fingerprint differs from the reviewed stdspec/{directory}/root.arch")

    return {
        "name": name, "abi": abi["name"], "abi_facts_v2": deepcopy(abi),
        "fingerprint": fingerprint, "provider": provider,
        "isa": "AMD64" if directory == "amd64" else "Armv8-A A64",
        "isa_features": tuple(db["features"]),
    }


_NATIVE_QUALIFIERS = {
    **dict.fromkeys(_RV64_PROFILES, _resolve_rv64),
    **dict.fromkeys(_LP64_GAS_PROFILES, _resolve_lp64_gas),
}


def resolve_native_target(db: dict) -> dict:
    """Return fresh facts from a reviewed qualifier, or raise ArchonError."""
    serialized = database.dumps(db)
    qualifier = _NATIVE_QUALIFIERS.get(db["architecture"])
    if qualifier is None:
        raise ArchonError(f"no reviewed native lowering provider: architecture is {db['architecture']!r}")
    fingerprint = hashlib.sha256(serialized.encode("ascii")).hexdigest()
    return qualifier(db, fingerprint)
