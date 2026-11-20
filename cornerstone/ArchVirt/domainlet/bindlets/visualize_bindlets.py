"""
visualize_bindlets — Bindlet wrappers for the visualize.* domainlets.

Exposes terminal rendering (Unicode/ASCII box drawing, trees,
adjacency matrices) and structured format export (DOT, Mermaid,
GraphML, GraphSON, JSON, S-expression) for every substrate graph
type through the unified bindlet interface.
"""

from __future__ import annotations

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
from domainlet.bindlets.graph_bindlets import _parse_context

_GRAPH_KINDS = ("cfg", "callgraph", "dependency", "pdg", "sdg", "cpg")


def _build_graph(kind: str, data: Any, function: str | None) -> Any:
    """Build one graph of the requested kind from a parsed dump."""
    from domainlet.graphs.callgraph import build_callgraph
    from domainlet.graphs.cfg import build_all_cfgs
    from domainlet.graphs.codeprop import build_cpg_for_function
    from domainlet.graphs.dependency import DependencyGraphBuilder
    from domainlet.graphs.progdep import PDG
    from domainlet.graphs.sysdep import build_sdg

    configuration = data.configurations[0]

    def pick_cfg():
        cfgs = build_all_cfgs(configuration)
        if not cfgs:
            raise ValueError("no functions found in translation unit")
        if function is None:
            return next(iter(cfgs.values()))
        for fn, cfg in cfgs.items():
            if getattr(fn, "name", str(fn)) == function:
                return cfg
        raise ValueError(f"function {function!r} not found")

    if kind == "cfg":
        return pick_cfg()
    if kind == "callgraph":
        return build_callgraph(configuration)
    if kind == "dependency":
        return DependencyGraphBuilder().build(pick_cfg())
    if kind == "pdg":
        return PDG(pick_cfg())
    if kind == "sdg":
        return build_sdg(configuration)
    if kind == "cpg":
        return build_cpg_for_function(pick_cfg(), data, config=None)
    raise ValueError(f"unknown graph kind {kind!r}; expected one of "
                     f"{', '.join(_GRAPH_KINDS)}")


# ══════════════════════════════════════════════════════════════════════
#  Terminal Render Bindlet — ASCII/Unicode views of any graph
# ══════════════════════════════════════════════════════════════════════

class VisualizeAsciiBindlet(AbstractDomainlet):
    """Bindlet for rendering graphs as terminal text."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="visualize.ascii",
            version="0.1.0",
            description="Terminal rendering — Unicode/ASCII box drawing, "
                        "trees and adjacency matrices for all graph types",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="render",
                    description="Render a graph as a layered box drawing "
                                "(styles: unicode, ascii, tree, matrix)",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues"],
                    tags=["visualize", "terminal", "ascii", "render"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        from domainlet.bindlet_base import BindletError, BindletErrorCode

        self._capability = config.get("capability", "render")
        self._kind = config.get("graph", "cpg")
        if self._kind not in _GRAPH_KINDS:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown graph kind {self._kind!r}; expected one "
                f"of {', '.join(_GRAPH_KINDS)}",
                source="VisualizeAsciiBindlet",
            )
        self._style = config.get("style", "unicode")
        self._function: str | None = config.get("function") or None
        try:
            self._max_nodes = int(config.get("max_nodes", "80"))
        except ValueError as exc:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"max_nodes must be an integer: {exc}",
                source="VisualizeAsciiBindlet",
            ) from exc

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.visualize import render

        data = _parse_context(context)
        graph = _build_graph(self._kind, data, self._function)
        rendered = render(graph, style=self._style,
                          max_nodes=self._max_nodes)
        return [
            AnalysisIssue(
                rule_id="visualize.rendered",
                message=rendered,
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
                extra={"graph": self._kind, "style": self._style},
            )
        ]


# ══════════════════════════════════════════════════════════════════════
#  Export Bindlet — structured formats to text or file
# ══════════════════════════════════════════════════════════════════════

class VisualizeExportBindlet(AbstractDomainlet):
    """Bindlet for exporting graphs to structured text formats."""

    def probe(self) -> DomainletInfo:
        from domainlet.visualize import FORMATS
        return DomainletInfo(
            name="visualize.export",
            version="0.1.0",
            description="Format export — " + ", ".join(FORMATS) +
                        " for all graph types (native exporters where "
                        "available)",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="export",
                    description="Export one graph to one format "
                                "(configure: format, output)",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "file", "text"],
                    tags=["visualize", "export", "dot", "mermaid"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        from domainlet.visualize import FORMATS
        from domainlet.bindlet_base import BindletError, BindletErrorCode

        self._capability = config.get("capability", "export")
        self._kind = config.get("graph", "cpg")
        if self._kind not in _GRAPH_KINDS:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown graph kind {self._kind!r}; expected one "
                f"of {', '.join(_GRAPH_KINDS)}",
                source="VisualizeExportBindlet",
            )
        self._format = config.get("format", "dot").lower()
        if self._format not in FORMATS:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown format {self._format!r}; expected one "
                f"of {', '.join(FORMATS)}",
                source="VisualizeExportBindlet",
            )
        self._output = config.get("output", "")
        self._function: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.visualize import export, export_to

        data = _parse_context(context)
        graph = _build_graph(self._kind, data, self._function)
        text = export(graph, self._format)

        if self._output:
            path = export_to(graph, self._format, self._output)
            message = f"exported {self._kind} graph to {path}"
        else:
            message = text

        return [
            AnalysisIssue(
                rule_id="visualize.exported",
                message=message,
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
                extra={"graph": self._kind, "format": self._format},
            )
        ]
