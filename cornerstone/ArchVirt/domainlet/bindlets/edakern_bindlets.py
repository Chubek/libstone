"""
edakern_bindlets — Bindlet wrappers for edakern.* domainlets.

Exposes RTL, elaboration, synthesis, netlist, optimization, timing,
simulation, formal verification, PnR, and power analysis domainlets
through the unified AbstractDomainlet interface.
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
    FixSuggestion,
    InputKind,
    Severity,
    SourceSpan,
)


# ═══════════════════════════════════════════════════════════════════
#  Base Edakern Bindlet
# ═══════════════════════════════════════════════════════════════════

class BaseEdakernBindlet(AbstractDomainlet):
    """Shared base for all edakern bindlets."""

    _kernel_module: str = ""
    _category: str = "edakern"

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


# ═══════════════════════════════════════════════════════════════════
#  RTL Bindlets
# ═══════════════════════════════════════════════════════════════════

class RTLTypesBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.rtl.rtl_types"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.rtl.rtl_types",
            version="1.0.0",
            description="RTL type definitions: bit-vectors, arrays, records, enums",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="define_bitvector",
                    description="Define a bit-vector type with width, signedness, and range",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "rtl", "types"],
                ),
                DomainletCapability(
                    name="type_check",
                    description="Check compatibility between two RTL types",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "rtl", "types"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "type_check")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"edakern.rtl_types.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class SignalModelBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.rtl.signal_model"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.rtl.signal_model",
            version="1.0.0",
            description="Signal and port semantics for HDL designs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="define_signal",
                    description="Define a signal with type and clock domain",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "rtl", "signal"],
                ),
                DomainletCapability(
                    name="check_connection",
                    description="Check port/signal connection compatibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "rtl", "connection"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_connection")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                payload = {}
        else:
            payload = {}
        value = self._run_operation(self._capability, payload)
        return [
            AnalysisIssue(
                rule_id=f"edakern.signal_model.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Synthesis Bindlets
# ═══════════════════════════════════════════════════════════════════

class SynthAIGBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.synth.rtl_to_aig"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.synth.rtl_to_aig",
            version="1.0.0",
            description="RTL-to-AIG conversion for logical synthesis",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="convert_to_aig",
                    description="Convert RTL graph to And-Inverter Graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "synthesis", "aig"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = "convert_to_aig"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {}
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        return [
            AnalysisIssue(
                rule_id="edakern.synth.aig",
                message=f"AIG conversion: {value.get('and_count', 0)} AND gates, {value.get('inverter_count', 0)} inverters",
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Timing Bindlets
# ═══════════════════════════════════════════════════════════════════

class STABindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.timing.sta_engine"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.timing.sta_engine",
            version="1.0.0",
            description="Static Timing Analysis engine",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="sta_propagate",
                    description="Run topological STA propagation on a timing graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "timing", "sta"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = "sta_propagate"
        self._clock_period = float(config.get("clock_period", 10.0))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {"clock_period": self._clock_period}
        if context.data:
            try:
                payload.update(json.loads(context.data))
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        severity = Severity.ERROR if not value.get("timing_met", True) else Severity.INFORMATION
        return [
            AnalysisIssue(
                rule_id="edakern.timing.sta",
                message=f"STA: worst slack={value.get('worst_slack', 0):.3f}ns, timing_met={value.get('timing_met')}",
                severity=severity,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Formal Verification Bindlets
# ═══════════════════════════════════════════════════════════════════

class EquivCheckBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.formal.equivalence_checking"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.formal.equivalence_checking",
            version="1.0.0",
            description="Combinational and sequential equivalence checking",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_combinational_eq",
                    description="Check combinational equivalence between two netlists",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "formal", "equivalence"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = "check_combinational_eq"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {}
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        severity = Severity.ERROR if not value.get("equivalent") else Severity.INFORMATION
        return [
            AnalysisIssue(
                rule_id="edakern.formal.equiv",
                message=f"Equivalence: {'PASS' if value.get('equivalent') else 'FAIL'} "
                        f"({value.get('matched_outputs', 0)}/{value.get('total_outputs', 0)} matched)",
                severity=severity,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Simulation Bindlets
# ═══════════════════════════════════════════════════════════════════

class VCDBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.sim.waveform_writer"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.sim.waveform_writer",
            version="1.0.0",
            description="VCD waveform dump writer",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="write_vcd",
                    description="Generate VCD format waveform data",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "simulation", "vcd"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = "write_vcd"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {}
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        return [
            AnalysisIssue(
                rule_id="edakern.sim.vcd",
                message=f"VCD generated: {value.get('variable_count', 0)} variables, "
                        f"{value.get('line_count', 0)} lines",
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Power Analysis Bindlets
# ═══════════════════════════════════════════════════════════════════

class PowerModelBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.power.power_model"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.power.power_model",
            version="1.0.0",
            description="Power analysis: dynamic and leakage power estimation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_dynamic_power",
                    description="Compute dynamic power (CV²f * activity)",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "power", "dynamic"],
                ),
                DomainletCapability(
                    name="estimate_total_power",
                    description="Estimate total power including dynamic and leakage",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "power", "total"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "estimate_total_power")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {}
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        return [
            AnalysisIssue(
                rule_id=f"edakern.power.{self._capability}",
                message=f"Power: {value.get('total_power_mw', value.get('dynamic_power_mw', 0)):.3f} mW",
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  PnR Bindlets
# ═══════════════════════════════════════════════════════════════════

class FloorplanBindlet(BaseEdakernBindlet):
    _kernel_module = "domainlet.edakern.pnr.floorplan"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="edakern.pnr.floorplan",
            version="1.0.0",
            description="Floorplan: die area definition and macro placement",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="define_die_area",
                    description="Define die area dimensions",
                    input_kind=InputKind.AST_JSON,
                    tags=["eda", "pnr", "floorplan"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = "define_die_area"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        import json
        payload = {}
        if context.data:
            try:
                payload = json.loads(context.data)
            except (json.JSONDecodeError, TypeError):
                pass
        value = self._run_operation(self._capability, payload)
        return [
            AnalysisIssue(
                rule_id="edakern.pnr.floorplan",
                message=f"Die: {value.get('die', {}).get('width_um', 0)}x"
                        f"{value.get('die', {}).get('height_um', 0)} um",
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Registry
# ═══════════════════════════════════════════════════════════════════

EDAKERN_BINDLETS: dict[str, type[BaseEdakernBindlet]] = {
    "rtl_types": RTLTypesBindlet,
    "signal_model": SignalModelBindlet,
    "synth_aig": SynthAIGBindlet,
    "sta": STABindlet,
    "equiv_check": EquivCheckBindlet,
    "vcd_writer": VCDBindlet,
    "power_model": PowerModelBindlet,
    "floorplan": FloorplanBindlet,
}


def get_edakern_bindlet(name: str) -> BaseEdakernBindlet:
    """Factory for creating edakern bindlets by name."""
    cls = EDAKERN_BINDLETS.get(name)
    if cls is None:
        raise ValueError(f"Unknown edakern bindlet: {name!r}. Available: {sorted(EDAKERN_BINDLETS)}")
    return cls()
