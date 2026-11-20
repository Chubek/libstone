"""
query_bindlets — Bindlet wrappers for query.* domainlets.

Exposes query-oriented wrappers around CFG construction, callgraph,
code-property graphs, and program/system dependency graphs through the
unified bindlet interface.
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


# QueryCallGraph Bindlet

class QueryCallGraphBindlet(AbstractDomainlet):
    """Bindlet for querying the interprocedural call graph."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.callgraph",
            version="0.1.0",
            description="Query Call Graph — callers, callees, reachability, recursion, SCC analysis",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-callers",
                    description="Find all callers of a given function",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "callgraph", "callers"],
                ),
                DomainletCapability(
                    name="query-callees",
                    description="Find all callees of a given function",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "callgraph", "callees"],
                ),
                DomainletCapability(
                    name="query-cycles",
                    description="Detect recursive cycles in the call graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "callgraph", "recursion"],
                ),
                DomainletCapability(
                    name="query-unreachable",
                    description="Find functions unreachable from the entry point",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "callgraph", "dead-code"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive summary of the call graph",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "callgraph", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.callgraph import build_callgraph
        from domainlet.query.query_call_graph import QueryCallGraph

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                cg = build_callgraph(configuration)
                qcg = QueryCallGraph(cg)

                if self._capability == "query-callers":
                    if self._function_name:
                        callers = qcg.callers_of(self._function_name)
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.callers",
                                message=f"'{self._function_name}' is called by: "
                                        f"{', '.join(c.name for c in callers) or 'nobody'}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.callers",
                                message="No function name specified for caller query",
                                severity=Severity.WARNING,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-callees":
                    if self._function_name:
                        callees = qcg.callees_of(self._function_name)
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.callees",
                                message=f"'{self._function_name}' calls: "
                                        f"{', '.join(c.name for c in callees) or 'nobody'}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.callees",
                                message="No function name specified for callee query",
                                severity=Severity.WARNING,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-cycles":
                    cycles = qcg.find_cycles()
                    if cycles:
                        for cycle in cycles:
                            names = [n.name for n in cycle]
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.callgraph.cycles",
                                    message=f"Recursive cycle: {', '.join(names)}",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.HIGH,
                                )
                            )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.cycles",
                                message="No recursive cycles found",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-unreachable":
                    unreachable = qcg.unreachable_functions()
                    if unreachable:
                        for fn in unreachable:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.callgraph.unreachable",
                                    message=f"Unreachable function: {fn.name}",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.MEDIUM,
                                )
                            )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.callgraph.unreachable",
                                message="All functions are reachable from entry",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-summary":
                    stats = qcg.statistics()
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.callgraph.summary",
                            message=(
                                f"Call Graph: {stats.get('functions', 0)} functions, "
                                f"{stats.get('total_nodes', 0)} nodes, "
                                f"{stats.get('total_edges', 0)} edges, "
                                f"{stats.get('sccs', 0)} SCCs, "
                                f"{stats.get('recursive_sccs', 0)} recursive"
                            ),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.callgraph.summary.dot",
                            message=qcg.to_dot(),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.callgraph.error",
                        message=f"Call graph query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# QueryCFG Bindlet

class QueryCFGBindlet(AbstractDomainlet):
    """Bindlet for querying intraprocedural control-flow graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.cfg",
            version="0.1.0",
            description="Query CFG — dominance, reachability, loop detection, path enumeration",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-dominators",
                    description="Compute dominator tree for each function CFG",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cfg", "dominators"],
                ),
                DomainletCapability(
                    name="query-back-edges",
                    description="Find back-edges identifying natural loops",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cfg", "loops"],
                ),
                DomainletCapability(
                    name="query-reachability",
                    description="Check reachability between basic blocks",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cfg", "reachability"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive CFG summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cfg", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.query.query_control_flow_graph import QueryCFG

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                func_cfgs = build_all_cfgs(configuration)
                for func_name, cfg in func_cfgs.items():
                    if self._function_name is not None and func_name != self._function_name:
                        continue
                    qcfg = QueryCFG(cfg)

                    if self._capability == "query-dominators":
                        doms = qcfg.dominators()
                        for node, dominated in doms.items():
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cfg.dominators",
                                    message=f"[{func_name}] Node BB{node.id} dominates {len(dominated)} node(s)",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                    elif self._capability == "query-back-edges":
                        back_edges = qcfg.find_back_edges()
                        if back_edges:
                            for src, tgt in back_edges:
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.cfg.back-edge",
                                        message=f"[{func_name}] Back-edge: BB{src.id} -> BB{tgt.id}",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                    )
                                )
                        else:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cfg.back-edge",
                                    message=f"[{func_name}] No back-edges (acyclic CFG)",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                    elif self._capability == "query-reachability":
                        entry = qcfg.entry_node
                        exit_node = qcfg.exit_node
                        if entry and exit_node:
                            reaches = qcfg.reaches(entry, exit_node)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cfg.reachability",
                                    message=f"[{func_name}] Entry {'reaches' if reaches else 'does NOT reach'} exit",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                    elif self._capability == "query-summary":
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.cfg.summary",
                                message=f"[{func_name}] CFG: {qcfg.node_count} nodes, {qcfg.edge_count} edges",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                        back_edges = qcfg.find_back_edges()
                        loops = qcfg.natural_loops()
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.cfg.loops",
                                message=f"[{func_name}] {len(back_edges)} back-edges, {len(loops)} natural loops",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.cfg.summary.dot",
                                message=qcfg.to_dot(),
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.cfg.error",
                        message=f"CFG query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# QueryCPG Bindlet

class QueryCPGBindlet(AbstractDomainlet):
    """Bindlet for querying the Code Property Graph."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.cpg",
            version="0.1.0",
            description="Query CPG — typed node queries, traversal, slicing, taint, CPGQL",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-nodes",
                    description="Query nodes by type (calls, identifiers, literals, …)",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cpg", "nodes"],
                ),
                DomainletCapability(
                    name="query-slice",
                    description="Backward/forward program slicing",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cpg", "slicing"],
                ),
                DomainletCapability(
                    name="query-taint",
                    description="Taint analysis — mark and propagate taint",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cpg", "taint"],
                ),
                DomainletCapability(
                    name="query-cpgql",
                    description="Execute CPGQL queries",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cpg", "cpgql"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive CPG summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "cpg", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._cpgql_query: str | None = config.get("cpgql") or None
        self._node_type: str | None = config.get("node_type") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.codeprop import build_cpg_for_translation_unit
        from domainlet.query.query_code_property_graph import QueryCPG

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                cpg = build_cpg_for_translation_unit(configuration)
                qcpg = QueryCPG(cpg)

                if self._capability == "query-nodes":
                    node_type_map = {
                        "call": qcpg.call_nodes,
                        "identifier": qcpg.identifier_nodes,
                        "literal": qcpg.literal_nodes,
                        "method": qcpg.method_nodes,
                        "return": qcpg.return_nodes,
                        "block": qcpg.block_nodes,
                        "local": qcpg.local_nodes,
                        "parameter": qcpg.parameter_nodes,
                        "control_structure": qcpg.control_structure_nodes,
                    }
                    if self._node_type and self._node_type in node_type_map:
                        nodes = node_type_map[self._node_type]()
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.cpg.nodes",
                                message=f"Found {len(nodes)} {self._node_type} node(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                        for node in nodes[:20]:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cpg.node",
                                    message=f"[{self._node_type}] {node}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )
                    else:
                        for nt, getter in node_type_map.items():
                            count = len(getter())
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cpg.node-counts",
                                    message=f"{nt}: {count}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                elif self._capability == "query-slice":
                    if qcpg.nodes:
                        last_id = max(qcpg.nodes.keys())
                        last_node = qcpg.node_by_id(last_id)
                        if last_node:
                            bw = qcpg.backward_slice(last_node)
                            fw = qcpg.forward_slice(last_node)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cpg.slice",
                                    message=f"Backward slice: {len(bw)} nodes, Forward slice: {len(fw)} nodes",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                elif self._capability == "query-taint":
                    tainted_count = 0
                    for node in qcpg.call_nodes():
                        qcpg.mark_tainted(node, "call-site")
                        tainted_count += 1
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.cpg.taint",
                            message=f"Marked {tainted_count} call-site(s) as tainted",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "query-cpgql":
                    if self._cpgql_query:
                        try:
                            result = qcpg.cpgql(self._cpgql_query)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cpg.cpgql",
                                    message=f"CPGQL query result: {result}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )
                        except Exception as e:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.cpg.cpgql.error",
                                    message=f"CPGQL query failed: {e}",
                                    severity=Severity.ERROR,
                                    confidence=Confidence.LOW,
                                )
                            )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.cpg.cpgql",
                                message="No CPGQL query string provided",
                                severity=Severity.WARNING,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-summary":
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.cpg.summary",
                            message=f"CPG: {qcpg.node_count} nodes, {qcpg.edge_count} edges",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.cpg.summary.dot",
                            message=qcpg.to_dot(),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.cpg.error",
                        message=f"CPG query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# QueryDependencyGraph Bindlet

class QueryDependencyGraphBindlet(AbstractDomainlet):
    """Bindlet for querying dependency graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.dependency",
            version="0.1.0",
            description="Query Dependency Graph — slice, dead stores, def-use chains, reachability",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-slice",
                    description="Backward/forward program slicing",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "dependency", "slicing"],
                ),
                DomainletCapability(
                    name="query-dead-stores",
                    description="Find dead stores (definitions never read)",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "dependency", "dead-code"],
                ),
                DomainletCapability(
                    name="query-def-use",
                    description="Compute def-use and use-def chains",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "dependency", "def-use"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive dependency graph summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "dependency", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._line: int | None = config.get("line")
        self._line = int(self._line) if self._line else None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.dependency import build_from_dump
        from domainlet.query.query_dependency_graph import QueryDependencyGraph

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                from domainlet.graphs.cfg import build_all_cfgs
                func_cfgs = build_all_cfgs(configuration)
                for func_name, cfg in func_cfgs.items():
                    try:
                        dg = build_from_dump(cfg)
                        qdg = QueryDependencyGraph(dg)

                        if self._capability == "query-slice":
                            if self._line:
                                bw = qdg.backward_slice(self._line)
                                fw = qdg.forward_slice(self._line)
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.dep.slice",
                                        message=f"[{func_name}] Backward slice at line {self._line}: "
                                                f"{len(bw)} nodes, Forward: {len(fw)} nodes",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                    )
                                )
                            else:
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.dep.slice",
                                        message=f"[{func_name}] No line specified for slicing",
                                        severity=Severity.WARNING,
                                        confidence=Confidence.HIGH,
                                    )
                                )

                        elif self._capability == "query-dead-stores":
                            dead = qdg.dead_stores()
                            if dead:
                                for node in dead:
                                    issues.append(
                                        AnalysisIssue(
                                            rule_id="query.dep.dead-store",
                                            message=f"[{func_name}] Dead store: {node}",
                                            severity=Severity.WARNING,
                                            confidence=Confidence.MEDIUM,
                                        )
                                    )
                            else:
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.dep.dead-store",
                                        message=f"[{func_name}] No dead stores found",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                    )
                                )

                        elif self._capability == "query-def-use":
                            duc = qdg.def_use_chains()
                            udc = qdg.use_def_chains()
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.dep.def-use",
                                    message=f"[{func_name}] {len(duc)} def-use chains, "
                                            f"{len(udc)} use-def chains",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                        elif self._capability == "query-summary":
                            summary = qdg.summary()
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.dep.summary",
                                    message=f"[{func_name}] DepGraph: {qdg.node_count} nodes, "
                                            f"{qdg.edge_count} edges — "
                                            f"{json.dumps(summary)}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.dep.summary.dot",
                                    message=qdg.to_dot(),
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                    except Exception as fexc:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.dep.func-error",
                                message=f"[{func_name}] Dependency graph query failed: {fexc}",
                                severity=Severity.WARNING,
                                confidence=Confidence.LOW,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.dep.error",
                        message=f"Dependency graph query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# QueryPDG Bindlet

class QueryPDGBindlet(AbstractDomainlet):
    """Bindlet for querying program dependency graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.pdg",
            version="0.1.0",
            description="Query PDG — slice, reachability, data/control dependence queries",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-slice",
                    description="Backward/forward program slicing at statement level",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "pdg", "slicing"],
                ),
                DomainletCapability(
                    name="query-reachability",
                    description="Forward/backward reachability in the PDG",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "pdg", "reachability"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive PDG summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "pdg", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._line: int | None = config.get("line")
        self._line = int(self._line) if self._line else None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.progdep import PDG
        from domainlet.query.query_program_dependency_graph import QueryPDG

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                from domainlet.graphs.cfg import build_all_cfgs
                func_cfgs = build_all_cfgs(configuration)
                for func_name, cfg in func_cfgs.items():
                    try:
                        pdg = PDG(cfg)
                        qpdg = QueryPDG(pdg)

                        if self._capability == "query-slice":
                            if self._line:
                                bw = qpdg.backward_slice(self._line)
                                fw = qpdg.forward_slice(self._line)
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.pdg.slice",
                                        message=f"[{func_name}] Backward slice at line {self._line}: "
                                                f"{len(bw)} nodes, Forward: {len(fw)} nodes",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                    )
                                )
                            else:
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.pdg.slice",
                                        message=f"[{func_name}] No line specified for slicing",
                                        severity=Severity.WARNING,
                                        confidence=Confidence.HIGH,
                                    )
                                )

                        elif self._capability == "query-reachability":
                            if qpdg.nodes:
                                first = qpdg.nodes[0]
                                last = qpdg.nodes[-1]
                                fwd = qpdg.forward_reachable(first)
                                bwd = qpdg.backward_reachable(last)
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="query.pdg.reachability",
                                        message=f"[{func_name}] Forward from entry: {len(fwd)} nodes, "
                                                f"Backward from exit: {len(bwd)} nodes",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                    )
                                )

                        elif self._capability == "query-summary":
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.pdg.summary",
                                    message=f"[{func_name}] PDG: {len(qpdg.nodes)} nodes, "
                                            f"{len(qpdg.edges)} edges "
                                            f"({len(qpdg.data_edges)} data, "
                                            f"{len(qpdg.control_edges)} control)",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.pdg.summary.dot",
                                    message=qpdg.to_dot(),
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )

                    except Exception as fexc:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.pdg.func-error",
                                message=f"[{func_name}] PDG query failed: {fexc}",
                                severity=Severity.WARNING,
                                confidence=Confidence.LOW,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.pdg.error",
                        message=f"PDG query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# QuerySDG Bindlet

class QuerySDGBindlet(AbstractDomainlet):
    """Bindlet for querying system dependency graphs."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="query.sdg",
            version="0.1.0",
            description="Query SDG — interprocedural slicing, procedure-level queries, reachability",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="query-slice",
                    description="Interprocedural backward/forward slicing",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "sdg", "interprocedural-slicing"],
                ),
                DomainletCapability(
                    name="query-procedures",
                    description="List all procedures and their entry/exit nodes",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "sdg", "procedures"],
                ),
                DomainletCapability(
                    name="query-reachability",
                    description="Forward/backward reachability in the SDG",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "sdg", "reachability"],
                ),
                DomainletCapability(
                    name="query-summary",
                    description="Return a comprehensive SDG summary",
                    input_kind=InputKind.AST_JSON,
                    tags=["query", "sdg", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "query-summary")
        self._procedure: str | None = config.get("procedure") or None
        self._line: int | None = config.get("line")
        self._line = int(self._line) if self._line else None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.sysdep import build_sdg
        from domainlet.query.query_system_dependency_graph import QuerySDG

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                sdg = build_sdg(configuration)
                qsdg = QuerySDG(sdg)

                if self._capability == "query-slice":
                    if self._procedure and self._line:
                        bw = qsdg.interprocedural_backward_slice(
                            self._procedure, self._line)
                        fw = qsdg.interprocedural_forward_slice(
                            self._procedure, self._line)
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.sdg.slice",
                                message=f"Interprocedural backward slice from "
                                        f"{self._procedure}:{self._line}: "
                                        f"{len(bw)} nodes, "
                                        f"Forward: {len(fw)} nodes",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.sdg.slice",
                                message="Procedure name and line number required for slicing",
                                severity=Severity.WARNING,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-procedures":
                    procedures = qsdg.procedures()
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.sdg.procedures",
                            message=f"Found {len(procedures)} procedure(s): "
                                    f"{', '.join(procedures)}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    for proc in procedures:
                        entry = qsdg.procedure_entry(proc)
                        exit_node = qsdg.procedure_exit(proc)
                        node_count = len(qsdg.procedure_nodes(proc))
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.sdg.procedure",
                                message=f"Procedure '{proc}': {node_count} nodes, "
                                        f"entry={entry.id if entry else '?'}, "
                                        f"exit={exit_node.id if exit_node else '?'}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-reachability":
                    if self._procedure:
                        entry = qsdg.procedure_entry(self._procedure)
                        if entry:
                            fwd = qsdg.reachable_forward(entry)
                            bwd = qsdg.reachable_backward(entry)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.sdg.reachability",
                                    message=f"From '{self._procedure}' entry: "
                                            f"{len(fwd)} forward reachable, "
                                            f"{len(bwd)} backward reachable",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.HIGH,
                                )
                            )
                        else:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="query.sdg.reachability",
                                    message=f"Procedure '{self._procedure}' not found",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.HIGH,
                                )
                            )
                    else:
                        issues.append(
                            AnalysisIssue(
                                rule_id="query.sdg.reachability",
                                message="Procedure name required for reachability query",
                                severity=Severity.WARNING,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "query-summary":
                    procedures = qsdg.procedures()
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.sdg.summary",
                            message=f"SDG: {qsdg.node_count} nodes, {qsdg.edge_count} edges, "
                                    f"{len(procedures)} procedures",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    data_count = len(qsdg.data_edges())
                    ctrl_count = len(qsdg.control_edges())
                    call_count = len(qsdg.call_edges())
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.sdg.edge-types",
                            message=f"Edge types: {data_count} data, {ctrl_count} control, "
                                    f"{call_count} call",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    issues.append(
                        AnalysisIssue(
                            rule_id="query.sdg.summary.dot",
                            message=qsdg.to_dot(),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="query.sdg.error",
                        message=f"SDG query failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues
