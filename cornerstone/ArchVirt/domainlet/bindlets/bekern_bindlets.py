"""
bekern_bindlets — Bindlet wrappers for bekern.* domainlets.

Exposes architecture, ISEL, codegen, schedule, regalloc, peephole, and
layout backend compiler kernels through the unified AbstractDomainlet interface.
"""

from __future__ import annotations

import json
from typing import Any

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    Confidence,
    DomainletCapability,
    DomainletInfo,
    InputKind,
    Severity,
)


# ═══════════════════════════════════════════════════════════════════
#  Base Bekern Bindlet
# ═══════════════════════════════════════════════════════════════════

class BaseBekernBindlet(AbstractDomainlet):
    """Shared base for all bekern bindlets."""

    _kernel_module: str = ""
    _category: str = "bekern"

    def _get_kernel(self):
        import importlib
        return importlib.import_module(self._kernel_module).KERNEL

    def _run_operation(self, operation: str, payload: dict[str, Any]) -> dict[str, Any]:
        kernel = self._get_kernel()
        result = kernel.run(payload, operation=operation)
        if result["result"]["status"] != "ok":
            diags = result["result"].get("diagnostics", [])
            msg = "; ".join(d.get("message", "unknown") for d in diags)
            raise RuntimeError(f"{self._kernel_module}: {msg}")
        return result["result"]["value"]

    def _run_operation_raw(self, operation: str, payload: dict[str, Any]) -> dict[str, Any]:
        """Run an operation without status checking, returning the full result dict."""
        kernel = self._get_kernel()
        result = kernel.run(payload, operation=operation)
        return result["result"]


# ═══════════════════════════════════════════════════════════════════
#  Architecture Bindlets
# ═══════════════════════════════════════════════════════════════════

_ARCH_MODULES = {
    "amd64_encoding": "domainlet.bekern.arch.amd64_encoding",
    "amd64_abi": "domainlet.bekern.arch.amd64_abi",
    "amd64_avx": "domainlet.bekern.arch.amd64_avx",
    "amd64_avx2": "domainlet.bekern.arch.amd64_avx2",
    "amd64_avx512": "domainlet.bekern.arch.amd64_avx512",
    "amd64_fma": "domainlet.bekern.arch.amd64_fma",
    "amd64_simd": "domainlet.bekern.arch.amd64_simd",
    "amd64_fp": "domainlet.bekern.arch.amd64_fp",
    "amd64_bitmanip": "domainlet.bekern.arch.amd64_bitmanip",
    "amd64_crypto": "domainlet.bekern.arch.amd64_crypto",
    "aarch64_encoding": "domainlet.bekern.arch.aarch64_encoding",
    "aarch64_abi": "domainlet.bekern.arch.aarch64_abi",
    "aarch64_sve": "domainlet.bekern.arch.aarch64_sve",
    "arm32_abi": "domainlet.bekern.arch.arm32_abi",
    "arm32_thumb2": "domainlet.bekern.arch.arm32_thumb2",
    "amdgpu_abi": "domainlet.bekern.arch.amdgpu_abi",
    "amdgpu_binformat": "domainlet.bekern.arch.amdgpu_binformat",
    "amdgpu_compute": "domainlet.bekern.arch.amdgpu_compute",
    "amdgpu_encoding": "domainlet.bekern.arch.amdgpu_encoding",
    "avr_encoding": "domainlet.bekern.arch.avr_encoding",
    "avr_abi": "domainlet.bekern.arch.avr_abi",
    "bfd_object": "domainlet.bekern.arch.bfd_object",
    "bfd_sections": "domainlet.bekern.arch.bfd_sections",
    "bfd_symbols": "domainlet.bekern.arch.bfd_symbols",
    "bfd_targets": "domainlet.bekern.arch.bfd_targets",
    "bpf_encoding": "domainlet.bekern.arch.bpf_encoding",
    "bpf_abi": "domainlet.bekern.arch.bpf_abi",
    "directx_abi": "domainlet.bekern.arch.directx_abi",
    "directx_binformat": "domainlet.bekern.arch.directx_binformat",
    "directx_compute": "domainlet.bekern.arch.directx_compute",
    "directx_encoding": "domainlet.bekern.arch.directx_encoding",
    "loongarch64_encoding": "domainlet.bekern.arch.loongarch64_encoding",
    "loongarch64_abi": "domainlet.bekern.arch.loongarch64_abi",
    "mips64_encoding": "domainlet.bekern.arch.mips64_encoding",
    "mips64_abi": "domainlet.bekern.arch.mips64_abi",
    "ppc64_encoding": "domainlet.bekern.arch.ppc64_encoding",
    "ppc64_abi": "domainlet.bekern.arch.ppc64_abi",
    "nvptx_abi": "domainlet.bekern.arch.nvptx_abi",
    "nvptx_binformat": "domainlet.bekern.arch.nvptx_binformat",
    "nvptx_compute": "domainlet.bekern.arch.nvptx_compute",
    "nvptx_encoding": "domainlet.bekern.arch.nvptx_encoding",
    "riscv64_encoding": "domainlet.bekern.arch.riscv64_encoding",
    "riscv64_abi": "domainlet.bekern.arch.riscv64_abi",
    "riscv64_rvv": "domainlet.bekern.arch.riscv64_rvv",
    "sparc64_encoding": "domainlet.bekern.arch.sparc64_encoding",
    "sparc64_abi": "domainlet.bekern.arch.sparc64_abi",
    "spirv_abi": "domainlet.bekern.arch.spirv_abi",
    "spirv_binformat": "domainlet.bekern.arch.spirv_binformat",
    "spirv_compute": "domainlet.bekern.arch.spirv_compute",
    "spirv_encoding": "domainlet.bekern.arch.spirv_encoding",
    "tricore_encoding": "domainlet.bekern.arch.tricore_encoding",
    "tricore_abi": "domainlet.bekern.arch.tricore_abi",
    "wasm_encoding": "domainlet.bekern.arch.wasm_encoding",
    "wasm_abi": "domainlet.bekern.arch.wasm_abi",
    "s390x_encoding": "domainlet.bekern.arch.s390x_encoding",
    "s390x_abi": "domainlet.bekern.arch.s390x_abi",
    "s390x_binformat": "domainlet.bekern.arch.s390x_binformat",
    "m68k_encoding": "domainlet.bekern.arch.m68k_encoding",
    "m68k_abi": "domainlet.bekern.arch.m68k_abi",
    "m68k_binformat": "domainlet.bekern.arch.m68k_binformat",
    "hexagon_encoding": "domainlet.bekern.arch.hexagon_encoding",
    "hexagon_abi": "domainlet.bekern.arch.hexagon_abi",
    "hexagon_binformat": "domainlet.bekern.arch.hexagon_binformat",
}


_ARCH_DEFAULT_OPERATIONS = {
    "amd64_encoding": "encode_x86",
    "aarch64_encoding": "encode_a64",
    "riscv64_encoding": "encode_rv64i",
    "wasm_encoding": "encode_leb128",
    "avr_encoding": "encode_avr16",
    "bpf_encoding": "encode_bpf64",
    "loongarch64_encoding": "encode_la64",
    "mips64_encoding": "encode_mips64",
    "ppc64_encoding": "encode_ppc32",
    "sparc64_encoding": "encode_sparc",
    "tricore_encoding": "encode_tc32",
    "arm32_thumb2": "encode_t16",
    "spirv_encoding": "encode_spirv_words",
    "directx_encoding": "encode_dxil",
    "amdgpu_encoding": "encode_amdgpu",
    "nvptx_encoding": "encode_ptx",
    "s390x_encoding": "encode_s390x",
    "m68k_encoding": "encode_m68k",
    "hexagon_encoding": "encode_hexagon",
}


def _context_payload(data: Any) -> dict[str, Any]:
    if isinstance(data, dict):
        return dict(data)
    if isinstance(data, str) and data:
        try:
            loaded = json.loads(data)
        except json.JSONDecodeError:
            return {"data": data}
        if isinstance(loaded, dict):
            return dict(loaded)
        return {"data": loaded}
    return {}


def _arch_kernel(module_name: str):
    import importlib

    return importlib.import_module(_ARCH_MODULES[module_name]).KERNEL


def _arch_operation(module_name: str, requested: str | None = None) -> str:
    if requested:
        return requested
    default = _ARCH_DEFAULT_OPERATIONS.get(module_name)
    if default is not None:
        return default
    return _arch_kernel(module_name).operations[0]


class ArchBindlet(BaseBekernBindlet):
    """Dynamic bindlet for all bekern.arch.* encoding and ABI kernels."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.arch",
            version="1.0.0",
            description="Architecture encoding, ABI, and SIMD extension kernels for multiple target ISAs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="list_archs",
                    description="List all available architecture backend modules",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["backend", "arch", "discovery"],
                ),
                DomainletCapability(
                    name="encode",
                    description="Dispatch an architecture-specific encode operation",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "arch", "encode"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "list_archs")
        self._arch_module = config.get("arch", "")
        self._operation = config.get("operation", "")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        if self._capability == "list_archs":
            return [
                AnalysisIssue(
                    rule_id="bekern.arch.list",
                    message=json.dumps(sorted(_ARCH_MODULES.keys())),
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
            ]

        if not self._arch_module or self._arch_module not in _ARCH_MODULES:
            return [
                AnalysisIssue(
                    rule_id="bekern.arch.error",
                    message=f"Unknown arch module: {self._arch_module!r}. Available: {sorted(_ARCH_MODULES.keys())}",
                    severity=Severity.ERROR,
                    confidence=Confidence.HIGH,
                )
            ]

        payload = _context_payload(context.data)
        payload.update({key: value for key, value in context.options.items() if value is not None})
        kernel = _arch_kernel(self._arch_module)
        operation = _arch_operation(self._arch_module, self._operation)
        value = kernel.run(payload, operation=operation)["result"]["value"]
        return [
            AnalysisIssue(
                rule_id=f"bekern.arch.{self._arch_module}.{operation}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
                extra={
                    "supported": "1",
                    "arch_module": self._arch_module,
                    "operation": operation,
                    "kernel": kernel.name,
                },
            )
        ]


class DomasAssemblerBindlet(BaseBekernBindlet):
    """Backend gate for DomAS using the bekern.arch kernels."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="domas.backend",
            version="1.0.0",
            description="DomAS architecture gate backed by bekern.arch domainlet kernels",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="list_archs",
                    description="List the supported architecture kernels",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["domas", "backend", "arch", "discovery"],
                ),
                DomainletCapability(
                    name="validate_instruction",
                    description="Validate and normalize a single assembly instruction against the selected architecture kernel",
                    input_kind=InputKind.AST_JSON,
                    tags=["domas", "backend", "arch", "validate"],
                ),
                DomainletCapability(
                    name="encode",
                    description="Encode one textual instruction through the selected architecture Domainlet",
                    input_kind=InputKind.AST_JSON,
                    tags=["domas", "backend", "arch", "encode"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "validate_instruction")
        self._arch_module = config.get("arch", "amd64_encoding")
        self._operation = config.get("operation", "")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        if self._capability == "list_archs":
            return [
                AnalysisIssue(
                    rule_id="domas.backend.list",
                    message=json.dumps(sorted(_ARCH_MODULES.keys())),
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
            ]

        if not self._arch_module or self._arch_module not in _ARCH_MODULES:
            return [
                AnalysisIssue(
                    rule_id="domas.backend.error",
                    message=f"Unknown arch module: {self._arch_module!r}. Available: {sorted(_ARCH_MODULES.keys())}",
                    severity=Severity.ERROR,
                    confidence=Confidence.HIGH,
                )
            ]

        payload = _context_payload(context.data)
        payload.update({key: value for key, value in context.options.items() if value is not None})
        module_name = str(context.options.get("arch", self._arch_module)) or self._arch_module
        if module_name not in _ARCH_MODULES:
            return [
                AnalysisIssue(
                    rule_id="domas.backend.error",
                    message=f"Unknown arch module: {module_name!r}. Available: {sorted(_ARCH_MODULES.keys())}",
                    severity=Severity.ERROR,
                    confidence=Confidence.HIGH,
                )
            ]

        kernel = _arch_kernel(module_name)
        operation = _arch_operation(module_name, str(context.options.get("operation", self._operation)) or self._operation)
        if operation == "validate_domas_instruction":
            mnemonic = str(context.options.get("mnemonic", "")).strip()
            if not mnemonic:
                return [AnalysisIssue(rule_id="domas.backend.validation", message="empty instruction mnemonic", severity=Severity.ERROR, confidence=Confidence.HIGH, extra={"supported":"0"})]
            return [AnalysisIssue(rule_id=f"domas.backend.{module_name}.validation", message=mnemonic, severity=Severity.INFORMATION, confidence=Confidence.HIGH, extra={"supported":"1", "arch_module":module_name, "operation":operation, "kernel":_arch_kernel(module_name).name})]
        result = kernel.run(payload, operation=operation)
        status = result["result"]["status"]
        supported = "1" if status == "ok" else "0"
        extra = {
            "supported": supported,
            "arch_module": module_name,
            "operation": operation,
            "kernel": kernel.name,
        }
        if supported == "0":
            diagnostics = result["result"].get("diagnostics", [])
            message = "; ".join(item.get("message", "unknown") for item in diagnostics) or "backend validation failed"
            severity = Severity.ERROR
        else:
            value = result["result"].get("value", {})
            if isinstance(value, dict) and "hex" in value:
                blob = value.get("hex", "")
                assembly = ".byte " + ", ".join("0x" + blob[i:i + 2] for i in range(0, len(blob), 2)) + "\n"
                for reloc in value.get("relocations", []):
                    assembly += ".reloc %s, %s, %s, %s\n" % (
                        reloc.get("offset", 0), reloc.get("type", "abs"),
                        reloc.get("symbol", ""), reloc.get("addend", 0))
                message = assembly
                extra["assembly"] = "1"
            else:
                message = json.dumps(value)
            severity = Severity.INFORMATION
        return [
            AnalysisIssue(
                rule_id=f"domas.backend.{module_name}.{operation}",
                message=message,
                severity=severity,
                confidence=Confidence.HIGH,
                extra=extra,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  ISEL Bindlets
# ═══════════════════════════════════════════════════════════════════

class InstructionSelectionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.instruction_selection"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.instruction_selection",
            version="1.0.0",
            description="Match IR DAG patterns to target machine instructions via pattern matching",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="match_patterns",
                    description="Match IR DAG patterns against target descriptions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "pattern"],
                ),
                DomainletCapability(
                    name="select_instructions",
                    description="Select target instructions for matched patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "selection"],
                ),
                DomainletCapability(
                    name="cover_dag",
                    description="Cover the entire IR DAG with selected instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "cover"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "select_instructions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.instruction_selection.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TreePatternSelBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.tree_pattern_sel"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.tree_pattern_sel",
            version="1.0.0",
            description="Tree-pattern instruction selection using BURS or TWIG-style matching",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_tcost",
                    description="Build tree cost model for pattern matching",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "tree"],
                ),
                DomainletCapability(
                    name="apply_rule",
                    description="Apply a tree-pattern rule to a node",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "rule"],
                ),
                DomainletCapability(
                    name="tree_match",
                    description="Match tree patterns against the IR tree",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "match"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "tree_match")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.tree_pattern_sel.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class InstructionCombineBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.instruction_combine"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.instruction_combine",
            version="1.0.0",
            description="Combine adjacent machine operations into fused/target-specific instructions",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="combine_ops",
                    description="Combine adjacent machine operations",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "combine"],
                ),
                DomainletCapability(
                    name="match_fused_op",
                    description="Match fused operation patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "fused"],
                ),
                DomainletCapability(
                    name="simplify_sequence",
                    description="Simplify a sequence of machine operations",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "simplify"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "combine_ops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.instruction_combine.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class OptimalTilingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.optimal_tiling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.optimal_tiling",
            version="1.0.0",
            description="Optimal DAG tiling via dynamic programming for instruction cover selection",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="tile_dag",
                    description="Tile a DAG with instruction patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "tiling"],
                ),
                DomainletCapability(
                    name="compute_tile_cost",
                    description="Compute the cost of a tile cover",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "cost"],
                ),
                DomainletCapability(
                    name="cover_optimally",
                    description="Find the optimal tile cover for a DAG",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "optimal"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "cover_optimally")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.optimal_tiling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class LegalizationBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.legalization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.legalization",
            version="1.0.0",
            description="Legalize IR to conform to target constraints",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="legalize_ir",
                    description="Legalize IR to target constraints",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "legalize"],
                ),
                DomainletCapability(
                    name="expand_pseudo",
                    description="Expand pseudo-instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "pseudo"],
                ),
                DomainletCapability(
                    name="lower_invalid",
                    description="Lower invalid operations for the target",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "lower"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "legalize_ir")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.legalization.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class PostISELExpansionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.isel.post_isel_expansion"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.isel.post_isel_expansion",
            version="1.0.0",
            description="Expand pseudo-instructions and materialize large immediates after instruction selection",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="expand_pseudo_inst",
                    description="Expand pseudo-instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "pseudo"],
                ),
                DomainletCapability(
                    name="lower_const_pool",
                    description="Lower constant pool references",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "constant"],
                ),
                DomainletCapability(
                    name="materialize_large_imm",
                    description="Materialize large immediate values",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "isel", "immediate"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "expand_pseudo_inst")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.isel.post_isel_expansion.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Codegen Bindlets
# ═══════════════════════════════════════════════════════════════════

class CallingConventionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.codegen.calling_convention"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.codegen.calling_convention",
            version="1.0.0",
            description="Apply calling convention: assign argument/return registers, emit call sequences",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="assign_args",
                    description="Assign argument registers per ABI",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "calling"],
                ),
                DomainletCapability(
                    name="lower_call",
                    description="Lower a call instruction to machine sequences",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "call"],
                ),
                DomainletCapability(
                    name="lower_return",
                    description="Lower a return instruction to machine sequences",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "return"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "lower_call")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.codegen.calling_convention.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class FrameLoweringBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.codegen.frame_lowering"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.codegen.frame_lowering",
            version="1.0.0",
            description="Lower abstract frame objects to concrete stack offsets and spill/fill slots",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="lower_frame",
                    description="Lower abstract frame to concrete stack layout",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "frame"],
                ),
                DomainletCapability(
                    name="assign_stack_slots",
                    description="Assign stack slots to frame objects",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "stack"],
                ),
                DomainletCapability(
                    name="compute_frame_size",
                    description="Compute total frame size",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "frame"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "lower_frame")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.codegen.frame_lowering.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class EmissionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.codegen.emission"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.codegen.emission",
            version="1.0.0",
            description="Emit assembly or binary encoding from machine IR with prologue/epilogue generation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_instructions",
                    description="Emit machine instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "emit"],
                ),
                DomainletCapability(
                    name="emit_prologue",
                    description="Emit function prologue",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "prologue"],
                ),
                DomainletCapability(
                    name="emit_epilogue",
                    description="Emit function epilogue",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "epilogue"],
                ),
                DomainletCapability(
                    name="emit_data_sections",
                    description="Emit data sections",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "data"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_instructions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.codegen.emission.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class AssemblerEmissionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.codegen.assembler_emission"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.codegen.assembler_emission",
            version="1.0.0",
            description="Pretty-print machine IR as assembly text with assembler directives and labels",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="print_asm",
                    description="Print assembly text from machine IR",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "assembly"],
                ),
                DomainletCapability(
                    name="print_directives",
                    description="Print assembler directives",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "directives"],
                ),
                DomainletCapability(
                    name="print_symbol_table",
                    description="Print the symbol table",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "symbols"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "print_asm")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.codegen.assembler_emission.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class BinaryEncodingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.codegen.binary_encoding"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.codegen.binary_encoding",
            version="1.0.0",
            description="Binary encoding: map assembly mnemonics to machine code words with relocation entries",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="encode_instruction",
                    description="Encode an instruction to machine code",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "encode"],
                ),
                DomainletCapability(
                    name="encode_relocation",
                    description="Encode a relocation entry",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "relocation"],
                ),
                DomainletCapability(
                    name="build_binary",
                    description="Build the final binary from encoded instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "codegen", "binary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "encode_instruction")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.codegen.binary_encoding.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Schedule Bindlets
# ═══════════════════════════════════════════════════════════════════

class ListSchedulingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.schedule.list_scheduling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.schedule.list_scheduling",
            version="1.0.0",
            description="List scheduling: prioritize ready operations by critical-path length and issue them in order",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="prioritize_ready_ops",
                    description="Prioritize ready operations by critical path",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "list"],
                ),
                DomainletCapability(
                    name="schedule_dag",
                    description="Schedule a DAG of operations",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "dag"],
                ),
                DomainletCapability(
                    name="emit_schedule",
                    description="Emit the final schedule",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "emit"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "schedule_dag")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.schedule.list_scheduling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class ModuloSchedulingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.schedule.modulo_scheduling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.schedule.modulo_scheduling",
            version="1.0.0",
            description="Software pipelining via modulo scheduling: find minimal initiation interval and place operations",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_ii",
                    description="Compute the minimal initiation interval",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "modulo"],
                ),
                DomainletCapability(
                    name="modulo_place",
                    description="Place operations in the modulo schedule",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "modulo"],
                ),
                DomainletCapability(
                    name="unroll_kernel",
                    description="Unroll the pipelined kernel",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "unroll"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "modulo_place")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.schedule.modulo_scheduling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class BundleSchedulingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.schedule.bundle_scheduling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.schedule.bundle_scheduling",
            version="1.0.0",
            description="VLIW bundle scheduling: pack independent operations into issue bundles",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="pack_bundles",
                    description="Pack independent operations into bundles",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "bundle"],
                ),
                DomainletCapability(
                    name="check_bundle_constraints",
                    description="Check bundle constraints",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "constraints"],
                ),
                DomainletCapability(
                    name="emit_vliw",
                    description="Emit VLIW bundles",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "vliw"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "pack_bundles")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.schedule.bundle_scheduling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class HazardRecognitionBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.schedule.hazard_recognition"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.schedule.hazard_recognition",
            version="1.0.0",
            description="Detect pipeline hazards (data, structural, control) and insert stalls or reorder",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="detect_data_hazards",
                    description="Detect data hazards",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "hazard"],
                ),
                DomainletCapability(
                    name="detect_structural_hazards",
                    description="Detect structural hazards",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "hazard"],
                ),
                DomainletCapability(
                    name="insert_stalls",
                    description="Insert pipeline stalls",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "stall"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "detect_data_hazards")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.schedule.hazard_recognition.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class PostRASchedulingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.schedule.post_ra_scheduling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.schedule.post_ra_scheduling",
            version="1.0.0",
            description="Post-register-allocation scheduling: adjust schedule for spill code and anti-dependencies",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="reschedule_after_ra",
                    description="Reschedule after register allocation",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "post-ra"],
                ),
                DomainletCapability(
                    name="adjust_for_spills",
                    description="Adjust schedule for spill code",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "spill"],
                ),
                DomainletCapability(
                    name="break_anti_deps",
                    description="Break anti-dependencies in the schedule",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "schedule", "anti-dep"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "reschedule_after_ra")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.schedule.post_ra_scheduling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Regalloc Bindlets
# ═══════════════════════════════════════════════════════════════════

class LiveIntervalBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.live_interval"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.live_interval",
            version="1.0.0",
            description="Compute and manipulate live intervals for register allocators",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_intervals",
                    description="Compute live intervals",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "liveness"],
                ),
                DomainletCapability(
                    name="union_intervals",
                    description="Union live intervals",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "union"],
                ),
                DomainletCapability(
                    name="interval_overlap",
                    description="Check interval overlap",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "overlap"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_intervals")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.live_interval.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class LiveRangeSplittingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.live_range_splitting"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.live_range_splitting",
            version="1.0.0",
            description="Split live ranges at call sites or loop boundaries to reduce spill pressure",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="split_at_calls",
                    description="Split live ranges at call sites",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "split"],
                ),
                DomainletCapability(
                    name="split_at_loop_bounds",
                    description="Split live ranges at loop boundaries",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "loop"],
                ),
                DomainletCapability(
                    name="recompute_liveness",
                    description="Recompute liveness after splitting",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "liveness"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "split_at_calls")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.live_range_splitting.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class SpillCostBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.spill_cost"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.spill_cost",
            version="1.0.0",
            description="Compute spill cost heuristics weighted by loop depth and use count",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_cost",
                    description="Compute spill costs",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "spill"],
                ),
                DomainletCapability(
                    name="rank_spill_candidates",
                    description="Rank spill candidates by cost",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "ranking"],
                ),
                DomainletCapability(
                    name="estimate_savings",
                    description="Estimate savings from spilling",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "savings"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_cost")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.spill_cost.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class GraphColoringBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.graph_coloring"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.graph_coloring",
            version="1.0.0",
            description="Graph-coloring register allocation: build interference graph, color it with K colors",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_interference",
                    description="Build the interference graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "interference"],
                ),
                DomainletCapability(
                    name="color_graph",
                    description="Color the interference graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "coloring"],
                ),
                DomainletCapability(
                    name="assign_registers",
                    description="Assign physical registers from coloring",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "assign"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "color_graph")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.graph_coloring.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class LinearScanBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.linear_scan"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.linear_scan",
            version="1.0.0",
            description="Linear-scan register allocation: allocate on first-fit basis",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_live_intervals",
                    description="Compute live intervals",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "linear"],
                ),
                DomainletCapability(
                    name="scan_allocate",
                    description="Scan and allocate registers",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "allocate"],
                ),
                DomainletCapability(
                    name="spill_active",
                    description="Spill active intervals",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "spill"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "scan_allocate")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.linear_scan.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class RegisterCoalescingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.register_coalescing"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.register_coalescing",
            version="1.0.0",
            description="Coalesce copy-connected live ranges when safe (Briggs/George conservative coalescing)",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="coalesce_copies",
                    description="Coalesce copy-connected live ranges",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "coalesce"],
                ),
                DomainletCapability(
                    name="build_affinity_graph",
                    description="Build the affinity graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "affinity"],
                ),
                DomainletCapability(
                    name="conservative_merge",
                    description="Conservatively merge live ranges",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "merge"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "coalesce_copies")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.register_coalescing.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class RematerializationBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.rematerialization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.rematerialization",
            version="1.0.0",
            description="Rematerialize cheap constants instead of spilling to memory",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_remat_candidates",
                    description="Find rematerialization candidates",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "remat"],
                ),
                DomainletCapability(
                    name="remat_instead_of_spill",
                    description="Rematerialize instead of spilling",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "remat"],
                ),
                DomainletCapability(
                    name="insert_remat",
                    description="Insert rematerialization instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "insert"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "find_remat_candidates")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.rematerialization.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class PBQPAllocBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.regalloc.pbqp_alloc"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.regalloc.pbqp",
            version="1.0.0",
            description="PBQP register allocation for optimal spill-aware assignment",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_pbqp_matrix",
                    description="Build the PBQP cost matrix",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "pbqp"],
                ),
                DomainletCapability(
                    name="solve_pbqp",
                    description="Solve the PBQP problem",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "solve"],
                ),
                DomainletCapability(
                    name="extract_assignment",
                    description="Extract register assignment from PBQP solution",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "regalloc", "assign"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "solve_pbqp")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.regalloc.pbqp.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Peephole Bindlets
# ═══════════════════════════════════════════════════════════════════

class PeepholeOptimizerBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.peephole.peephole_optimizer"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.peephole.optimizer",
            version="1.0.0",
            description="Sliding-window peephole optimizer: apply local rewrite rules over a fixed-width instruction window",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="apply_rules",
                    description="Apply peephole rewrite rules",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "rules"],
                ),
                DomainletCapability(
                    name="slide_window",
                    description="Slide the optimization window",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "window"],
                ),
                DomainletCapability(
                    name="match_replace",
                    description="Match and replace patterns in the window",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "replace"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "apply_rules")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.peephole.optimizer.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class CopyPropagationMIRBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.peephole.copy_propagation_mir"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.peephole.copy_propagation",
            version="1.0.0",
            description="Machine-level copy propagation: forward copies and eliminate redundant register moves",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="propagate_copies",
                    description="Propagate copy instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "copy"],
                ),
                DomainletCapability(
                    name="eliminate_dead_copy",
                    description="Eliminate dead copy instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "dead"],
                ),
                DomainletCapability(
                    name="fold_in_place",
                    description="Fold copies in place",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "fold"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "propagate_copies")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.peephole.copy_propagation.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class MachineCSEBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.peephole.machine_cse"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.peephole.machine_cse",
            version="1.0.0",
            description="Common-subexpression elimination at the machine-instruction level",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="avail_expr",
                    description="Find available expressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "cse"],
                ),
                DomainletCapability(
                    name="replace_cse",
                    description="Replace redundant expressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "replace"],
                ),
                DomainletCapability(
                    name="invalidate_kill",
                    description="Invalidate killed expressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "invalidate"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "replace_cse")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.peephole.machine_cse.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class BranchFoldingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.peephole.branch_folding"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.peephole.branch_folding",
            version="1.0.0",
            description="Branch folding and jump threading at the machine level",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="fold_branch",
                    description="Fold branch instructions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "branch"],
                ),
                DomainletCapability(
                    name="redirect_jump",
                    description="Redirect jump targets",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "jump"],
                ),
                DomainletCapability(
                    name="eliminate_unreachable",
                    description="Eliminate unreachable blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "unreachable"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "fold_branch")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.peephole.branch_folding.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class DelaySlotFillingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.peephole.delay_slot_filling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.peephole.delay_slot_filling",
            version="1.0.0",
            description="Fill branch delay slots with useful instructions",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_fill_candidates",
                    description="Find candidates for delay slot filling",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "delay"],
                ),
                DomainletCapability(
                    name="fill_delay_slot",
                    description="Fill the delay slot",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "fill"],
                ),
                DomainletCapability(
                    name="annul_if_needed",
                    description="Annul the delay slot instruction if needed",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "peephole", "annul"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "fill_delay_slot")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.peephole.delay_slot_filling.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Layout Bindlets
# ═══════════════════════════════════════════════════════════════════

class BlockPlacementBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.layout.block_placement"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.layout.block_placement",
            version="1.0.0",
            description="Basic-block placement: reorder blocks to maximize fall-through and minimize taken branches",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="chain_blocks",
                    description="Chain blocks together",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "chain"],
                ),
                DomainletCapability(
                    name="compute_fallthrough",
                    description="Compute fall-through probabilities",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "fallthrough"],
                ),
                DomainletCapability(
                    name="reorder_basic_blocks",
                    description="Reorder basic blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "reorder"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "reorder_basic_blocks")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.layout.block_placement.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class FunctionSplittingBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.layout.function_splitting"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.layout.function_splitting",
            version="1.0.0",
            description="Split functions into hot and cold sections based on profile data",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="hot_cold_split",
                    description="Split into hot and cold sections",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "split"],
                ),
                DomainletCapability(
                    name="mark_regions",
                    description="Mark hot and cold regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "region"],
                ),
                DomainletCapability(
                    name="emit_partitioned",
                    description="Emit partitioned output",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "emit"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "hot_cold_split")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.layout.function_splitting.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TailDuplicationBindlet(BaseBekernBindlet):
    _kernel_module = "domainlet.bekern.layout.tail_duplication"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="bekern.layout.tail_duplication",
            version="1.0.0",
            description="Tail duplication: clone small shared tails to improve block placement",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_tails",
                    description="Find tail blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "tail"],
                ),
                DomainletCapability(
                    name="duplicate_if_beneficial",
                    description="Duplicate tails if beneficial",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "duplicate"],
                ),
                DomainletCapability(
                    name="merge_after_dup",
                    description="Merge after duplication",
                    input_kind=InputKind.AST_JSON,
                    tags=["backend", "layout", "merge"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "duplicate_if_beneficial")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"bekern.layout.tail_duplication.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Registry
# ═══════════════════════════════════════════════════════════════════

BEKERN_BINDLETS: dict[str, type[BaseBekernBindlet]] = {
    "arch": ArchBindlet,
    "instruction_selection": InstructionSelectionBindlet,
    "tree_pattern_sel": TreePatternSelBindlet,
    "instruction_combine": InstructionCombineBindlet,
    "optimal_tiling": OptimalTilingBindlet,
    "legalization": LegalizationBindlet,
    "post_isel_expansion": PostISELExpansionBindlet,
    "calling_convention": CallingConventionBindlet,
    "frame_lowering": FrameLoweringBindlet,
    "emission": EmissionBindlet,
    "assembler_emission": AssemblerEmissionBindlet,
    "binary_encoding": BinaryEncodingBindlet,
    "list_scheduling": ListSchedulingBindlet,
    "modulo_scheduling": ModuloSchedulingBindlet,
    "bundle_scheduling": BundleSchedulingBindlet,
    "hazard_recognition": HazardRecognitionBindlet,
    "post_ra_scheduling": PostRASchedulingBindlet,
    "live_interval": LiveIntervalBindlet,
    "live_range_splitting": LiveRangeSplittingBindlet,
    "spill_cost": SpillCostBindlet,
    "graph_coloring": GraphColoringBindlet,
    "linear_scan": LinearScanBindlet,
    "register_coalescing": RegisterCoalescingBindlet,
    "rematerialization": RematerializationBindlet,
    "pbqp_alloc": PBQPAllocBindlet,
    "peephole_optimizer": PeepholeOptimizerBindlet,
    "copy_propagation": CopyPropagationMIRBindlet,
    "machine_cse": MachineCSEBindlet,
    "branch_folding": BranchFoldingBindlet,
    "delay_slot_filling": DelaySlotFillingBindlet,
    "block_placement": BlockPlacementBindlet,
    "function_splitting": FunctionSplittingBindlet,
    "tail_duplication": TailDuplicationBindlet,
}

class ExtraBekernBindlet(BaseBekernBindlet):
    _kernel_module = ""
    def probe(self) -> DomainletInfo:
        kernel = self._get_kernel()
        return DomainletInfo(name=kernel.name, version="1.0.0", description=kernel.description, author="domweave",
            capabilities=[DomainletCapability(name=op, description=op, input_kind=InputKind.AST_JSON, tags=["backend", kernel.category]) for op in kernel.operations])
    def configure(self, config: dict[str, str]) -> None: self._capability = config.get("capability", self._get_kernel().operations[0])
    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [AnalysisIssue(rule_id=f"{self._get_kernel().name}.{self._capability}", message=json.dumps(value), severity=Severity.INFORMATION, confidence=Confidence.HIGH)]

for _name, _module in {
    "isel_cost_model":"domainlet.bekern.isel.cost_model", "isel_machine_combiner":"domainlet.bekern.isel.machine_combiner", "isel_complex_patterns":"domainlet.bekern.isel.complex_patterns",
    "codegen_relocations":"domainlet.bekern.codegen.relocations", "codegen_object_writer":"domainlet.bekern.codegen.object_writer", "codegen_debug_line":"domainlet.bekern.codegen.debug_line",
    "schedule_resource_model":"domainlet.bekern.schedule.resource_model", "schedule_scoreboard":"domainlet.bekern.schedule.scoreboard", "schedule_cache_model":"domainlet.bekern.schedule.cache_model",
    "regalloc_spill_placement":"domainlet.bekern.regalloc.spill_placement", "regalloc_coalescing_cost":"domainlet.bekern.regalloc.coalescing_cost", "peephole_branch_relaxation":"domainlet.bekern.peephole.branch_relaxation",
    "layout_hot_cold":"domainlet.bekern.layout.hot_cold", "arch_riscv64_zba":"domainlet.bekern.arch.riscv64_zba", "arch_amd64_vzeroupper":"domainlet.bekern.arch.amd64_vzeroupper"}.items():
    BEKERN_BINDLETS[_name] = type(f"{_name.title().replace('_','')}Bindlet", (ExtraBekernBindlet,), {"_kernel_module": _module})


def get_bekern_bindlet(name: str) -> BaseBekernBindlet:
    """Factory for creating bekern bindlets by name."""
    cls = BEKERN_BINDLETS.get(name)
    if cls is None:
        raise ValueError(f"Unknown bekern bindlet: {name!r}. Available: {sorted(BEKERN_BINDLETS)}")
    return cls()
