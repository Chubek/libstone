"""DomMune-facing bindlet: Lua frontend gate via fekern.

Exposes the explicitly selected ``Lua`` gatebindle
(``fekern.gate.GateRegistry.from_gatebindle("Lua")``) through the
standard bindlet lifecycle so the C++ DomMune frontend can parse Lua
source without embedding gatebindle knowledge itself.
"""

from __future__ import annotations

import json
from typing import ClassVar

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


def _node_to_dict(node):
    return {
        "kind": getattr(node, "kind", ""),
        "text": getattr(node, "text", ""),
        "error": bool(getattr(node, "error", False)),
        "fields": {
            str(key): _field_to_jsonable(value)
            for key, value in getattr(node, "fields", {}).items()
        },
        "children": [_node_to_dict(child)
                     for child in getattr(node, "children", [])],
    }


def _field_to_jsonable(value):
    if hasattr(value, "kind") and hasattr(value, "children"):
        return _node_to_dict(value)
    if isinstance(value, (list, tuple)):
        return [_field_to_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _field_to_jsonable(item)
                for key, item in value.items()}
    return value


class DomMuneFrontendBindlet(AbstractDomainlet):
    """Parse Lua source through fekern's explicit Lua gate."""

    _CAPABILITIES: ClassVar[set] = {"parse"}

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="dommune.frontend",
            version="0.1.0",
            description="DomMune Lua parser via the fekern Lua gate",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    "parse", "Parse Lua source via fekern Lua gate",
                    InputKind.SOURCE_TEXT),
            ],
            config_schema={
                "capability": {"type": "string", "enum": sorted(self._CAPABILITIES)},
                "gate": {"type": "string", "default": "Lua"},
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        capability = config.get("capability", "parse")
        if capability not in self._CAPABILITIES:
            raise BindletError(BindletErrorCode.CONFIG_INVALID,
                               f"unknown capability: {capability}",
                               "DomMuneFrontendBindlet")
        self._capability = capability
        self._gate = (config.get("gate", "Lua") or "Lua").strip() or "Lua"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.fekern.gate import GateRegistry

        gate = (context.options.get("gate", "") or "").strip() or getattr(
            self, "_gate", "Lua")
        source = context.data if isinstance(context.data, str) else ""
        filename = context.source_file or "<memory>"
        definition = None
        last_error: Exception | None = None
        for spelling in (gate, gate.lower(), gate.capitalize()):
            try:
                definition = GateRegistry.from_gatebindle(spelling).select(spelling)
                break
            except Exception as exc:
                last_error = exc
        if definition is None:
            raise BindletError(BindletErrorCode.EXECUTION_FAILED,
                               f"fekern Lua gate {gate!r} unavailable: {last_error}",
                               "DomMuneFrontendBindlet")
        try:
            result = definition.parse(source, filename=filename)
        except Exception as exc:
            raise BindletError(BindletErrorCode.EXECUTION_FAILED,
                               f"Lua-gate parse failed: {exc}",
                               "DomMuneFrontendBindlet") from exc

        diagnostics = []
        for item in result.diagnostics:
            severity = str(getattr(getattr(item, "severity", "error"), "value",
                                   getattr(item, "severity", "error")))
            diagnostics.append({
                "severity": severity,
                "message": str(getattr(item, "message", str(item))),
            })
        payload = {
            "root": _node_to_dict(result.root),
            "diagnostics": diagnostics,
            "metadata": {
                "language": getattr(result.metadata, "language", "lua"),
                "version": getattr(result.metadata, "version", "5.4"),
            },
        }
        errors = sum(1 for d in diagnostics if d["severity"] == "error")
        warnings = sum(1 for d in diagnostics if d["severity"] == "warning")
        issues = [AnalysisIssue(
            "dommune.frontend.parsed",
            f"parsed {filename} via fekern gate {gate!r}",
            Severity.INFORMATION,
            Confidence.HIGH,
            extra={
                "luatree_json": json.dumps(payload, separators=(",", ":")),
                "error_count": str(errors),
                "warning_count": str(warnings),
            },
        )]
        for diag in diagnostics:
            issues.append(AnalysisIssue(
                "dommune.frontend.diagnostic",
                diag["message"],
                Severity.ERROR if diag["severity"] == "error" else Severity.WARNING,
                Confidence.HIGH,
            ))
        return issues
