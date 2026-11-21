"""Opt-in runtime native backend generation alongside the legacy IR pipeline."""

import json
from pathlib import Path
import re

from ..diagnostics import ArchonError

STAGE_ORDER = 1600

_SOURCE_DIR = Path(__file__).with_name("native")
_RV64_SOURCES = (
    "ABI.c", "Backend.c", "CFGLowering.c", "FrameLowering.c", "IntegerConversion.c", "Legalizer.c",
    "MachineIR.c", "MIRText.c", "NativeEmitter.c", "NativeEncoding.c", "NativeFrameVerify.c",
    "NativeHomes.c", "NativeISel.c", "NativeLayout.c", "NativeRegalloc.c", "NativeSchedule.c",
    "NativeVerify.c",
)
_RV64_BASE_MNEMONICS = (
    "ADD", "SUB", "AND", "OR", "XOR",
    "SLL", "SRL", "SRA", "SLT", "SLTU", "ADDI", "SLLI",
    "LB", "LBU", "LH", "LHU", "LW", "LWU", "LD", "SB", "SH", "SW", "SD",
    "JALR", "EBREAK", "BEQ", "BNE", "AUIPC",
)
_RV64_M_MNEMONICS = ("MUL", "DIV", "REM", "DIVU", "REMU")
_GAS_SOURCES = ("Backend.c", "Legalizer.c", "MIRText.c", "NativeVerify.c", "ScalarGAS.c")
_AMD64_REGISTERS = (
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
)


def check_options(options):
    profile = options.get("profile", "legacy")
    if profile not in ("legacy", "native"):
        raise ArchonError("IR-generation profile must be 'legacy' or 'native'")
    abi = options.get("abi")
    if abi is not None and (profile != "native" or not isinstance(abi, str) or not abi):
        raise ArchonError("IR-generation option 'abi' requires a native ABI name and profile")


def _render_abi(abi):
    return (
        '#include "NativeInternal.h"\n\n'
        '/* Values validated by the reviewed RV64 scalar provider. */\n'
        'const ANABI an_abi = {\n'
        f'    .name = "{abi["name"]}",\n'
        f'    .argc = {len(abi["args"])}u,\n'
        '    .args = {' + ', '.join(f'{r}u' for r in abi["args"]) + '},\n'
        + "".join(f'    .{key} = {abi[key]}u,\n' for key in (
            "result", "stack_align", "stack_pointer", "return_address",
        ))
        + '};\n'
    )


def _render_rv64_encodings(facts, opcodes):
    """Bind validated encoding fields to the native emitter's operand slots."""
    rows = []
    for key in opcodes:
        form = facts["instruction_facts"][key]
        by_name = {}
        reg_count, imm_width, imm_signed = 0, 0, 0
        for operand in form["operands"]:
            if operand["kind"] == "reg":
                if reg_count == 3 or operand["width"] != 5 or operand["signed"]:
                    raise ArchonError(f"unsupported native register operands for {key}")
                by_name[operand["name"]] = (reg_count, 5)
                reg_count += 1
            elif operand["kind"] == "imm" and not imm_width and 0 < operand["width"] <= 20:
                imm_width, imm_signed = operand["width"], int(operand["signed"])
                by_name[operand["name"]] = (3, imm_width)
            else:
                raise ArchonError(f"unsupported native immediate operands for {key}")
        fields, source_bits = [], [0] * 4
        encoding_bits = form["mask"]
        if form["value"] & ~encoding_bits or not 0 <= encoding_bits <= 0xffffffff:
            raise ArchonError(f"invalid native fixed bits for {key}")
        for field in form["fields"]:
            slot, source_width = by_name[field["operand"]]
            lsb, width, source_lsb = field["lsb"], field["width"], field["operand_lsb"]
            if not (0 < width <= source_width and 0 <= source_lsb <= source_width - width
                    and 0 <= lsb <= 32 - width):
                raise ArchonError(f"invalid native encoding field for {key}")
            mask = (1 << width) - 1
            if source_bits[slot] & (mask << source_lsb) or encoding_bits & (mask << lsb):
                raise ArchonError(f"overlapping native encoding fields for {key}")
            source_bits[slot] |= mask << source_lsb
            encoding_bits |= mask << lsb
            fields.append(f"{{{slot}u, {lsb}u, {width}u, {source_lsb}u}}")
        if encoding_bits != 0xffffffff or len(fields) > 6 or any(
            source_bits[i] != 31 for i in range(reg_count)
        ):
            raise ArchonError(f"incomplete native encoding for {key}")
        imm_zero_mask = ((1 << imm_width) - 1) ^ source_bits[3]
        if imm_zero_mask != (1 if key in ("BEQ", "BNE") else 0):
            raise ArchonError(f"unsupported missing immediate bits for {key}")
        rows.append(
            f"    [AN_{key}] = {{AN_MN_{key}, UINT32_C(0x{form['mask']:08x}), "
            f"UINT32_C(0x{form['value']:08x}), {(1 << reg_count) - 1}u, {imm_width}u, "
            f"{imm_signed}u, {imm_zero_mask}u, {len(fields)}u,\n"
            f"        {{{', '.join(fields) or '{0, 0, 0, 0}'}}}}},"
        )
    return (
        '#include "NativeInternal.h"\n#include "TargetNative.h"\n\n'
        "const ANEncoding an_encodings[AN_OPCODE_COUNT] = {\n"
        + "\n".join(rows) + "\n};\n"
    )


def _render_adapter_cmake(stem, title):
    return (
        f"add_library(archon_{stem}_ir STATIC IR.c)\n"
        f"add_library(archon_{stem}_backend STATIC {title}Backend.c)\n"
        f"target_link_libraries(archon_{stem}_backend PUBLIC archon_{stem}_ir archon_backend)\n"
        f"set_target_properties(archon_{stem}_ir archon_{stem}_backend PROPERTIES\n"
        "    C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)\n"
        f"set_target_properties(archon_{stem}_ir PROPERTIES EXPORT_NAME {title}IR)\n"
        f"set_target_properties(archon_{stem}_backend PROPERTIES EXPORT_NAME {title}Backend)\n"
        f"add_library(Archon::{title}IR ALIAS archon_{stem}_ir)\n"
        f"add_library(Archon::{title}Backend ALIAS archon_{stem}_backend)\n"
        f"target_include_directories(archon_{stem}_ir PUBLIC\n"
        '    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"\n'
        '    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/archon/backend>")\n'
        f"install(TARGETS archon_{stem}_ir archon_{stem}_backend EXPORT ArchonBackendTargets\n"
        "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
        f"install(FILES {title}Backend.h IR.h DESTINATION ${{CMAKE_INSTALL_INCLUDEDIR}}/archon/backend)\n"
    )


def _render_backend_cmake(sources):
    return (
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(ArchonNativeBackend LANGUAGES C)\n"
        "add_library(archon_backend STATIC\n"
        + "".join(f"    {name}\n" for name in sources) + ")\n"
        "set_target_properties(archon_backend PROPERTIES C_STANDARD 99 "
        "C_STANDARD_REQUIRED YES C_EXTENSIONS NO EXPORT_NAME Backend)\n"
        "add_library(Archon::Backend ALIAS archon_backend)\n"
        "add_executable(archon-backend NativeDriver.c)\n"
        "target_link_libraries(archon-backend PRIVATE archon_backend)\n"
        "set_target_properties(archon-backend PROPERTIES C_STANDARD 99 "
        "C_STANDARD_REQUIRED YES C_EXTENSIONS NO)\n"
        "include(GNUInstallDirs)\n"
        "target_include_directories(archon_backend PUBLIC\n"
        '    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"\n'
        '    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/archon/backend>")\n'
        "install(TARGETS archon_backend EXPORT ArchonBackendTargets\n"
        "    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})\n"
        "install(EXPORT ArchonBackendTargets NAMESPACE Archon:: FILE ArchonBackendConfig.cmake\n"
        "    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ArchonBackend)\n"
        "install(TARGETS archon-backend RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})\n"
        "install(FILES Backend.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/archon/backend)\n"
        "install(FILES BackendManifest.json DESTINATION ${CMAKE_INSTALL_DATADIR}/archon/backend)\n"
    )


def _emit_rv64(facts, archdb_version):
    target = facts["name"]
    abi = facts["abi"]
    isa = facts["isa"]
    has_m = "M" in facts["isa_features"]
    opcodes = _RV64_BASE_MNEMONICS + (_RV64_M_MNEMONICS if has_m else ())
    mnemonics = facts["instructions"]
    definitions = []
    for key in opcodes:
        name = mnemonics.get(key)
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9]*", name):
            raise ArchonError(f"native target has no validated GAS mnemonic for {key}")
        definitions.append(f'#define AN_MN_{key} "{name}"')
    fingerprint = facts["fingerprint"]
    manifest = {
        "schema_version": 1,
        "backend_abi_version": 1,
        "machineir_schema_version": 1,
        "target": target,
        "abi": abi,
        "archdb_version": archdb_version,
        "archdb_sha256": fingerprint,
        "provider": facts["provider"],
        "isa": isa,
        "isa_features": facts["isa_features"],
        "abi_facts": facts["abi_facts"],
        "instruction_evidence": {
            stem: {"archdb_encoding": row["in_db"], "basis": row["evidence"]}
            for stem, row in facts["instruction_facts"].items()
        },
        "input": ["in-memory generic MachineIR v1", "canonical MachineIR text v1"],
        "artifact": [
            "ELF-oriented GAS assembly (default or PIC)",
            "physical instruction inspection text v1",
            f"raw {isa} machine code (one function, no imports or globals)",
        ],
        "floating_point": ({
            "formats": ["binary32", "binary64"],
            "abi": "LP64 soft-float: integer registers and stack",
            "mir": ["fconst", "fload", "fstore", "fneg", "feq", "flt", "fle"],
            "limits": "no runtime arithmetic other than negation, conversions, or FP environment",
        } if has_m else {"formats": [], "abi": "LP64 soft-float; FP MIR is unsupported"}),
        "address_width": 64,
        "endian": "little",
        "schedule": ["input order (default)", "verified pure-region ordering; no latency model"],
        "pic": "assembly-only .option pic with GAS la address materialization",
        "optimizer": "none",
        "oracle": False,
    }
    if has_m:
        manifest["variadic"] = {
            "mir": ["rv64_va_start"],
            "abi": "LP64 scalar XLEN slots with a contiguous integer-register save area",
        }
    else:
        manifest["expanded_mir"] = {"mul": "RV64I shift-and-add modulo 2^64"}
        manifest["unsupported_mir"] = [
            "div_s", "rem_s", "div_u", "rem_u", "div_checked_s", "mul_checked_s",
        ]
    files = {name: (_SOURCE_DIR / name).read_text(encoding="utf-8")
              for name in (*_RV64_SOURCES, "NativeDriver.c", "NativeInternal.h", "Backend.h")}
    files["NativeABIData.c"] = _render_abi(facts["abi_facts"])
    files["NativeEncodingData.c"] = _render_rv64_encodings(facts, opcodes)
    files["TargetNative.h"] = (
        "#ifndef ARCHON_TARGET_NATIVE_H\n#define ARCHON_TARGET_NATIVE_H\n"
        f'#define AN_TARGET_NAME "{target}/{abi}"\n'
        f'#define AN_TARGET_SHA256 "{fingerprint}"\n'
        f"#define AN_RV64_HAS_M {int(has_m)}\n"
        + ("#define AN_RV64_SOFT_FP_TRANSPORT 1\n#define AN_RV64_STDARG 1\n" if has_m else "")
        + "\n".join(definitions) + "\n#endif\n"
    )
    files["CMakeLists.txt"] = _render_backend_cmake(
        (*_RV64_SOURCES, "NativeABIData.c", "NativeEncodingData.c"),
    )
    return files, manifest


def _emit_scalar_gas(facts, archdb_version, *, registers, emitter, pic):
    abi = facts["abi_facts_v2"]
    fingerprint = facts["fingerprint"]
    stack = abi["stack_pointer"]
    ret = abi["return_address"]
    sources = (*_GAS_SOURCES, emitter, "NativeABIData.c")
    files = {name: (_SOURCE_DIR / name).read_text(encoding="utf-8")
             for name in (*_GAS_SOURCES, emitter, "NativeDriver.c", "NativeInternal.h",
                          "Backend.h", "ScalarGAS.h")}
    files["TargetNative.h"] = (
        "#ifndef ARCHON_TARGET_NATIVE_H\n#define ARCHON_TARGET_NATIVE_H\n"
        f'#define AN_TARGET_NAME "{facts["name"]}/{facts["abi"]}"\n'
        f'#define AN_TARGET_SHA256 "{fingerprint}"\n'
        "#define AN_SCALAR_GAS 1\n"
        "#define AN_SCALAR_GAS_PIC 1\n"
        "#define AN_SCALAR_GAS_FP 1\n"
        + ("#define AN_SYSV_AMD64 1\n" if emitter == "AMD64GAS.c" else "")
        + ("#define AN_AAPCS64 1\n" if emitter == "AArch64GAS.c" else "")
        + "#endif\n"
    )
    stack_reg = registers[stack["index"]] if stack["kind"] == "register" else "sp"
    link = f'"{registers[ret["index"]]}"' if ret["kind"] == "register" else "NULL"
    files["NativeABIData.c"] = (
        '#include "ScalarGAS.h"\n\n'
        '/* ABI v2 data qualified and pinned by native_target.py. The common MIR\n'
        ' * verifier only needs the ABI name and local/stack alignment. */\n'
        'const ANABI an_abi = {\n'
        f'    .name = "{abi["name"]}", .stack_align = {abi["stack_align"]}u,\n'
        '};\n'
        'const ANGasABI an_gas_abi = {\n'
        f'    .argc = {len(abi["args"])}u,\n'
        '    .fp_argc = 8u, /* Reviewed scalar FP bank; not modeled by ABI v2. */\n'
        '    .args = {' + ', '.join(f'"{registers[r]}"' for r in abi["args"]) + '},\n'
        + f'    .result = "{registers[abi["result"]]}", .sp = "{stack_reg}", .link = {link},\n'
        + "".join(f'    .{key} = {abi[key]}u,\n' for key in (
            "stack_entry_remainder", "stack_slot_bytes", "stack_arg_offset",
        ))
        + '};\n'
    )
    files["CMakeLists.txt"] = _render_backend_cmake(sources)
    return files, {
        "schema_version": 1,
        "backend_abi_version": 1,
        "machineir_schema_version": 1,
        "target": facts["name"],
        "abi": facts["abi"],
        "archdb_version": archdb_version,
        "archdb_sha256": fingerprint,
        "provider": facts["provider"],
        "isa": facts["isa"],
        "isa_features": facts["isa_features"],
        "abi_facts_v2": abi,
        "instruction_evidence": "reviewed baseline GAS sequences; not derived encodings or ArchSem",
        "input": ["in-memory generic MachineIR v1", "canonical MachineIR text v1"],
        "artifact": ["ELF-oriented GAS assembly (default or PIC)"],
        "floating_point": {
            "formats": ["IEEE binary32", "IEEE binary64"],
            "abi": "reviewed fixed-arity scalar FP bank (registers 0 through 7, result 0)",
        },
        "address_width": 64,
        "endian": "little",
        "schedule": ["input order only"],
        "stack_frame_limit_bytes": 65536,
        "pic": pic,
        "optimizer": "none",
        "oracle": False,
    }


def _emit_amd64(facts, archdb_version):
    return _emit_scalar_gas(facts, archdb_version, emitter="AMD64GAS.c",
                            pic="assembly-only via RIP-relative GOTPCREL addresses and ELF direct calls",
                            registers=tuple("%" + register for register in _AMD64_REGISTERS))


def _emit_aarch64(facts, archdb_version):
    return _emit_scalar_gas(facts, archdb_version, emitter="AArch64GAS.c",
                            pic="assembly-only via page-relative GOT addresses and ELF direct calls",
                            registers=tuple(f"x{i}" for i in range(31)))


# The provider owns the lowering sources and generated encoding/ABI data.
# Architecture spellings and arbitrary ArchDB records cannot select C code.
_NATIVE_EMITTERS = {
    "riscv64-elf-lp64": _emit_rv64,
    "amd64-sysv-elf-lp64": _emit_amd64,
    "aarch64-aapcs64-elf-lp64": _emit_aarch64,
}


def emit(context):
    options = context["options"]
    check_options(options)
    if options.get("profile") != "native":
        return {}

    from .native_target import resolve_native_target

    db = context["database"]
    if db is None:
        raise ArchonError("native backend generation requires a compiled ArchDB")
    facts = resolve_native_target(db)
    if options.get("abi", facts["abi"]) != facts["abi"]:
        raise ArchonError("compiled target does not support the requested native ABI")
    provider = _NATIVE_EMITTERS.get(facts["provider"])
    if provider is None:
        raise ArchonError("no native lowering provider for this architecture and ABI")
    files, manifest = provider(facts, db["version"])
    if context["ir"] == "gated":
        from .gated_native import emit_gated_adapter

        files.update(emit_gated_adapter(context))
        files["CMakeLists.txt"] += _render_adapter_cmake("gated", "Gated")
        manifest["input"].append("in-memory Gated SSA single-block scalar adapter")
    elif context["ir"] == "typed-tac":
        from .typed_tac_native import emit_typed_tac_adapter

        files.update(emit_typed_tac_adapter(context))
        files["CMakeLists.txt"] += _render_adapter_cmake("typed_tac", "TypedTAC")
        manifest["input"].append("in-memory Typed TAC scalar control-flow adapter")
    files["BackendManifest.json"] = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    return files
