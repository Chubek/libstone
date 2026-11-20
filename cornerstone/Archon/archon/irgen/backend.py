"""Opt-in runtime native backend generation alongside the legacy IR pipeline."""

import json
from pathlib import Path
import re

from ..diagnostics import ArchonError

STAGE_ORDER = 1600

_SOURCE_DIR = Path(__file__).with_name("native")
_SOURCES = (
    "ABI.c", "Backend.c", "CFGLowering.c", "FrameLowering.c", "IntegerConversion.c", "Legalizer.c",
    "MachineIR.c", "MIRText.c", "NativeEmitter.c", "NativeEncoding.c", "NativeFrameVerify.c",
    "NativeHomes.c", "NativeISel.c", "NativeLayout.c", "NativeRegalloc.c", "NativeSchedule.c",
    "NativeVerify.c",
)
_REQUIRED_MNEMONICS = (
    "ADD", "SUB", "MUL", "DIV", "REM", "DIVU", "REMU", "AND", "OR", "XOR",
    "SLL", "SRL", "SRA", "SLT", "SLTU", "ADDI", "SLLI", "LD", "SD",
    "JALR", "EBREAK", "BEQ", "BNE", "AUIPC",
)


def check_options(options):
    profile = options.get("profile", "legacy")
    if profile not in ("legacy", "native"):
        raise ArchonError("IR-generation profile must be 'legacy' or 'native'")
    abi = options.get("abi")
    if abi is not None and (profile != "native" or abi != "LP64"):
        raise ArchonError("IR-generation option 'abi' requires native profile and LP64")


def _render_abi(abi):
    return (
        '#include "NativeInternal.h"\n\n'
        '/* Values validated by the reviewed RV64IM scalar provider. */\n'
        'const ANABI an_abi = {\n'
        f'    .name = "{abi["name"]}",\n'
        f'    .argc = {len(abi["args"])}u,\n'
        '    .args = {' + ', '.join(f'{r}u' for r in abi["args"]) + '},\n'
        + "".join(f'    .{key} = {abi[key]}u,\n' for key in (
            "result", "stack_align", "stack_pointer", "return_address",
        ))
        + '};\n'
    )


def _render_encodings(facts):
    """Bind validated encoding fields to the native emitter's operand slots."""
    rows = []
    for key in _REQUIRED_MNEMONICS:
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


def emit(context):
    options = context["options"]
    check_options(options)
    if options.get("profile") != "native":
        return {}

    from .native_target import resolve_native_target

    db = context["database"]
    if db is None:
        raise ArchonError("native backend generation requires a compiled RV64IM ArchDB")
    facts = resolve_native_target(db)
    if facts["abi"] != options.get("abi", "LP64"):
        raise ArchonError("compiled target does not support the requested native ABI")
    target = facts["name"]
    abi = facts["abi"]
    if target != "RISCV64IM" or abi != "LP64":
        raise ArchonError("no native lowering provider for this architecture and ABI")
    mnemonics = facts["instructions"]
    definitions = []
    for key in _REQUIRED_MNEMONICS:
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
        "archdb_version": db["version"],
        "archdb_sha256": fingerprint,
        "abi_facts": facts["abi_facts"],
        "instruction_evidence": {
            stem: {"archdb_encoding": row["in_db"], "basis": row["evidence"]}
            for stem, row in facts["instruction_facts"].items()
        },
        "input": ["in-memory generic MachineIR v1", "canonical MachineIR text v1"],
        "artifact": [
            "static ELF-oriented GAS assembly", "physical instruction inspection text v1",
            "raw RV64IM machine code (one function, no imports or globals)",
        ],
        "address_width": 64,
        "endian": "little",
        "schedule": ["input order (default)", "verified pure-region ordering; no latency model"],
        "optimizer": "none",
        "oracle": False,
    }
    files = {name: (_SOURCE_DIR / name).read_text(encoding="utf-8")
             for name in (*_SOURCES, "NativeDriver.c", "NativeInternal.h", "Backend.h")}
    files["NativeABIData.c"] = _render_abi(facts["abi_facts"])
    files["NativeEncodingData.c"] = _render_encodings(facts)
    files["TargetNative.h"] = (
        "#ifndef ARCHON_TARGET_NATIVE_H\n#define ARCHON_TARGET_NATIVE_H\n"
        f'#define AN_TARGET_NAME "{target}/{abi}"\n'
        f'#define AN_TARGET_SHA256 "{fingerprint}"\n'
        + "\n".join(definitions) + "\n#endif\n"
    )
    files["CMakeLists.txt"] = (
        "cmake_minimum_required(VERSION 3.16)\n"
        "project(ArchonNativeBackend LANGUAGES C)\n"
        "add_library(archon_backend STATIC\n"
        + "".join(f"    {name}\n" for name in (*_SOURCES, "NativeABIData.c", "NativeEncodingData.c")) + ")\n"
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
