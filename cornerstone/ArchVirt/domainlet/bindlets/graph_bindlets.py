"""
graph_bindlets — Bindlet wrappers for graphs.* domainlets.

Exposes CFG construction, callgraph, code-property graphs, and
program/system dependency graphs through the unified interface.
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
    SourceSpan,
)


def _parse_context(context: AnalysisContext) -> Any:
    import os
    import tempfile

    from domainlet.domgate_feed import parsedump

    if context.source_file and os.path.isfile(context.source_file):
        return parsedump(context.source_file)

    suffixes = {
        "c": ".c",
        "c++": ".cpp",
        "cpp": ".cpp",
        "python": ".py",
        "ruby": ".rb",
        "rust": ".rs",
        "go": ".go",
    }
    with tempfile.NamedTemporaryFile(
        mode="w",
        suffix=suffixes.get(context.language.lower(), ".c"),
        delete=False,
    ) as f:
        f.write(context.data)
        source_path = f.name

    try:
        return parsedump(source_path)
    finally:
        os.unlink(source_path)


# ══════════════════════════════════════════════════════════════════════
#  CFG Bindlet — build and analyse control-flow graphs
# ══════════════════════════════════════════════════════════════════════

class CFGBindlet(AbstractDomainlet):
    """Bindlet for building and querying intraprocedural control-flow graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.cfg",
            version="0.6.0",
            description="Control Flow Graph — build CFGs, compute dominators, enumerate back-edges",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-cfg",
                    description="Build CFGs for every function in a translation unit",
                    input_kind=InputKind.AST_JSON,
                    tags=["cfg", "graph", "control-flow"],
                ),
                DomainletCapability(
                    name="dominators",
                    description="Compute dominator tree for each function CFG",
                    input_kind=InputKind.AST_JSON,
                    tags=["cfg", "dominators", "ssa"],
                ),
                DomainletCapability(
                    name="back-edges",
                    description="Find back-edges identifying natural loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["cfg", "loops", "back-edge"],
                ),
                DomainletCapability(
                    name="analyze-cfg",
                    description="Run all CFG analyses and return a summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["cfg", "comprehensive", "metrics"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "analyze-cfg")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs, CFGNode

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name is not None and func_name != self._function_name:
                    continue

                if self._capability == "build-cfg":
                    issues.append(
                        AnalysisIssue(
                            rule_id="cfg.built",
                            message=f"CFG for '{func_name}': {len(cfg.nodes)} nodes, "
                                    f"{len(cfg.edges)} edges",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "dominators":
                    doms = cfg.dominators()
                    for node, dominated in doms.items():
                        issues.append(
                            AnalysisIssue(
                                rule_id="cfg.dominators",
                                message=f"Node BB{node.id} dominates {len(dominated)} node(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "back-edges":
                    back_edges = cfg.back_edges()
                    for edge in back_edges:
                        issues.append(
                            AnalysisIssue(
                                rule_id="cfg.backedge",
                                message=f"Back-edge: BB{edge.source.id} -> BB{edge.target.id}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "analyze-cfg":
                    n_nodes = len(cfg.nodes)
                    n_edges = len(cfg.edges)
                    n_back_edges = len(cfg.back_edges())
                    doms = cfg.dominators()

                    issues.append(
                        AnalysisIssue(
                            rule_id="cfg.summary",
                            message=f"CFG '{func_name}' summary: {n_nodes} nodes, {n_edges} edges, "
                                    f"{n_back_edges} back-edges, {len(doms)} dominator entries",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  CallGraph Bindlet — build and query call graphs
# ══════════════════════════════════════════════════════════════════════

class CallGraphBindlet(AbstractDomainlet):
    """Bindlet for building and analysing function call graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.callgraph",
            version="0.6.0",
            description="Call Graph — build, compute SCCs, query caller/callee relationships",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-callgraph",
                    description="Build a call graph from a translation unit",
                    input_kind=InputKind.AST_JSON,
                    tags=["callgraph", "interprocedural", "scc"],
                ),
                DomainletCapability(
                    name="find-cycles",
                    description="Find recursive cycles (SCCs) in the call graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["callgraph", "recursion", "tarjan"],
                ),
                DomainletCapability(
                    name="reachability",
                    description="Compute reachability in the call graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["callgraph", "reachability", "dead-code"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build-callgraph")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.callgraph import CallGraph, tarjan_scc

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                cg = CallGraph(configuration)

                if self._capability == "build-callgraph":
                    issues.append(
                        AnalysisIssue(
                            rule_id="callgraph.built",
                            message=f"CallGraph: {len(cg.nodes)} nodes, {len(cg.edges)} edges",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "find-cycles":
                    sccs = tarjan_scc(cg)
                    recursive_sccs = [s for s in sccs if len(s) > 1]
                    for scc in recursive_sccs:
                        func_names = [str(n) for n in scc]
                        issues.append(
                            AnalysisIssue(
                                rule_id="callgraph.recursion",
                                message=f"Recursive cycle: {' → '.join(func_names)}",
                                severity=Severity.WARNING,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                    if not recursive_sccs:
                        issues.append(
                            AnalysisIssue(
                                rule_id="callgraph.no-recursion",
                                message="No recursive cycles detected",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "reachability":
                    root_nodes = getattr(cg, "roots", []) or list(cg.nodes)[:1]
                    for root in root_nodes:
                        reachable = cg.reachable_from(root) if hasattr(
                            cg, "reachable_from") else set()
                        issues.append(
                            AnalysisIssue(
                                rule_id="callgraph.reachable",
                                message=f"From '{root}': {len(reachable)} reachable callee(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="callgraph.error",
                        message=f"CallGraph analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Code-Property Graph Bindlet
# ══════════════════════════════════════════════════════════════════════

class CodePropBindlet(AbstractDomainlet):
    """Bindlet for building Code Property Graphs (AST + CFG + PDG merged)."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.codeprop",
            version="0.6.0",
            description="Code Property Graph — merge AST, CFG, and PDG into a unified representation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-cpg",
                    description="Build a full Code Property Graph from source",
                    input_kind=InputKind.AST_JSON,
                    tags=["cpg", "unified", "multi-graph"],
                ),
                DomainletCapability(
                    name="query-cpg",
                    description="Run pattern queries on a CPG",
                    input_kind=InputKind.AST_JSON,
                    tags=["cpg", "query", "traversal"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build-cpg")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.codeprop import CPGBuilder

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                builder = CPGBuilder(configuration)
                cpg = builder.build()

                issues.append(
                    AnalysisIssue(
                        rule_id="cpg.built",
                        message=f"CPG: {len(cpg.nodes)} nodes, {len(cpg.edges)} edges",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

                if self._capability == "query-cpg":
                    ast_count = sum(1 for n in cpg.nodes if hasattr(
                        n, "kind") and "ast" in str(n.kind).lower())
                    cfg_count = sum(1 for n in cpg.nodes if hasattr(
                        n, "kind") and "cfg" in str(n.kind).lower())
                    pdg_count = sum(1 for n in cpg.nodes if hasattr(
                        n, "kind") and "pdg" in str(n.kind).lower())

                    issues.append(
                        AnalysisIssue(
                            rule_id="cpg.layers",
                            message=f"CPG layers: {ast_count} AST, {cfg_count} CFG, {pdg_count} PDG nodes",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="cpg.error",
                        message=f"CPG construction failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Dependency Graph Bindlet
# ══════════════════════════════════════════════════════════════════════

class DependencyBindlet(AbstractDomainlet):
    """Bindlet for data/control dependency graph construction and slicing."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.dependency",
            version="0.6.0",
            description="Dependency Graph — data/control dependencies, backward slicing",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-depgraph",
                    description="Build a dependency graph from source",
                    input_kind=InputKind.AST_JSON,
                    tags=["dependency", "slicing", "dataflow"],
                ),
                DomainletCapability(
                    name="slice-backward",
                    description="Compute a backward slice from a program point",
                    input_kind=InputKind.AST_JSON,
                    tags=["slicing", "backward", "dependency"],
                ),
                DomainletCapability(
                    name="slice-forward",
                    description="Compute a forward slice from a program point",
                    input_kind=InputKind.AST_JSON,
                    tags=["slicing", "forward", "impact"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "build-depgraph")
        self._slicing_criterion = config.get("criterion", "")
        self._slicing_line = int(config.get("line", "0"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.dependency import DependencyGraphBuilder, slice_backward, DepKind

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                builder = DependencyGraphBuilder(configuration)
                dep_graph = builder.build()

                if self._capability == "build-depgraph":
                    data_edges = sum(1 for e in dep_graph.edges if hasattr(
                        e, "kind") and e.kind == DepKind.DATA)
                    ctrl_edges = sum(1 for e in dep_graph.edges if hasattr(
                        e, "kind") and e.kind == DepKind.CONTROL)
                    issues.append(
                        AnalysisIssue(
                            rule_id="depgraph.built",
                            message=f"Dependency graph: {len(dep_graph.nodes)} nodes, "
                                    f"{len(dep_graph.edges)} edges ({data_edges} data, {ctrl_edges} control)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "slice-backward":
                    if self._slicing_criterion:
                        criterion_node = None
                        for node in dep_graph.nodes:
                            if hasattr(node, "name") and node.name == self._slicing_criterion:
                                criterion_node = node
                                break
                        if criterion_node:
                            sliced = slice_backward(dep_graph, criterion_node)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="depgraph.slice",
                                    message=f"Backward slice from '{self._slicing_criterion}': "
                                            f"{len(sliced)} node(s) in slice",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.MEDIUM,
                                )
                            )
                        else:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="depgraph.slice-missing",
                                    message=f"Slice criterion '{self._slicing_criterion}' not found",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.LOW,
                                )
                            )

                elif self._capability == "slice-forward":
                    issues.append(
                        AnalysisIssue(
                            rule_id="depgraph.forward",
                            message="Forward slicing is available through the dependency graph API",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="depgraph.error",
                        message=f"Dependency graph analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Program Dependency Graph Bindlet
# ══════════════════════════════════════════════════════════════════════

class ProgDepBindlet(AbstractDomainlet):
    """Bindlet for program dependency graphs (PDG)."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.progdep",
            version="0.6.0",
            description="Program Dependency Graph — control + data dependencies at the statement level",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-pdg",
                    description="Build a Program Dependency Graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["pdg", "dependency", "slicing"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.progdep import ProgramDependencyGraph

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                builder = ProgramDependencyGraph()
                pdg = builder.build(configuration)

                issues.append(
                    AnalysisIssue(
                        rule_id="pdg.built",
                        message=f"Program Dependency Graph: {len(pdg.nodes)} nodes, "
                                f"{len(pdg.edges)} edges",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )
            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="pdg.error",
                        message=f"PDG construction failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  System Dependency Graph Bindlet
# ══════════════════════════════════════════════════════════════════════

class SysDepBindlet(AbstractDomainlet):
    """Bindlet for system dependency graphs (SDG) — interprocedural PDGs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="graphs.sysdep",
            version="0.6.0",
            description="System Dependency Graph — interprocedural PDG connecting all functions",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-sdg",
                    description="Build a System Dependency Graph across the whole program",
                    input_kind=InputKind.AST_JSON,
                    tags=["sdg", "interprocedural", "slicing"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.sysdep import SystemDependencyGraph

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                sdg = SystemDependencyGraph()
                graph = sdg.build(configuration)

                issues.append(
                    AnalysisIssue(
                        rule_id="sdg.built",
                        message=f"System Dependency Graph: {len(graph.nodes)} nodes, "
                                f"{len(graph.edges)} edges",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )
            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="sdg.error",
                        message=f"SDG construction failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues
