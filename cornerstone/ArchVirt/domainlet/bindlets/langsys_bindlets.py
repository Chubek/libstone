"""
langsys_bindlets — Bindlet wrappers for langsys.* domainlets.

Exposes analyzer, concurrency, extcc, ffi, generator, interop,
interpreter, intrinsics, IR, memory, optimization, polyhedral,
runtime, support, transformer, and validator language-system
kernels through the unified AbstractDomainlet interface.
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
#  Base Langsys Bindlet
# ═══════════════════════════════════════════════════════════════════


class BaseLangsysBindlet(AbstractDomainlet):
    """Shared base for all langsys bindlets."""

    _kernel_module: str = ""
    _category: str = "langsys"

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
#  Analyzer Bindlets
# ═══════════════════════════════════════════════════════════════════

class EscapeAnalysisBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.analyzer.escape_analysis"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.analyzer.escape_analysis",
            version="1.0.0",
            description="Escape analysis: determine which allocations never escape their creating function, enabling stack allocation or scalar replacement.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="track_capture",
                    description="track capture",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="decide_stack_alloc",
                    description="decide stack alloc",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="decide_heap_alloc",
                    description="decide heap alloc",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="propagate_noescape",
                    description="propagate noescape",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "track_capture")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.analyzer.escape_analysis." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class PointsToBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.analyzer.points_to"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.analyzer.points_to",
            version="1.0.0",
            description="Points-to analysis: Andersen inclusion-based and Steensgaard unification-based pointer analysis, constraint-graph construction.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="andersen_analysis",
                    description="andersen analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="steensgaard_analysis",
                    description="steensgaard analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="query_points_to",
                    description="query points to",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="build_constraint_graph",
                    description="build constraint graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "andersen_analysis")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.analyzer.points_to." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class SideEffectBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.analyzer.side_effect"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.analyzer.side_effect",
            version="1.0.0",
            description="Side-effect analysis: classify reads/writes/allocations per function, summarize inter-procedurally, and propagate pure/const attributes.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="classify_effects",
                    description="classify effects",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="summarize_function",
                    description="summarize function",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="check_purity",
                    description="check purity",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
                DomainletCapability(
                    name="propagate_effects",
                    description="propagate effects",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "analyzer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "classify_effects")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.analyzer.side_effect." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Concurrency Bindlets
# ═══════════════════════════════════════════════════════════════════

class AtomicNormalizationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.atomic_normalization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.atomic_normalization",
            version="1.0.0",
            description="atomic normalization",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="normalize_atomic_ops",
                    description="normalize atomic ops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
                DomainletCapability(
                    name="canonicalize_memory_order",
                    description="canonicalize memory order",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
                DomainletCapability(
                    name="expand_rmw",
                    description="expand rmw",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "normalize_atomic_ops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.atomic_normalization." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class BarrierPlacementBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.barrier_placement"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.barrier_placement",
            version="1.0.0",
            description="barrier placement",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="place_barriers",
                    description="place barriers",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
                DomainletCapability(
                    name="insert_fences",
                    description="insert fences",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
                DomainletCapability(
                    name="hoist_safepoints",
                    description="hoist safepoints",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "place_barriers")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.barrier_placement." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CriticalSectionsBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.critical_sections"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.critical_sections",
            version="1.0.0",
            description="critical sections",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="identify_critical_sections",
                    description="identify critical sections",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="split_lock_regions",
                    description="split lock regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="estimate_contention",
                    description="estimate contention",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "identify_critical_sections")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.critical_sections." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class HappensBeforeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.happens_before"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.happens_before",
            version="1.0.0",
            description="happens before",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_happens_before",
                    description="build happens before",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="infer_sync_edges",
                    description="infer sync edges",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="summarize_ordering",
                    description="summarize ordering",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_happens_before")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.happens_before." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class InterleavingModelBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.interleaving_model"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.interleaving_model",
            version="1.0.0",
            description="interleaving model",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="enumerate_interleavings",
                    description="enumerate interleavings",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="prune_equivalent_orders",
                    description="prune equivalent orders",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="compress_schedule_space",
                    description="compress schedule space",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "enumerate_interleavings")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.interleaving_model." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ParallelRegionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.parallel_region"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.parallel_region",
            version="1.0.0",
            description="parallel region",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="extract_parallel_regions",
                    description="extract parallel regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
                DomainletCapability(
                    name="outline_parallel_blocks",
                    description="outline parallel blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
                DomainletCapability(
                    name="annotate_spawn_points",
                    description="annotate spawn points",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "extract_parallel_regions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.parallel_region." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class RAceDetectionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.race_detection"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.race_detection",
            version="1.0.0",
            description="race detection",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="detect_data_races",
                    description="detect data races",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="detect_ordering_hazards",
                    description="detect ordering hazards",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
                DomainletCapability(
                    name="emit_race_report",
                    description="emit race report",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "detect_data_races")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.race_detection." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class TaskGraphBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.concurrency.task_graph"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.concurrency.task_graph",
            version="1.0.0",
            description="task graph",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_task_graph",
                    description="build task graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
                DomainletCapability(
                    name="schedule_ready_sets",
                    description="schedule ready sets",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
                DomainletCapability(
                    name="balance_work_queues",
                    description="balance work queues",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "concurrency", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_task_graph")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.concurrency.task_graph." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Extcc Bindlets
# ═══════════════════════════════════════════════════════════════════

class XcgBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xcg"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xcg",
            version="1.0.0",
            description="xcg",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_emission_rule",
                    description="register emission rule",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="dispatch_extension_emit",
                    description="dispatch extension emit",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="lower_to_target",
                    description="lower to target",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="optimize_extension_code",
                    description="optimize extension code",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="compose_emission_order",
                    description="compose emission order",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="emit_header_preamble",
                    description="emit header preamble",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="emit_extension_runtime",
                    description="emit extension runtime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="validate_target_constraints",
                    description="validate target constraints",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_emission_rule")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xcg." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XextBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xext"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xext",
            version="1.0.0",
            description="xext",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="discover_extensions",
                    description="discover extensions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="load_extension",
                    description="load extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="validate_extension",
                    description="validate extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="resolve_dependencies",
                    description="resolve dependencies",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="activate_extension",
                    description="activate extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="deactivate_extension",
                    description="deactivate extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="reload_extension",
                    description="reload extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="query_extension_capability",
                    description="query extension capability",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "discover_extensions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xext." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XpgBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xpg"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xpg",
            version="1.0.0",
            description="xpg",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="merge_grammars",
                    description="merge grammars",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="resolve_extension_point",
                    description="resolve extension point",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="inject_production",
                    description="inject production",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="compute_first_follow",
                    description="compute first follow",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="build_parse_table",
                    description="build parse table",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="detect_ambiguities",
                    description="detect ambiguities",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="rewrite_extension_parses",
                    description="rewrite extension parses",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="generate_combinator",
                    description="generate combinator",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "merge_grammars")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xpg." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XphaseBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xphase"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xphase",
            version="1.0.0",
            description="xphase",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_phase",
                    description="register phase",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="compute_phase_graph",
                    description="compute phase graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="schedule_phases",
                    description="schedule phases",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="detect_phase_cycles",
                    description="detect phase cycles",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="parallelize_independent",
                    description="parallelize independent",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="apply_extension_phases",
                    description="apply extension phases",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="record_phase_timing",
                    description="record phase timing",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
                DomainletCapability(
                    name="report_phase_status",
                    description="report phase status",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "backend"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_phase")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xphase." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XsemBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xsem"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xsem",
            version="1.0.0",
            description="xsem",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_semantic_rule",
                    description="register semantic rule",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="dispatch_extension_check",
                    description="dispatch extension check",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="propagate_attributes",
                    description="propagate attributes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="build_symbol_table",
                    description="build symbol table",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="check_def_before_use",
                    description="check def before use",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="check_reachability",
                    description="check reachability",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="analyze_side_effects",
                    description="analyze side effects",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="emit_semantic_diagnostics",
                    description="emit semantic diagnostics",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_semantic_rule")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xsem." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XstBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xst"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xst",
            version="1.0.0",
            description="xst",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="define_extension_node",
                    description="define extension node",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="register_production",
                    description="register production",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="compose_tree",
                    description="compose tree",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="query_extension_nodes",
                    description="query extension nodes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="validate_extension_schema",
                    description="validate extension schema",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="serialize_tree",
                    description="serialize tree",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="deserialize_tree",
                    description="deserialize tree",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
                DomainletCapability(
                    name="rewrite_extension_subtree",
                    description="rewrite extension subtree",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "frontend"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "define_extension_node")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xst." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XtrBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xtr"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xtr",
            version="1.0.0",
            description="xtr",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_type_rule",
                    description="register type rule",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="resolve_type",
                    description="resolve type",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="check_extension_types",
                    description="check extension types",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="unify_cross_extension",
                    description="unify cross extension",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="compute_subtype_rel",
                    description="compute subtype rel",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="infer_extension_vars",
                    description="infer extension vars",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="emit_type_diagnostics",
                    description="emit type diagnostics",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_type_rule")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xtr." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class XweaveBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.extcc.xweave"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.extcc.xweave",
            version="1.0.0",
            description="xweave",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_pass",
                    description="register pass",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="resolve_pass_order",
                    description="resolve pass order",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="weave_extension_passes",
                    description="weave extension passes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="detect_pass_conflicts",
                    description="detect pass conflicts",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="inline_pass_sequences",
                    description="inline pass sequences",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="validate_pass_graph",
                    description="validate pass graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="schedule_weave",
                    description="schedule weave",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
                DomainletCapability(
                    name="rewind_on_extension_change",
                    description="rewind on extension change",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "extcc", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_pass")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.extcc.xweave." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Ffi Bindlets
# ═══════════════════════════════════════════════════════════════════

class ABILoweringBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.abi_lowering"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.abi_lowering",
            version="1.0.0",
            description="abi lowering",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="classify_arguments",
                    description="classify arguments",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="assign_registers",
                    description="assign registers",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="layout_stack",
                    description="layout stack",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="lower_return",
                    description="lower return",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "classify_arguments")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.abi_lowering." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CallMarshallingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.call_marshalling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.call_marshalling",
            version="1.0.0",
            description="call marshalling",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="pack_arguments",
                    description="pack arguments",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="unpack_results",
                    description="unpack results",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="convert_scalars",
                    description="convert scalars",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="convert_aggregate",
                    description="convert aggregate",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "pack_arguments")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.call_marshalling." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CallbackBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.callback_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.callback_bridge",
            version="1.0.0",
            description="callback bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_trampoline",
                    description="emit trampoline",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="enter_runtime",
                    description="enter runtime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="marshal_callback",
                    description="marshal callback",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="leave_runtime",
                    description="leave runtime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_trampoline")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.callback_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ClosureConversionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.closure_conversion"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.closure_conversion",
            version="1.0.0",
            description="closure conversion",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="capture_environment",
                    description="capture environment",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="emit_entry",
                    description="emit entry",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="adapt_receiver",
                    description="adapt receiver",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="release_environment",
                    description="release environment",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "capture_environment")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.closure_conversion." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class FFITypeMappingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.ffi_type_mapping"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.ffi_type_mapping",
            version="1.0.0",
            description="ffi type mapping",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="map_primitive",
                    description="map primitive",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "semantic"],
                ),
                DomainletCapability(
                    name="map_pointer",
                    description="map pointer",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "semantic"],
                ),
                DomainletCapability(
                    name="map_struct",
                    description="map struct",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "semantic"],
                ),
                DomainletCapability(
                    name="validate_compatibility",
                    description="validate compatibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "semantic"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "map_primitive")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.ffi_type_mapping." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ForeignExceptionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.foreign_exception"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.foreign_exception",
            version="1.0.0",
            description="foreign exception",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="encode_exception",
                    description="encode exception",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="decode_error",
                    description="decode error",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="install_personality",
                    description="install personality",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
                DomainletCapability(
                    name="build_landing_pad",
                    description="build landing pad",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "encode_exception")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.foreign_exception." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class NativeSymbolBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.native_symbol"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.native_symbol",
            version="1.0.0",
            description="native symbol",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="lookup_symbol",
                    description="lookup symbol",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "linking"],
                ),
                DomainletCapability(
                    name="check_version",
                    description="check version",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "linking"],
                ),
                DomainletCapability(
                    name="apply_visibility",
                    description="apply visibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "linking"],
                ),
                DomainletCapability(
                    name="bind_address",
                    description="bind address",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "linking"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "lookup_symbol")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.native_symbol." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class VariadicAdapterBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ffi.variadic_adapter"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ffi.variadic_adapter",
            version="1.0.0",
            description="variadic adapter",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="promote_arguments",
                    description="promote arguments",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="build_va_list",
                    description="build va list",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="read_va_arg",
                    description="read va arg",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
                DomainletCapability(
                    name="validate_format",
                    description="validate format",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ffi", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "promote_arguments")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ffi.variadic_adapter." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Generator Bindlets
# ═══════════════════════════════════════════════════════════════════

class ConstantPoolBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.generator.constant_pool"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.generator.constant_pool",
            version="1.0.0",
            description="Constant-pool construction: intern strings and numeric constants, deduplicate, assign section labels, and emit read-only data.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="intern_constants",
                    description="intern constants",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="assign_labels",
                    description="assign labels",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_data_section",
                    description="emit data section",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="deduplicate",
                    description="deduplicate",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "intern_constants")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.generator.constant_pool." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DebugInfoBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.generator.debug_info_emission"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.generator.debug_info",
            version="1.0.0",
            description="DWARF debug information emission: emit line-number program, scope DIEs, type descriptions, and macro sections.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_dwarf_line",
                    description="emit dwarf line",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_scope_info",
                    description="emit scope info",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_type_die",
                    description="emit type die",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_macro_info",
                    description="emit macro info",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_dwarf_line")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.generator.debug_info." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ExceptionTableBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.generator.exception_table"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.generator.exception_table",
            version="1.0.0",
            description="Exception-handling table emission: build try/catch ranges, landing pads, and language-specific data area (LSDA) for unwinding.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_try_range",
                    description="build try range",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="build_catch_clause",
                    description="build catch clause",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_lsda",
                    description="emit lsda",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="sort_ranges",
                    description="sort ranges",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_try_range")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.generator.exception_table." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class GCMetadataBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.generator.gc_metadata"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.generator.gc_metadata",
            version="1.0.0",
            description="GC metadata emission: stack maps, root-set descriptors, reference-type bitmaps, and safepoint poll sites for precise garbage collection.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_stack_map",
                    description="emit stack map",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_gc_roots",
                    description="emit gc roots",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_reftype_map",
                    description="emit reftype map",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_safepoint_poll",
                    description="emit safepoint poll",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_stack_map")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.generator.gc_metadata." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class TrampolineBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.generator.trampoline_gen"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.generator.trampoline",
            version="1.0.0",
            description="Trampoline and thunk generation: closure entry points, virtual-method adjustment thunks, interface adapters, and tail-call stubs.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_closure_trampoline",
                    description="emit closure trampoline",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_virtual_thunk",
                    description="emit virtual thunk",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_interface_adapter",
                    description="emit interface adapter",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
                DomainletCapability(
                    name="emit_tail_call",
                    description="emit tail call",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "generator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_closure_trampoline")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.generator.trampoline." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Interop Bindlets
# ═══════════════════════════════════════════════════════════════════

class AsyncBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.async_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.async_bridge",
            version="1.0.0",
            description="async bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="adapt_future",
                    description="adapt future",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="schedule_task",
                    description="schedule task",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="propagate_cancel",
                    description="propagate cancel",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="propagate_error",
                    description="propagate error",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "adapt_future")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.async_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class IpcBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.ipc_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.ipc_bridge",
            version="1.0.0",
            description="ipc bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="encode_request",
                    description="encode request",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="decode_request",
                    description="decode request",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="encode_response",
                    description="encode response",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="route_call",
                    description="route call",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "encode_request")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.ipc_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ModuleBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.module_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.module_bridge",
            version="1.0.0",
            description="module bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="resolve_imports",
                    description="resolve imports",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="publish_exports",
                    description="publish exports",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="adapt_module",
                    description="adapt module",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="check_cycles",
                    description="check cycles",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "resolve_imports")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.module_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ObjectProtocolBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.object_protocol"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.object_protocol",
            version="1.0.0",
            description="object protocol",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="adapt_layout",
                    description="adapt layout",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "middle"],
                ),
                DomainletCapability(
                    name="resolve_method",
                    description="resolve method",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "middle"],
                ),
                DomainletCapability(
                    name="box_object",
                    description="box object",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "middle"],
                ),
                DomainletCapability(
                    name="unbox_object",
                    description="unbox object",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "adapt_layout")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.object_protocol." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class OwnershipBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.ownership_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.ownership_bridge",
            version="1.0.0",
            description="ownership bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="retain",
                    description="retain",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="release",
                    description="release",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="transfer",
                    description="transfer",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="validate_lifetime",
                    description="validate lifetime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "retain")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.ownership_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class PluginBoundaryBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.plugin_boundary"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.plugin_boundary",
            version="1.0.0",
            description="plugin boundary",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="load_plugin",
                    description="load plugin",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="negotiate_capabilities",
                    description="negotiate capabilities",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="bind_entrypoints",
                    description="bind entrypoints",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="unload_plugin",
                    description="unload plugin",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "load_plugin")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.plugin_boundary." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class SerializationBridgeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.serialization_bridge"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.serialization_bridge",
            version="1.0.0",
            description="serialization bridge",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="encode_value",
                    description="encode value",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="decode_value",
                    description="decode value",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="register_schema",
                    description="register schema",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
                DomainletCapability(
                    name="check_version",
                    description="check version",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "encode_value")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.serialization_bridge." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class VersionHandshakeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interop.version_handshake"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interop.version_handshake",
            version="1.0.0",
            description="version handshake",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="advertise_features",
                    description="advertise features",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="negotiate_version",
                    description="negotiate version",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="check_compatibility",
                    description="check compatibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
                DomainletCapability(
                    name="report_mismatch",
                    description="report mismatch",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interop", "linking"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "advertise_features")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interop.version_handshake." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Interp Bindlets
# ═══════════════════════════════════════════════════════════════════

class BuiltinDispatchBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.builtin_dispatch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.builtin_dispatch",
            version="1.0.0",
            description="Builtin-function lookup for the interpreter: builtin table population, arity checking, and fast-path dispatch to host implementations of core routines.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_builtin",
                    description="register builtin",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="check_arity",
                    description="check arity",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="dispatch_builtin",
                    description="dispatch builtin",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="fallback_slowpath",
                    description="fallback slowpath",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_builtin")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.builtin_dispatch." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class BytecodeDecoderBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.bytecode_decoder"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.bytecode_decoder",
            version="1.0.0",
            description="Decode flat bytecode back into instructions for execution or disassembly, with operand unpacking and bounds-checked reads.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="decode_instruction",
                    description="decode instruction",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="decode_stream",
                    description="decode stream",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="disassemble",
                    description="disassemble",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "decode_instruction")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.bytecode_decoder." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class BytecodeEncoderBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.bytecode_encoder"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.bytecode_encoder",
            version="1.0.0",
            description="Assemble an instruction stream into flat bytecode: operand packing, jump-label resolution, and unit assembly.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="encode_instruction",
                    description="encode instruction",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="resolve_labels",
                    description="resolve labels",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="pack_operands",
                    description="pack operands",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="assemble_unit",
                    description="assemble unit",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "encode_instruction")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.bytecode_encoder." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class BytecodeVerifierBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.bytecode_verifier"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.bytecode_verifier",
            version="1.0.0",
            description="Verify bytecode before first execution: reachable max stack depth, in-range jump targets, consistent type states, and operand bounds.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_stack_depth",
                    description="check stack depth",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "analysis"],
                ),
                DomainletCapability(
                    name="check_jump_targets",
                    description="check jump targets",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "analysis"],
                ),
                DomainletCapability(
                    name="check_type_state",
                    description="check type state",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "analysis"],
                ),
                DomainletCapability(
                    name="check_operand_bounds",
                    description="check operand bounds",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_stack_depth")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.bytecode_verifier." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CallCounterBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.call_counter"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.call_counter",
            version="1.0.0",
            description="Hotness instrumentation for tiered execution: per-function and per-loop-backedge counters, threshold state machine, and compilation-request triggers.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="increment_counter",
                    description="increment counter",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="check_threshold",
                    description="check threshold",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="mark_hot",
                    description="mark hot",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="request_tier_up",
                    description="request tier up",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "increment_counter")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.call_counter." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ClosureBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.closure"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.closure",
            version="1.0.0",
            description="Closures and variable capture for the runtime: upvalue links, captured-cell discipline, and environment over captured bindings.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="capture_upvalue",
                    description="capture upvalue",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="link_cell",
                    description="link cell",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="read_captured",
                    description="read captured",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="write_captured",
                    description="write captured",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "capture_upvalue")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.closure." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ConstantPoolBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.constant_pool"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.constant_pool",
            version="1.0.0",
            description="Runtime constant pool: interning of literals, deduplication, and stable constant-id lookup for load instructions.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="intern_constant",
                    description="intern constant",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="dedupe_constants",
                    description="dedupe constants",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="lookup_constant",
                    description="lookup constant",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="compact_pool",
                    description="compact pool",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "intern_constant")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.constant_pool." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ControlFlowBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.control_flow"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.control_flow",
            version="1.0.0",
            description="Control-transfer opcodes for the interpreter: branches, forward and backward jump patching, call-site chaining, and return sequences.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="eval_branch",
                    description="eval branch",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="patch_jump",
                    description="patch jump",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="chain_calls",
                    description="chain calls",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="sequence_returns",
                    description="sequence returns",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "eval_branch")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.control_flow." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CPSEvalBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.cps_eval"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.cps_eval",
            version="1.0.0",
            description="CPS evaluation: transform evaluation into explicit continuation closures, enabling deep-recursion-safe execution without growing the host stack.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_continuation",
                    description="build continuation",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="apply_continuation",
                    description="apply continuation",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="trampoline_step",
                    description="trampoline step",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="run_cps",
                    description="run cps",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_continuation")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.cps_eval." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DebuggerBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.debugger"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.debugger",
            version="1.0.0",
            description="Debugging support for the interpreter: breakpoints on bytecode offsets, single-stepping, and frame inspection.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="set_breakpoint",
                    description="set breakpoint",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="single_step",
                    description="single step",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="inspect_frame",
                    description="inspect frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="map_source_offset",
                    description="map source offset",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "set_breakpoint")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.debugger." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DeoptBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.deopt"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.deopt",
            version="1.0.0",
            description="Deoptimization: bail out of compiled code back to the interpreter, reconstructing interpreter frames from compiled-frame state maps.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="bailout_to_interp",
                    description="bailout to interp",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="reconstruct_frame",
                    description="reconstruct frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="flush_caches",
                    description="flush caches",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="resume_interpreted",
                    description="resume interpreted",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "bailout_to_interp")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.deopt." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DispatchBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.dispatch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.dispatch",
            version="1.0.0",
            description="Opcode dispatch strategies for the interpreter main loop: computed goto-style table dispatch, switch dispatch, and direct-threading.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="select_strategy",
                    description="select strategy",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="build_dispatch_table",
                    description="build dispatch table",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="thread_code",
                    description="thread code",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="step_frame",
                    description="step frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "select_strategy")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.dispatch." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class EnvironmentChainBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.environment_chain"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.environment_chain",
            version="1.0.0",
            description="Lexical environments for the evaluator: scope chains, block scoping, and declaration shadowing discipline.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="push_scope",
                    description="push scope",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="pop_scope",
                    description="pop scope",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="declare_binding",
                    description="declare binding",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="resolve_lexical",
                    description="resolve lexical",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "push_scope")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.environment_chain." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ExceptionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.exception"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.exception",
            version="1.0.0",
            description="Interpreter exception semantics: handler stack maintenance, catch dispatch on a thrown error, and resume-point restoration.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="push_handler",
                    description="push handler",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="throw_error",
                    description="throw error",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="dispatch_catch",
                    description="dispatch catch",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="restore_resume_point",
                    description="restore resume point",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "push_handler")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.exception." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class FrameBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.frame"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.frame",
            version="1.0.0",
            description="Call frames for the runtime: activation records, frame chains, and frame pointer restoration during normal returns and unwinding.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="push_frame",
                    description="push frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="pop_frame",
                    description="pop frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="walk_frames",
                    description="walk frames",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="restore_frame_pointer",
                    description="restore frame pointer",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "push_frame")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.frame." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class InlineCacheBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.inline_cache"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.inline_cache",
            version="1.0.0",
            description="Runtime inline caches: per-call-site shape/type guard, cache fill, and miss handling for monomorphic sites.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="guard_shape",
                    description="guard shape",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="fill_cache",
                    description="fill cache",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="probe_cache",
                    description="probe cache",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="handle_miss",
                    description="handle miss",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "guard_shape")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.inline_cache." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LineTableBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.line_table"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.line_table",
            version="1.0.0",
            description="Source-mapping tables for the interpreter: bytecode-offset to source-position ranges, statement boundaries, and reverse lookup.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="record_position",
                    description="record position",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="build_ranges",
                    description="build ranges",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="lookup_offset",
                    description="lookup offset",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="reverse_lookup",
                    description="reverse lookup",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "record_position")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.line_table." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class MethodDispatchBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.method_dispatch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.method_dispatch",
            version="1.0.0",
            description="Method resolution at runtime: class-hierarchy lookup, MRO walking, and cached method-slot binding for instance calls.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="lookup_method",
                    description="lookup method",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="walk_mro",
                    description="walk mro",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="bind_method_slot",
                    description="bind method slot",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="call_bound",
                    description="call bound",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "lookup_method")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.method_dispatch." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class NativeBindingsBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.native_bindings"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.native_bindings",
            version="1.0.0",
            description="Native-function bindings and the host call bridge: signature marshalling, host-call frames, and error propagation from native callees back into the interpreter.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="register_native",
                    description="register native",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="marshal_arguments",
                    description="marshal arguments",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="invoke_native",
                    description="invoke native",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="propagate_native_error",
                    description="propagate native error",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "register_native")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.native_bindings." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ObjectModelBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.object_model"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.object_model",
            version="1.0.0",
            description="Runtime object model for the interpreter: tagged-value boxing and unboxing, field access, and instance construction.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="box_value",
                    description="box value",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="unbox_value",
                    description="unbox value",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="read_field",
                    description="read field",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="construct_instance",
                    description="construct instance",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "box_value")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.object_model." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class OpcodeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.opcode"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.opcode",
            version="1.0.0",
            description="Define an interpreter instruction set: stable opcode numbering, operand layouts, and the metadata tables encoders and dispatchers consume.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="define_opcode",
                    description="define opcode",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="assign_codes",
                    description="assign codes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="layout_operands",
                    description="layout operands",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
                DomainletCapability(
                    name="emit_metadata",
                    description="emit metadata",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "define_opcode")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.opcode." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class OSRBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.osr"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.osr",
            version="1.0.0",
            description="On-stack replacement: migrate a running interpreter frame to freshly compiled code, with state transfer and resume handoff.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_osr_point",
                    description="check osr point",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="transfer_frame_state",
                    description="transfer frame state",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="swap_entry",
                    description="swap entry",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="resume_compiled",
                    description="resume compiled",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_osr_point")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.osr." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class PeepholeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.peephole"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.peephole",
            version="1.0.0",
            description="Peephole rewrites on the bytecode stream: adjacent-opcode folding, redundant-store elimination, and jump-to-jump threading.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="fold_adjacent",
                    description="fold adjacent",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "transform"],
                ),
                DomainletCapability(
                    name="eliminate_redundant_store",
                    description="eliminate redundant store",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "transform"],
                ),
                DomainletCapability(
                    name="thread_jumps",
                    description="thread jumps",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "transform"],
                ),
                DomainletCapability(
                    name="rewrite_stream",
                    description="rewrite stream",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "fold_adjacent")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.peephole." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class PolymorphicInlineCacheBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.polymorphic_inline_cache"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.polymorphic_inline_cache",
            version="1.0.0",
            description="Polymorphic inline caches: multi-shape entry recording, megamorphic transition, and guarded multi-hit dispatch.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="record_shape",
                    description="record shape",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="transition_megamorphic",
                    description="transition megamorphic",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="guard_multi_hit",
                    description="guard multi hit",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="evict_entry",
                    description="evict entry",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "record_shape")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.polymorphic_inline_cache." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class RegisterVmBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.register_vm"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.register_vm",
            version="1.0.0",
            description="Register-machine execution semantics: three-operand opcode evaluation, register-file access rules, and fixed-slot calling.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="read_register",
                    description="read register",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="write_register",
                    description="write register",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="eval_three_address",
                    description="eval three address",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="check_register_bounds",
                    description="check register bounds",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "read_register")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.register_vm." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ResourceLimitsBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.resource_limits"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.resource_limits",
            version="1.0.0",
            description="Execution budgets for the interpreter: step limits, fuel exhaustion traps, and watchdog-driven abort with structured diagnostics.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="set_step_budget",
                    description="set step budget",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="consume_fuel",
                    description="consume fuel",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="check_watchdog",
                    description="check watchdog",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="abort_with_report",
                    description="abort with report",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "set_step_budget")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.resource_limits." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class StackVmBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.stack_vm"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.stack_vm",
            version="1.0.0",
            description="Stack-machine execution semantics: operand-stack discipline for arithmetic, comparison, and load/store opcodes with underflow and overflow guards.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="push_operand",
                    description="push operand",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="pop_operand",
                    description="pop operand",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="eval_binop",
                    description="eval binop",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="guard_stack_limits",
                    description="guard stack limits",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "push_operand")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.stack_vm." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class TailCallBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.tail_call"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.tail_call",
            version="1.0.0",
            description="Proper tail calls in the interpreter: caller-frame reuse on self-tail and mutual-tail sequences, and loop-back conversion.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="detect_tail_position",
                    description="detect tail position",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="reuse_caller_frame",
                    description="reuse caller frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="convert_to_loop",
                    description="convert to loop",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="run_tail_sequence",
                    description="run tail sequence",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "detect_tail_position")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.tail_call." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class TreeEvalBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.tree_eval"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.tree_eval",
            version="1.0.0",
            description="Tree-walking evaluation of an AST: environment-chained variable lookup, statement sequencing, and expression reduction.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="eval_expression",
                    description="eval expression",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="exec_statement",
                    description="exec statement",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="lookup_scoped",
                    description="lookup scoped",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="reduce_node",
                    description="reduce node",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "eval_expression")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.tree_eval." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ValueTaggingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.interp.value_tagging"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.interp.value_tagging",
            version="1.0.0",
            description="Tagged runtime values: small-value immediate tagging, pointer tagging, and tag checks before untagging in arithmetic opcodes.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="tag_immediate",
                    description="tag immediate",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="tag_pointer",
                    description="tag pointer",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="check_tag",
                    description="check tag",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
                DomainletCapability(
                    name="untag_value",
                    description="untag value",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "interp", "runtime"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "tag_immediate")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.interp.value_tagging." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Intrinsics Bindlets
# ═══════════════════════════════════════════════════════════════════

class IntrinsicRecognitionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.intrinsic_recognition"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.intrinsic_recognition",
            version="1.0.0",
            description="intrinsic recognition",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="recognize_intrinsic_idioms",
                    description="recognize intrinsic idioms",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "semantic"],
                ),
                DomainletCapability(
                    name="classify_builtin_patterns",
                    description="classify builtin patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "semantic"],
                ),
                DomainletCapability(
                    name="tag_lowerable_ops",
                    description="tag lowerable ops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "semantic"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "recognize_intrinsic_idioms")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.intrinsic_recognition." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LanePackingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.lane_packing"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.lane_packing",
            version="1.0.0",
            description="lane packing",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="pack_lanes",
                    description="pack lanes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="reshape_vectors",
                    description="reshape vectors",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="legalize_widths",
                    description="legalize widths",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "pack_lanes")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.lane_packing." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class MaskMaterializationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.mask_materialization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.mask_materialization",
            version="1.0.0",
            description="mask materialization",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="materialize_masks",
                    description="materialize masks",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
                DomainletCapability(
                    name="canonicalize_predicates",
                    description="canonicalize predicates",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
                DomainletCapability(
                    name="guard_vector_ops",
                    description="guard vector ops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "materialize_masks")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.mask_materialization." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ReductionLoweringBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.reduction_lowering"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.reduction_lowering",
            version="1.0.0",
            description="reduction lowering",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="recognize_reductions",
                    description="recognize reductions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
                DomainletCapability(
                    name="build_reduction_trees",
                    description="build reduction trees",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
                DomainletCapability(
                    name="lower_horizontal_ops",
                    description="lower horizontal ops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "recognize_reductions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.reduction_lowering." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ShuffleCanonicalizationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.shuffle_canonicalization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.shuffle_canonicalization",
            version="1.0.0",
            description="shuffle canonicalization",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="canonicalize_shuffle",
                    description="canonicalize shuffle",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="fold_lane_permutations",
                    description="fold lane permutations",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="normalize_swizzles",
                    description="normalize swizzles",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "canonicalize_shuffle")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.shuffle_canonicalization." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class VectorLiftBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.intrinsics.vector_lift"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.intrinsics.vector_lift",
            version="1.0.0",
            description="vector lift",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="lift_vector_ops",
                    description="lift vector ops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="widen_scalars",
                    description="widen scalars",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
                DomainletCapability(
                    name="synthesize_lane_types",
                    description="synthesize lane types",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "intrinsics", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "lift_vector_ops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.intrinsics.vector_lift." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Ir Bindlets
# ═══════════════════════════════════════════════════════════════════

class CFGBuilderBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.cfg_builder"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.cfg_builder",
            version="1.0.0",
            description="Build a control-flow graph from structured syntax.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_cfg",
                    description="build cfg",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
                DomainletCapability(
                    name="split_blocks",
                    description="split blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_cfg")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.cfg_builder." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DefUseGraphBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.def_use_graph"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.def_use_graph",
            version="1.0.0",
            description="Link definitions to all reachable uses.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_def_use",
                    description="build def use",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
                DomainletCapability(
                    name="track_uses",
                    description="track uses",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_def_use")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.def_use_graph." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DominatorTreeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.dominator_tree"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.dominator_tree",
            version="1.0.0",
            description="Compute dominator tree and dominance frontiers.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_dominators",
                    description="compute dominators",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
                DomainletCapability(
                    name="compute_frontiers",
                    description="compute frontiers",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_dominators")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.dominator_tree." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LoopNestBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.loop_nest"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.loop_nest",
            version="1.0.0",
            description="Discover natural loops and nested loop structure.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="discover_loops",
                    description="discover loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
                DomainletCapability(
                    name="nest_loops",
                    description="nest loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "discover_loops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.loop_nest." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class Mem2regBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.mem2reg"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.mem2reg",
            version="1.0.0",
            description="Promote stack slots into SSA registers.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="promote_memory",
                    description="promote memory",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
                DomainletCapability(
                    name="scalarize_slots",
                    description="scalarize slots",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "promote_memory")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.mem2reg." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class PhiEliminationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.phi_elimination"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.phi_elimination",
            version="1.0.0",
            description="Remove phi nodes after SSA-based analysis.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="eliminate_phi",
                    description="eliminate phi",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
                DomainletCapability(
                    name="resolve_parallel_copy",
                    description="resolve parallel copy",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "eliminate_phi")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.phi_elimination." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class SSABuilderBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.ssa_builder"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.ssa_builder",
            version="1.0.0",
            description="Convert CFG form to SSA with explicit phi nodes.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_ssa",
                    description="build ssa",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
                DomainletCapability(
                    name="insert_phi",
                    description="insert phi",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_ssa")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.ssa_builder." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ValueNumberingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.ir.value_numbering"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.ir.value_numbering",
            version="1.0.0",
            description="Assign congruence classes to equivalent expressions.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="assign_values",
                    description="assign values",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
                DomainletCapability(
                    name="fold_equivalences",
                    description="fold equivalences",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "ir", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "assign_values")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.ir.value_numbering." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Memory Bindlets
# ═══════════════════════════════════════════════════════════════════

class AllocatorBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.allocator"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.allocator",
            version="1.0.0",
            description="allocator",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="select_allocator",
                    description="select allocator",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="lower_allocate",
                    description="lower allocate",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="lower_deallocate",
                    description="lower deallocate",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="size_class",
                    description="size class",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "select_allocator")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.allocator." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class BorrowCheckerBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.borrow_checker"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.borrow_checker",
            version="1.0.0",
            description="borrow checker",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer_borrows",
                    description="infer borrows",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="check_aliasing",
                    description="check aliasing",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="check_moves",
                    description="check moves",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="emit_diagnostics",
                    description="emit diagnostics",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "infer_borrows")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.borrow_checker." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class EscapeTrackingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.escape_tracking"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.escape_tracking",
            version="1.0.0",
            description="escape tracking",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="collect_uses",
                    description="collect uses",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="propagate_escape",
                    description="propagate escape",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="classify_allocation",
                    description="classify allocation",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="summarize",
                    description="summarize",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "collect_uses")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.escape_tracking." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class GCBarrierBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.gc_barrier"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.gc_barrier",
            version="1.0.0",
            description="gc barrier",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="identify_pointer_stores",
                    description="identify pointer stores",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="insert_write_barrier",
                    description="insert write barrier",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="insert_read_barrier",
                    description="insert read barrier",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="insert_safepoint",
                    description="insert safepoint",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "identify_pointer_stores")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.gc_barrier." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LayoutBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.layout"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.layout",
            version="1.0.0",
            description="layout",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="layout_struct",
                    description="layout struct",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="layout_union",
                    description="layout union",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="layout_array",
                    description="layout array",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
                DomainletCapability(
                    name="layout_object",
                    description="layout object",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "semantic"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "layout_struct")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.layout." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LifetimeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.lifetime"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.lifetime",
            version="1.0.0",
            description="lifetime",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_liveness",
                    description="compute liveness",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="place_drops",
                    description="place drops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="extend_lifetime",
                    description="extend lifetime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="check_use_after_free",
                    description="check use after free",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_liveness")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.lifetime." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class RegionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.region"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.region",
            version="1.0.0",
            description="region",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer_regions",
                    description="infer regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="assign_allocations",
                    description="assign allocations",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="insert_region_exit",
                    description="insert region exit",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
                DomainletCapability(
                    name="coalesce_regions",
                    description="coalesce regions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "analysis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "infer_regions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.region." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class SanitizerBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.memory.sanitizer"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.memory.sanitizer",
            version="1.0.0",
            description="sanitizer",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="instrument_bounds",
                    description="instrument bounds",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="instrument_lifetime",
                    description="instrument lifetime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="instrument_race",
                    description="instrument race",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
                DomainletCapability(
                    name="emit_runtime_hooks",
                    description="emit runtime hooks",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "memory", "lowering"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "instrument_bounds")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.memory.sanitizer." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Optimization Bindlets
# ═══════════════════════════════════════════════════════════════════

class CommonSubexpressionEliminationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.common_subexpression_elimination"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.common_subexpression_elimination",
            version="1.0.0",
            description="Reuse repeated computations within a dominance region.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_common_subexpressions",
                    description="find common subexpressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="reuse_values",
                    description="reuse values",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "find_common_subexpressions")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.common_subexpression_elimination." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ConstantFoldingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.constant_folding"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.constant_folding",
            version="1.0.0",
            description="Replace constant expressions with literal values.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="fold_constants",
                    description="fold constants",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="propagate_literals",
                    description="propagate literals",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "fold_constants")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.constant_folding." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class CopyPropagationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.copy_propagation"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.copy_propagation",
            version="1.0.0",
            description="Replace redundant copies with their source values.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="propagate_copies",
                    description="propagate copies",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="collapse_moves",
                    description="collapse moves",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "propagate_copies")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.copy_propagation." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DeadCodeEliminationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.dead_code_elimination"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.dead_code_elimination",
            version="1.0.0",
            description="Remove instructions and blocks with no observable effect.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="eliminate_dead_code",
                    description="eliminate dead code",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="prune_unreachable",
                    description="prune unreachable",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "eliminate_dead_code")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.dead_code_elimination." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class DeadStoreEliminationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.dead_store_elimination"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.dead_store_elimination",
            version="1.0.0",
            description="Drop stores that are overwritten before being read.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="remove_dead_stores",
                    description="remove dead stores",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="trim_memory_writes",
                    description="trim memory writes",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "remove_dead_stores")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.dead_store_elimination." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class GlobalValueNumberingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.global_value_numbering"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.global_value_numbering",
            version="1.0.0",
            description="Canonicalize equivalent expressions across the function.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="number_values",
                    description="number values",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="merge_equivalences",
                    description="merge equivalences",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "number_values")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.global_value_numbering." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class InliningBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.inlining"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.inlining",
            version="1.0.0",
            description="Inline hot or trivially small call sites.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="inline_calls",
                    description="inline calls",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="clone_function_body",
                    description="clone function body",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "inline_calls")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.inlining." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LoopInterchangeBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.loop_interchange"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.loop_interchange",
            version="1.0.0",
            description="Swap nested loops to improve locality and vectorization.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="interchange_loops",
                    description="interchange loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="reshape_iteration_space",
                    description="reshape iteration space",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "interchange_loops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.loop_interchange." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LoopUnrollingBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.loop_unrolling"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.loop_unrolling",
            version="1.0.0",
            description="Replicate loop bodies to reduce control overhead.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="unroll_loops",
                    description="unroll loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="replicate_bodies",
                    description="replicate bodies",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "unroll_loops")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.loop_unrolling." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ScalarReplacementOfAggregatesBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.scalar_replacement_of_aggregates"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.scalar_replacement_of_aggregates",
            version="1.0.0",
            description="Break aggregates into scalar pieces for faster SSA.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="split_aggregates",
                    description="split aggregates",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="promote_fields",
                    description="promote fields",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "split_aggregates")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.scalar_replacement_of_aggregates." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class SparseConditionalConstantPropagationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.sparse_conditional_constant_propagation"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.sparse_conditional_constant_propagation",
            version="1.0.0",
            description="Propagate constants and eliminate infeasible control flow.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="propagate_constants",
                    description="propagate constants",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="prune_feasible_edges",
                    description="prune feasible edges",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "propagate_constants")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.sparse_conditional_constant_propagation." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class StrengthReductionBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.optimization.strength_reduction"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.optimization.strength_reduction",
            version="1.0.0",
            description="Rewrite expensive operations into cheaper equivalents.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="reduce_strength",
                    description="reduce strength",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
                DomainletCapability(
                    name="rewrite_inductions",
                    description="rewrite inductions",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "optimization", "transform"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "reduce_strength")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.optimization.strength_reduction." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Runtime Bindlets
# ═══════════════════════════════════════════════════════════════════

class DynDispatchBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.runtime.dyn_dispatch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.runtime.dyn_dispatch",
            version="1.0.0",
            description="Dynamic-dispatch runtime: virtual call resolution with polymorphic inline-cache (PIC) support for method lookups.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="resolve_virtual",
                    description="resolve virtual",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="cache_method",
                    description="cache method",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="guard_polymorphic",
                    description="guard polymorphic",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="emit_inline_cache",
                    description="emit inline cache",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "resolve_virtual")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.runtime.dyn_dispatch." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class GCSupportBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.runtime.gc_support"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.runtime.gc_support",
            version="1.0.0",
            description="Tracing garbage-collector support: walk root set from stack maps, mark reachable objects, sweep dead allocations.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="walk_roots",
                    description="walk roots",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="trace_heap",
                    description="trace heap",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="mark_reachable",
                    description="mark reachable",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="sweep_unmarked",
                    description="sweep unmarked",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "walk_roots")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.runtime.gc_support." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class RTTIBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.runtime.rtti"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.runtime.rtti",
            version="1.0.0",
            description="Run-Time Type Information: emit type-id assignments, type-info tables, and dynamic-cast checking sequences.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_typeid",
                    description="emit typeid",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="emit_typeinfo",
                    description="emit typeinfo",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="dynamic_cast_check",
                    description="dynamic cast check",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="emit_base_offset",
                    description="emit base offset",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "emit_typeid")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.runtime.rtti." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class StackUnwindBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.runtime.stack_unwind"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.runtime.stack_unwind",
            version="1.0.0",
            description="Stack-unwinding engine: walk call frames, restore callee-saved registers, locate exception handlers via LSDA tables.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="unwind_frame",
                    description="unwind frame",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="restore_callee_saved",
                    description="restore callee saved",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="find_handler",
                    description="find handler",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
                DomainletCapability(
                    name="phase1_search",
                    description="phase1 search",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "runtime", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "unwind_frame")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.runtime.stack_unwind." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Support Bindlets
# ═══════════════════════════════════════════════════════════════════

class CallGraphBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.support.call_graph"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.support.call_graph",
            version="1.0.0",
            description="Call-graph construction: build from IR call sites, compute strongly-connected components, topologically sort for bottom-up passes.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build_from_ir",
                    description="build from ir",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="compute_scc",
                    description="compute scc",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="topological_sort",
                    description="topological sort",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="detect_recursion",
                    description="detect recursion",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build_from_ir")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.support.call_graph." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class LatticeOpsBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.support.lattice_ops"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.support.lattice_ops",
            version="1.0.0",
            description="Generic lattice operations: meet, join, partial-order comparison, and transfer-function enumeration for abstract interpretation.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_meet",
                    description="compute meet",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="compute_join",
                    description="compute join",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="check_partial_order",
                    description="check partial order",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
                DomainletCapability(
                    name="enumerate_transfer",
                    description="enumerate transfer",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "support", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_meet")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.support.lattice_ops." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Transformer Bindlets
# ═══════════════════════════════════════════════════════════════════

class LoopUnswitchBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.transformer.loop_unswitch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.transformer.loop_unswitch",
            version="1.0.0",
            description="Loop unswitching: hoist invariant conditional branches outside loops, duplicating the loop body per branch direction.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_invariant_condition",
                    description="find invariant condition",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="clone_loop_body",
                    description="clone loop body",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="hoist_branch_outside",
                    description="hoist branch outside",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "find_invariant_condition")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.transformer.loop_unswitch." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class OutliningBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.transformer.outlining"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.transformer.outlining",
            version="1.0.0",
            description="Function outlining: extract cold or rarely-executed code regions into separate functions to improve I-cache density.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_cold_region",
                    description="find cold region",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="extract_to_function",
                    description="extract to function",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="insert_call",
                    description="insert call",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="remap_returns",
                    description="remap returns",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "find_cold_region")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.transformer.outlining." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ScalarReplacementBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.transformer.scalar_replacement"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.transformer.scalar_replacement",
            version="1.0.0",
            description="Scalar replacement of aggregates: decompose struct/array allocations into individual scalar variables for better register allocation.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="find_aggregate_alloca",
                    description="find aggregate alloca",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="split_into_scalars",
                    description="split into scalars",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="rewrite_accesses",
                    description="rewrite accesses",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
                DomainletCapability(
                    name="dead_aggregate_removal",
                    description="dead aggregate removal",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "transformer", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "find_aggregate_alloca")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.transformer.scalar_replacement." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Validator Bindlets
# ═══════════════════════════════════════════════════════════════════

class InitializationBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.validator.initialization"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.validator.initialization",
            version="1.0.0",
            description="Definite-initialization analysis: ensure every local variable is assigned before first read on all CFG paths.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="track_definiteness",
                    description="track definiteness",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_before_use",
                    description="check before use",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="warn_uninitialized",
                    description="warn uninitialized",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "track_definiteness")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.validator.initialization." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class IRInvariantsBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.validator.ir_invariants"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.validator.ir_invariants",
            version="1.0.0",
            description="IR-level invariant validation: verify SSA well-formedness, phi-node placement, def-use chain consistency, and dominator tree integrity.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_ssa_properties",
                    description="check ssa properties",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_phi_consistency",
                    description="check phi consistency",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_def_use_wellformedness",
                    description="check def use wellformedness",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_dominator_tree",
                    description="check dominator tree",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_ssa_properties")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.validator.ir_invariants." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class OwnershipBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.validator.ownership_borrowck"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.validator.ownership",
            version="1.0.0",
            description="Ownership and borrow-checking: track move semantics, borrow lifetimes, and detect use-after-move or mutable aliasing violations.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="track_ownership",
                    description="track ownership",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_borrow_lifetime",
                    description="check borrow lifetime",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="detect_use_after_move",
                    description="detect use after move",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="check_mut_aliasing",
                    description="check mut aliasing",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "track_ownership")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.validator.ownership." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]

class ReachabilityBindlet(BaseLangsysBindlet):
    _kernel_module = "domainlet.langsys.validator.reachability"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="langsys.validator.reachability",
            version="1.0.0",
            description="Static reachability analysis: flag unreachable code after return/break/continue/panic, detect dead basic blocks in CFG.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute_reachable",
                    description="compute reachable",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="warn_unreachable_code",
                    description="warn unreachable code",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
                DomainletCapability(
                    name="detect_dead_blocks",
                    description="detect dead blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["langsys", "validator", "middle"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute_reachable")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id="langsys.validator.reachability." + self._capability,
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Registry
# ═══════════════════════════════════════════════════════════════════

LANGSYS_BINDLETS: dict[str, type[BaseLangsysBindlet]] = {
    "escape_analysis": EscapeAnalysisBindlet,
    "points_to": PointsToBindlet,
    "side_effect": SideEffectBindlet,
    "atomic_normalization": AtomicNormalizationBindlet,
    "barrier_placement": BarrierPlacementBindlet,
    "critical_sections": CriticalSectionsBindlet,
    "happens_before": HappensBeforeBindlet,
    "interleaving_model": InterleavingModelBindlet,
    "parallel_region": ParallelRegionBindlet,
    "race_detection": RAceDetectionBindlet,
    "task_graph": TaskGraphBindlet,
    "xcg": XcgBindlet,
    "xext": XextBindlet,
    "xpg": XpgBindlet,
    "xphase": XphaseBindlet,
    "xsem": XsemBindlet,
    "xst": XstBindlet,
    "xtr": XtrBindlet,
    "xweave": XweaveBindlet,
    "abi_lowering": ABILoweringBindlet,
    "call_marshalling": CallMarshallingBindlet,
    "callback_bridge": CallbackBridgeBindlet,
    "closure_conversion": ClosureConversionBindlet,
    "ffi_type_mapping": FFITypeMappingBindlet,
    "foreign_exception": ForeignExceptionBindlet,
    "native_symbol": NativeSymbolBindlet,
    "variadic_adapter": VariadicAdapterBindlet,
    "constant_pool": ConstantPoolBindlet,
    "debug_info": DebugInfoBindlet,
    "exception_table": ExceptionTableBindlet,
    "gc_metadata": GCMetadataBindlet,
    "trampoline": TrampolineBindlet,
    "async_bridge": AsyncBridgeBindlet,
    "ipc_bridge": IpcBridgeBindlet,
    "module_bridge": ModuleBridgeBindlet,
    "object_protocol": ObjectProtocolBindlet,
    "ownership_bridge": OwnershipBridgeBindlet,
    "plugin_boundary": PluginBoundaryBindlet,
    "serialization_bridge": SerializationBridgeBindlet,
    "version_handshake": VersionHandshakeBindlet,
    "builtin_dispatch": BuiltinDispatchBindlet,
    "bytecode_decoder": BytecodeDecoderBindlet,
    "bytecode_encoder": BytecodeEncoderBindlet,
    "bytecode_verifier": BytecodeVerifierBindlet,
    "call_counter": CallCounterBindlet,
    "closure": ClosureBindlet,
    "constant_pool": ConstantPoolBindlet,
    "control_flow": ControlFlowBindlet,
    "cps_eval": CPSEvalBindlet,
    "debugger": DebuggerBindlet,
    "deopt": DeoptBindlet,
    "dispatch": DispatchBindlet,
    "environment_chain": EnvironmentChainBindlet,
    "exception": ExceptionBindlet,
    "frame": FrameBindlet,
    "inline_cache": InlineCacheBindlet,
    "line_table": LineTableBindlet,
    "method_dispatch": MethodDispatchBindlet,
    "native_bindings": NativeBindingsBindlet,
    "object_model": ObjectModelBindlet,
    "opcode": OpcodeBindlet,
    "osr": OSRBindlet,
    "peephole": PeepholeBindlet,
    "polymorphic_inline_cache": PolymorphicInlineCacheBindlet,
    "register_vm": RegisterVmBindlet,
    "resource_limits": ResourceLimitsBindlet,
    "stack_vm": StackVmBindlet,
    "tail_call": TailCallBindlet,
    "tree_eval": TreeEvalBindlet,
    "value_tagging": ValueTaggingBindlet,
    "intrinsic_recognition": IntrinsicRecognitionBindlet,
    "lane_packing": LanePackingBindlet,
    "mask_materialization": MaskMaterializationBindlet,
    "reduction_lowering": ReductionLoweringBindlet,
    "shuffle_canonicalization": ShuffleCanonicalizationBindlet,
    "vector_lift": VectorLiftBindlet,
    "cfg_builder": CFGBuilderBindlet,
    "def_use_graph": DefUseGraphBindlet,
    "dominator_tree": DominatorTreeBindlet,
    "loop_nest": LoopNestBindlet,
    "mem2reg": Mem2regBindlet,
    "phi_elimination": PhiEliminationBindlet,
    "ssa_builder": SSABuilderBindlet,
    "value_numbering": ValueNumberingBindlet,
    "allocator": AllocatorBindlet,
    "borrow_checker": BorrowCheckerBindlet,
    "escape_tracking": EscapeTrackingBindlet,
    "gc_barrier": GCBarrierBindlet,
    "layout": LayoutBindlet,
    "lifetime": LifetimeBindlet,
    "region": RegionBindlet,
    "sanitizer": SanitizerBindlet,
    "common_subexpression_elimination": CommonSubexpressionEliminationBindlet,
    "constant_folding": ConstantFoldingBindlet,
    "copy_propagation": CopyPropagationBindlet,
    "dead_code_elimination": DeadCodeEliminationBindlet,
    "dead_store_elimination": DeadStoreEliminationBindlet,
    "global_value_numbering": GlobalValueNumberingBindlet,
    "inlining": InliningBindlet,
    "loop_interchange": LoopInterchangeBindlet,
    "loop_unrolling": LoopUnrollingBindlet,
    "scalar_replacement_of_aggregates": ScalarReplacementOfAggregatesBindlet,
    "sparse_conditional_constant_propagation": SparseConditionalConstantPropagationBindlet,
    "strength_reduction": StrengthReductionBindlet,
    "dyn_dispatch": DynDispatchBindlet,
    "gc_support": GCSupportBindlet,
    "rtti": RTTIBindlet,
    "stack_unwind": StackUnwindBindlet,
    "call_graph": CallGraphBindlet,
    "lattice_ops": LatticeOpsBindlet,
    "loop_unswitch": LoopUnswitchBindlet,
    "outlining": OutliningBindlet,
    "scalar_replacement": ScalarReplacementBindlet,
    "initialization": InitializationBindlet,
    "ir_invariants": IRInvariantsBindlet,
    "ownership": OwnershipBindlet,
    "reachability": ReachabilityBindlet,
}

class ExtraLangsysBindlet(BaseLangsysBindlet):
    _kernel_module = ""
    def probe(self) -> DomainletInfo:
        kernel = self._get_kernel()
        return DomainletInfo(name=kernel.name, version="1.0.0", description=kernel.description, author="domweave",
            capabilities=[DomainletCapability(name=op, description=op, input_kind=InputKind.AST_JSON, tags=["langsys", kernel.category]) for op in kernel.operations])
    def configure(self, config: dict[str, str]) -> None: self._capability = config.get("capability", self._get_kernel().operations[0])
    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [AnalysisIssue(rule_id=f"{self._get_kernel().name}.{self._capability}", message=json.dumps(value), severity=Severity.INFORMATION, confidence=Confidence.HIGH)]

for _name, _module in {
    "analyzer_nullability":"domainlet.langsys.analyzer.nullability", "analyzer_taint":"domainlet.langsys.analyzer.taint", "ir_loop_simplify":"domainlet.langsys.ir.loop_simplify", "ir_critical_edges":"domainlet.langsys.ir.critical_edges",
    "optimization_loop_peeling":"domainlet.langsys.optimization.loop_peeling", "optimization_range_analysis":"domainlet.langsys.optimization.range_analysis", "memory_stack_promotion":"domainlet.langsys.memory.stack_promotion", "memory_alias_summary":"domainlet.langsys.memory.alias_summary",
    "interp_instruction_trace":"domainlet.langsys.interp.instruction_trace", "runtime_safepoint":"domainlet.langsys.runtime.safepoint", "transformer_jump_threading":"domainlet.langsys.transformer.jump_threading", "validator_type_soundness":"domainlet.langsys.validator.type_soundness",
    "ffi_ownership_transfer":"domainlet.langsys.ffi.ownership_transfer", "concurrency_lockset":"domainlet.langsys.concurrency.lockset", "generator_stack_maps":"domainlet.langsys.generator.stack_maps"}.items():
    LANGSYS_BINDLETS[_name] = type(f"{_name.title().replace('_','')}Bindlet", (ExtraLangsysBindlet,), {"_kernel_module": _module})


def get_langsys_bindlet(name: str) -> BaseLangsysBindlet:
    """Factory for creating langsys bindlets by name."""
    cls = LANGSYS_BINDLETS.get(name)
    if cls is None:
        raise ValueError(f"Unknown langsys bindlet: {name!r}. Available: {sorted(LANGSYS_BINDLETS)}")
    return cls()
