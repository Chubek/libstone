"""Bindlet exposing the hardened build-system domainlets through Domfix."""

from __future__ import annotations

import json
from collections.abc import Mapping
from importlib import import_module
from typing import Any

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
)

_ADAPTERS = {
    "cmake": "cmake",
    "ninja": "ninja",
    "make": "make",
    "bazel": "bazel",
    "meson": "meson",
}
_CAPABILITIES = {
    "parse",
    "resolve",
    "depfile",
    "serialize",
    "manifest",
    "provenance",
    "schedule",
}

ADVERTISEMENT = {
    "name": "buildsys",
    "version": "0.2.0",
    "capabilities": {
        "operations": sorted(_CAPABILITIES),
        "input_types": ["source_text"],
        "output_types": ["issues", "dependency_graph", "manifest", "provenance"],
    },
    "resources": {"device": "cpu"},
    "endpoints": {},
    "metadata": {
        "description": "Hardened build graph, cache, execution, and adapter kernels",
        "operations": sorted(_CAPABILITIES),
    },
}


def get_advertisement() -> dict[str, Any]:
    """Return a defensive copy for runtime discovery."""
    return {
        **ADVERTISEMENT,
        "capabilities": dict(ADVERTISEMENT["capabilities"]),
        "metadata": dict(ADVERTISEMENT["metadata"]),
    }


def _json_data(value: Any) -> Any:
    if isinstance(value, str):
        try:
            return json.loads(value)
        except json.JSONDecodeError:
            return value
    return value


class BuildSysBindlet(AbstractDomainlet):
    """Run build graph, manifest, provenance, and scheduling kernels."""

    _adapter = "make"
    _capability = "parse"
    _format = "json"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="buildsys",
            version="0.2.0",
            description=(
                "Build dependency graphs, depfiles, manifests, provenance, "
                "scheduling, and build-file adapters"
            ),
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="parse",
                    description="Parse a CMake, Ninja, Make, Bazel, or Meson definition",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "dependency_graph"],
                    tags=["build", "graph", "adapter"],
                ),
                DomainletCapability(
                    name="resolve",
                    description="Resolve dependencies and report cycles",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "dependency_graph"],
                    tags=["build", "topological-sort", "cycle-detection"],
                ),
                DomainletCapability(
                    name="depfile",
                    description="Parse compiler-generated dependency files",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "dependency_graph"],
                    tags=["build", "depfile", "generated-headers"],
                ),
                DomainletCapability(
                    name="serialize",
                    description="Export an adapter graph as stable JSON or DOT",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "dependency_graph"],
                    tags=["build", "graph", "serialization"],
                ),
                DomainletCapability(
                    name="manifest",
                    description="Validate and canonicalize a JSON build manifest",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "manifest"],
                    tags=["build", "schema", "validation"],
                ),
                DomainletCapability(
                    name="provenance",
                    description="Create a stable action provenance record",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "provenance"],
                    tags=["build", "reproducibility", "fingerprint"],
                ),
                DomainletCapability(
                    name="schedule",
                    description="Plan dependency-ready resource-bounded execution waves",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "schedule"],
                    tags=["build", "parallelism", "resources"],
                ),
            ],
            config_schema={
                "adapter": "cmake|ninja|make|bazel|meson (default: make)",
                "capability": "|".join(sorted(_CAPABILITIES)),
                "format": "json|dot for serialize (default: json)",
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        adapter = config.get("adapter", "make").lower()
        capability = config.get("capability", "parse").lower()
        if adapter not in _ADAPTERS:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown build adapter {adapter!r}",
                "BuildSysBindlet",
            )
        if capability not in _CAPABILITIES:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown capability {capability!r}",
                "BuildSysBindlet",
            )
        output_format = config.get("format", "json").lower()
        if output_format not in {"json", "dot"}:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                "format must be 'json' or 'dot'",
                "BuildSysBindlet",
            )
        self._adapter = adapter
        self._capability = capability
        self._format = output_format

    def _parse_adapter(self, data: Any):
        if not isinstance(data, str):
            raise ValueError("adapter capabilities require source text")
        module = import_module(f"domainlet.buildsys.{_ADAPTERS[self._adapter]}")
        return module.parse(data)

    @staticmethod
    def _graph_payload(graph) -> dict[str, Any]:
        return {
            name: {
                "deps": list(target.deps),
                "outputs": list(target.outputs),
                "metadata": target.metadata,
            }
            for name, target in sorted(graph.nodes.items())
        }

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        data = getattr(context, "data", None)
        try:
            if self._capability in {"parse", "resolve", "serialize"}:
                graph = self._parse_adapter(data)
                mapping = {name: target.deps for name, target in graph.nodes.items()}
                from domainlet.buildsys.dep_resolve import detect_cycles, topological_sort

                cycles = detect_cycles(mapping)
                if self._capability == "resolve":
                    if cycles:
                        return [
                            AnalysisIssue(
                                rule_id="buildsys.cycle",
                                message=f"{self._adapter}: dependency cycle(s): {cycles}",
                                severity=Severity.ERROR,
                                confidence=Confidence.HIGH,
                            )
                        ]
                    order = topological_sort(mapping)
                    return [
                        AnalysisIssue(
                            rule_id="buildsys.order",
                            message="build order: " + " -> ".join(order),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                            extra={"order": json.dumps(order)},
                        )
                    ]
                if self._capability == "serialize":
                    from domainlet.buildsys.graph_io import dumps, to_dot

                    payload = to_dot(graph) if self._format == "dot" else dumps(graph)
                    return [
                        AnalysisIssue(
                            rule_id="buildsys.graph",
                            message=f"{self._adapter}: serialized {len(graph.nodes)} target(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                            extra={"format": self._format, "graph": payload},
                        )
                    ]
                message = (
                    f"{self._adapter}: {len(graph.nodes)} targets, "
                    f"{sum(len(target.deps) for target in graph.nodes.values())} dependencies"
                )
                if cycles:
                    message += f", {len(cycles)} cycle(s)"
                return [
                    AnalysisIssue(
                        rule_id="buildsys.graph",
                        message=message,
                        severity=Severity.WARNING if cycles else Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                        extra={"graph": json.dumps(self._graph_payload(graph), sort_keys=True)},
                    )
                ]

            if self._capability == "depfile":
                from domainlet.buildsys.depfile import parse

                rules = parse(data)
                return [
                    AnalysisIssue(
                        rule_id="buildsys.depfile",
                        message=f"parsed {len(rules)} depfile target(s)",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                        extra={"rules": json.dumps(rules, sort_keys=True)},
                    )
                ]

            if self._capability == "manifest":
                from domainlet.buildsys.manifest import validate

                normalized = validate(_json_data(data))
                return [
                    AnalysisIssue(
                        rule_id="buildsys.manifest",
                        message=f"validated {len(normalized['targets'])} manifest target(s)",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                        extra={"manifest": json.dumps(normalized, sort_keys=True)},
                    )
                ]

            if self._capability == "provenance":
                from domainlet.buildsys.core import BuildAction
                from domainlet.buildsys.provenance import ActionProvenance

                value = _json_data(data)
                if not isinstance(value, Mapping):
                    raise ValueError("provenance input must be an action mapping")
                action = BuildAction(
                    value.get("command", value.get("argv")),
                    cwd=value.get("cwd"),
                    env=dict(value.get("env", {})),
                    inputs=list(value.get("inputs", [])),
                    outputs=list(value.get("outputs", [])),
                    timeout=value.get("timeout"),
                    priority=value.get("priority", 0),
                )
                provenance = ActionProvenance.from_action(
                    action,
                    include_content=str(value.get("include_content", "")).lower() == "true",
                )
                return [
                    AnalysisIssue(
                        rule_id="buildsys.provenance",
                        message=f"provenance fingerprint: {provenance.fingerprint()}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                        extra={"provenance": json.dumps(provenance.to_dict(), sort_keys=True)},
                    )
                ]

            from domainlet.buildsys.resources import ResourceBudget, ResourceRequest, schedule

            value = _json_data(data)
            if not isinstance(value, Mapping):
                raise ValueError("schedule input must contain a graph mapping")
            raw_graph = value.get("graph", value)
            raw_requests = value.get("requests", {})
            requests = {
                name: ResourceRequest(**request) if isinstance(request, Mapping) else request
                for name, request in raw_requests.items()
            }
            raw_budget = value.get("budget", {})
            budget = ResourceBudget(**raw_budget) if isinstance(raw_budget, Mapping) else None
            waves = schedule(raw_graph, requests, budget)
            return [
                AnalysisIssue(
                    rule_id="buildsys.schedule",
                    message=f"planned {len(waves)} execution wave(s)",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                    extra={"waves": json.dumps(waves)},
                )
            ]
        except BindletError:
            raise
        except Exception as exc:
            raise BindletError(
                BindletErrorCode.EXECUTION_FAILED,
                f"buildsys execution failed: {exc}",
                "BuildSysBindlet",
                detail=repr(exc),
            ) from exc


__all__ = ["ADVERTISEMENT", "BuildSysBindlet", "get_advertisement"]
