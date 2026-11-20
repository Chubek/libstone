"""dynkern_bindlet — Bindlet for dynamic analysis kernels.

Discovers, validates, and composes dynkern kernels into a pipeline.
Provides a single entry point for running dynamic analyses.
"""
from __future__ import annotations

import importlib
import os
from collections import deque
from typing import Any, Dict, List, Optional, Set, Tuple

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    BindletError,
    BindletErrorCode,
    Confidence,
    DomainletCapability,
    DomainletInfo,
    InputKind,
    Severity,
    SourceSpan,
)
from domainlet.dynkern.base import (
    Kernel,
    KernelDescriptor,
    Report,
    SimpleReport,
    StreamKind,
)
from domainlet.dynkern.mex import MexBlock
from domainlet.dynkern.weave import Weave


# ══════════════════════════════════════════════════════════════════════
#  Registry
# ══════════════════════════════════════════════════════════════════════

class KernelRegistry:
    """Registry of all discovered dynkern kernels."""

    def __init__(self) -> None:
        self._descriptors: Dict[str, KernelDescriptor] = {}

    def register(self, descriptor: KernelDescriptor) -> None:
        self._descriptors[descriptor.name] = descriptor

    def get(self, name: str) -> KernelDescriptor:
        if name not in self._descriptors:
            raise KeyError(f"Kernel '{name}' not found in registry")
        return self._descriptors[name]

    def all(self) -> List[KernelDescriptor]:
        return list(self._descriptors.values())

    def __contains__(self, name: str) -> bool:
        return name in self._descriptors


# ══════════════════════════════════════════════════════════════════════
#  Discovery
# ══════════════════════════════════════════════════════════════════════

_KERNEL_MODULES: List[str] = [
    "mex",
    "shade",
    "weave",
    "prop",
    "def_",
    "addr",
    "taint",
    "sym",
    "range_",
    "cover",
    "trace",
    "cg",
    "micro",
    "sync",
    "hb",
    "lockset",
    "heap",
    "constr",
    "replay",
    "sched",
]


def discover_kernels() -> KernelRegistry:
    """Import all dynkern modules and collect their KERNEL descriptors."""
    registry = KernelRegistry()
    for mod_name in _KERNEL_MODULES:
        try:
            mod = importlib.import_module(f"domainlet.dynkern.{mod_name}")
            if hasattr(mod, "KERNEL"):
                descriptor = mod.KERNEL
                if isinstance(descriptor, KernelDescriptor):
                    registry.register(descriptor)
        except ImportError as exc:
            # Module not available (e.g. missing optional dependency)
            pass
    return registry


# ══════════════════════════════════════════════════════════════════════
#  Dependency resolution
# ══════════════════════════════════════════════════════════════════════

def resolve_dependencies(
    names: List[str],
    registry: KernelRegistry,
) -> List[KernelDescriptor]:
    """Topologically order kernels by depends_on.

    Raises ValueError on cycles or missing dependencies.
    """
    # Collect all required kernels (transitive closure)
    required: Set[str] = set()
    queue: deque[str] = deque(names)

    while queue:
        name = queue.popleft()
        if name in required:
            continue
        required.add(name)
        descriptor = registry.get(name)
        for dep in descriptor.depends_on:
            if dep not in required:
                queue.append(dep)

    # Topological sort (Kahn's algorithm)
    in_degree: Dict[str, int] = {name: 0 for name in required}
    adj: Dict[str, List[str]] = {name: [] for name in required}

    for name in required:
        descriptor = registry.get(name)
        for dep in descriptor.depends_on:
            if dep not in required:
                raise ValueError(
                    f"Missing dependency: '{name}' requires '{dep}', "
                    f"but '{dep}' is not in the registry"
                )
            adj[dep].append(name)
            in_degree[name] = in_degree.get(name, 0) + 1

    sorted_names: List[str] = []
    zero_degree = deque(n for n in required if in_degree[n] == 0)

    while zero_degree:
        node = zero_degree.popleft()
        sorted_names.append(node)
        for neighbor in adj[node]:
            in_degree[neighbor] -= 1
            if in_degree[neighbor] == 0:
                zero_degree.append(neighbor)

    if len(sorted_names) != len(required):
        raise ValueError(
            f"Cycle detected in kernel dependencies among: {required}"
        )

    return [registry.get(name) for name in sorted_names]


# ══════════════════════════════════════════════════════════════════════
#  Stream validation
# ══════════════════════════════════════════════════════════════════════

def validate_streams(
    descriptors: List[KernelDescriptor],
    available_streams: StreamKind,
) -> None:
    """Check that each kernel's stream requirement is met by the pipeline."""
    for desc in descriptors:
        if desc.streams not in available_streams:
            if not (available_streams & desc.streams):
                raise ValueError(
                    f"Kernel '{desc.name}' requires stream {desc.streams.name} "
                    f"but pipeline only provides {available_streams.name}"
                )


# ══════════════════════════════════════════════════════════════════════
#  Pipeline
# ══════════════════════════════════════════════════════════════════════

class Pipeline:
    """A composed pipeline of dynkern kernels."""

    def __init__(
        self,
        kernels: List[Kernel],
        weave: Weave,
        registry: KernelRegistry,
    ) -> None:
        self._kernels = kernels
        self._weave = weave
        self._registry = registry

    def run(self, blocks: List[MexBlock]) -> List[Report]:
        """Run all kernels on the given MEX blocks."""
        self._weave.on_start()
        self._weave.lower_all(blocks)
        return self._weave.on_finish()

    @property
    def kernels(self) -> List[Kernel]:
        return list(self._kernels)


def build_pipeline(
    names: List[str],
    registry: Optional[KernelRegistry] = None,
) -> Pipeline:
    """Build a pipeline of kernels with transitive dependencies.

    Parameters
    ----------
    names:
        Names of the requested kernels.
    registry:
        Kernel registry.  If None, auto-discovered.

    Returns
    -------
    Pipeline
        A composed pipeline ready to run.

    Raises
    ------
    ValueError
        If a dependency is missing or a cycle is detected.
    """
    if registry is None:
        registry = discover_kernels()

    descriptors = resolve_dependencies(names, registry)

    # Determine available streams based on what kernels need
    available = StreamKind.CONCRETE
    for desc in descriptors:
        available |= desc.streams

    validate_streams(descriptors, available)

    # Instantiate kernels
    kernels: List[Kernel] = []
    for desc in descriptors:
        kernel = desc.factory()
        kernels.append(kernel)

    weave = Weave(kernels)
    return Pipeline(kernels=kernels, weave=weave, registry=registry)


# ══════════════════════════════════════════════════════════════════════
#  Bindlet integration
# ══════════════════════════════════════════════════════════════════════

class DynkernBindlet(AbstractDomainlet):
    """Bindlet for dynamic analysis kernels.

    Provides a unified interface to the dynkern family through the
    standard domainlet lifecycle.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="dynkern",
            version="0.1.0",
            description="Dynamic Analysis Kernel Family — composable dynamic analyses",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="list-kernels",
                    description="List all available dynkern kernels",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "discovery"],
                ),
                DomainletCapability(
                    name="run-pipeline",
                    description="Build and run a pipeline of dynkern kernels",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "pipeline", "dynamic-analysis"],
                ),
                DomainletCapability(
                    name="def",
                    description="Run the DEF (definedness) kernel",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "def", "uninitialized"],
                ),
                DomainletCapability(
                    name="taint",
                    description="Run the TAINT kernel",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "taint", "data-flow"],
                ),
                DomainletCapability(
                    name="addr",
                    description="Run the ADDR (addressability) kernel",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "addr", "oob", "uaf"],
                ),
                DomainletCapability(
                    name="cover",
                    description="Run the COVER (coverage) kernel",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "cover", "fuzzing"],
                ),
                DomainletCapability(
                    name="hb",
                    description="Run the HB (happens-before) kernel",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["dynkern", "hb", "race", "concurrency"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "list-kernels")
        self._kernel_names: List[str] = []
        if "kernels" in config:
            self._kernel_names = [
                k.strip() for k in config["kernels"].split(",") if k.strip()
            ]

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        issues: list[AnalysisIssue] = []

        try:
            registry = discover_kernels()

            if self._capability == "list-kernels":
                for desc in registry.all():
                    issues.append(AnalysisIssue(
                        rule_id="dynkern.list",
                        message=f"Kernel '{desc.name}' ({desc.kind}): "
                                f"streams={desc.streams.name}, "
                                f"deps={list(desc.depends_on)}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    ))
                return issues

            # Run a pipeline
            if not self._kernel_names:
                # Default: run the requested capability as a kernel name
                self._kernel_names = [self._capability.upper()]

            pipeline = build_pipeline(self._kernel_names, registry)

            # Create a simple MEX block for testing
            from domainlet.dynkern.mex import MexBlock, MexOp, MexOpKind, MexStmt, MexTemp

            block = MexBlock(id=0, addr=0x1000)
            block.add_stmt(MexStmt(
                dst=MexTemp(id=0, size=4, type_name="i32"),
                op=MexOp.MOV,
                args=(),
                kind=MexOpKind.CONST,
                addr=0x1000,
                comment="test mov",
            ))

            reports = pipeline.run([block])

            for report in reports:
                for finding in report.findings:
                    issues.append(AnalysisIssue(
                        rule_id=f"dynkern.{finding.rule_id}",
                        message=f"[{finding.kernel}] {finding.message}",
                        severity=Severity.WARNING,
                        confidence=Confidence.MEDIUM,
                        extra=finding.extra,
                    ))

                # Also report counters
                issues.append(AnalysisIssue(
                    rule_id="dynkern.counters",
                    message=f"[{report.kernel_name}] {report.summary()}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                ))

        except Exception as exc:
            issues.append(AnalysisIssue(
                rule_id="dynkern.error",
                message=f"Dynkern execution failed: {exc}",
                severity=Severity.ERROR,
                confidence=Confidence.LOW,
            ))

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Module-level convenience API
# ══════════════════════════════════════════════════════════════════════

def run_pipeline(
    names: List[str],
    blocks: Optional[List[MexBlock]] = None,
    registry: Optional[KernelRegistry] = None,
) -> List[Report]:
    """Convenience function: build and run a pipeline.

    Parameters
    ----------
    names:
        Names of kernels to include.
    blocks:
        MEX blocks to process.  If None, an empty list is used.
    registry:
        Kernel registry.  If None, auto-discovered.

    Returns
    -------
    list[Report]
        One report per kernel in the pipeline.
    """
    pipeline = build_pipeline(names, registry)
    return pipeline.run(blocks or [])


# ══════════════════════════════════════════════════════════════════════
#  Module-level exports
# ══════════════════════════════════════════════════════════════════════

__all__ = [
    "DynkernBindlet",
    "KernelRegistry",
    "Pipeline",
    "build_pipeline",
    "discover_kernels",
    "resolve_dependencies",
    "run_pipeline",
    "validate_streams",
]
