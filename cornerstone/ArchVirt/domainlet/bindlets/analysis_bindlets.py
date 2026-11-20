"""
analysis_bindlets — Bindlet wrappers for analyses.* domainlets.

Exposes ctrlflow, dataflow, distrib, interproc, path, taint, and typing
analyses through the unified AbstractDomainlet interface.
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
    FixSuggestion,
    InputKind,
    Severity,
    SourceSpan,
)


# ══════════════════════════════════════════════════════════════════════
#  Shared helper — parse domgate dump from AnalysisContext
# ══════════════════════════════════════════════════════════════════════

def _parse_context(context: AnalysisContext) -> Any:
    """Parse AST data from an AnalysisContext into a DomgateData object."""
    import tempfile
    import os

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".dump", delete=False
    ) as f:
        f.write(context.data)
        dump_path = f.name

    try:
        from domainlet.domgate_feed import parsedump
        return parsedump(dump_path)
    finally:
        os.unlink(dump_path)


def _make_source_span(token: Any) -> SourceSpan:
    """Build a SourceSpan from a domgate-style Token object."""
    return SourceSpan(
        file=getattr(token, "file", ""),
        line_start=getattr(token, "linenr", 0),
        col_start=getattr(token, "column", 0),
    )


# ══════════════════════════════════════════════════════════════════════
#  Control-Flow Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class CtrlFlowBindlet(AbstractDomainlet):
    """Bindlet for control-flow analysis: dominators, loops, induction variables."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.ctrlflow",
            version="0.6.0",
            description="Control-flow analysis — dominator trees, loop detection, induction variables, loop bounds",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute-dominators",
                    description="Build dominator tree from a CFG",
                    input_kind=InputKind.AST_JSON,
                    tags=["control-flow", "dominators", "cfg"],
                ),
                DomainletCapability(
                    name="detect-loops",
                    description="Identify natural loops in a CFG",
                    input_kind=InputKind.AST_JSON,
                    tags=["control-flow", "loops", "cfg"],
                ),
                DomainletCapability(
                    name="induction-vars",
                    description="Detect and classify induction variables",
                    input_kind=InputKind.AST_JSON,
                    tags=["control-flow", "induction", "optimization"],
                ),
                DomainletCapability(
                    name="loop-bounds",
                    description="Estimate loop iteration bounds",
                    input_kind=InputKind.AST_JSON,
                    tags=["control-flow", "bounds", "complexity"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        capability = config.get("capability", "compute-dominators")
        if capability not in {"compute-dominators", "detect-loops", "induction-vars", "loop-bounds"}:
            from domainlet.bindlet_base import BindletError, BindletErrorCode
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown capability: {capability}",
                source="CtrlFlowBindlet",
            )
        self._capability = capability
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.analyses.ctrlflow import (
            DominatorTree,
            NaturalLoopDetector,
            InductionVariableAnalysis,
            LoopBoundAnalysis,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                if self._capability == "compute-dominators":
                    dt = DominatorTree(cfg)
                    dt.compute()
                    for nid, doms in dt._idom.items():
                        issues.append(
                            AnalysisIssue(
                                rule_id="ctrlflow.dominator",
                                message=f"Node {nid}: immediate dominator = {doms}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "detect-loops":
                    loops = NaturalLoopDetector(cfg).detect()
                    for loop in loops:
                        header = loop.header if hasattr(
                            loop, "header") else loop
                        issues.append(
                            AnalysisIssue(
                                rule_id="ctrlflow.loop",
                                message=f"Natural loop detected with header node {header.id}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                elif self._capability == "induction-vars":
                    iva = InductionVariableAnalysis(cfg)
                    result = iva.analyze()
                    for var_info in result:
                        issues.append(
                            AnalysisIssue(
                                rule_id="ctrlflow.induction",
                                message=f"Induction variable: {var_info}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

                elif self._capability == "loop-bounds":
                    lba = LoopBoundAnalysis(cfg)
                    bounds = lba.analyze()
                    for b in bounds:
                        issues.append(
                            AnalysisIssue(
                                rule_id="ctrlflow.bounds",
                                message=f"Loop bound: {b}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Dataflow Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class DataflowBindlet(AbstractDomainlet):
    """Bindlet for intra/interprocedural dataflow analyses."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.dataflow",
            version="0.6.0",
            description="Dataflow analysis framework — lattice-based intra/interprocedural solvers",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="run-forward",
                    description="Run a forward dataflow analysis on a CFG",
                    input_kind=InputKind.AST_JSON,
                    tags=["dataflow", "analysis", "lattice"],
                ),
                DomainletCapability(
                    name="run-interprocedural",
                    description="Run an interprocedural dataflow analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["dataflow", "interprocedural", "callgraph"],
                ),
                DomainletCapability(
                    name="run-all",
                    description="Run all available dataflow analyses",
                    input_kind=InputKind.AST_JSON,
                    tags=["dataflow", "batch", "comprehensive"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._analysis_name = config.get("analysis", "run-all")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.analyses.dataflow import (
            IntraproceduralSolver,
            InterproceduralSolver,
            run_forward_analysis,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                try:
                    if self._analysis_name == "run-forward":
                        result = run_forward_analysis(cfg)
                    elif self._analysis_name == "run-interprocedural":
                        solver = InterproceduralSolver()
                        result = solver.solve(cfg)
                    else:
                        solver = IntraproceduralSolver()
                        result = solver.solve(cfg)

                    if result is not None:
                        summary = getattr(result, "summary", str(result))
                        issues.append(
                            AnalysisIssue(
                                rule_id="dataflow.result",
                                message=f"Function '{func_name}': {summary}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="dataflow.error",
                            message=f"Dataflow analysis failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Distributive Analysis Bindlet (IFDS/IDE)
# ══════════════════════════════════════════════════════════════════════

class DistribBindlet(AbstractDomainlet):
    """Bindlet for IFDS/IDE distributive analysis frameworks."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.distrib",
            version="0.6.0",
            description="Distributive analysis — IFDS/IDE solvers for taint, uninitialized variables",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="ifds-taint",
                    description="Run IFDS-based taint analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["ifds", "taint", "distributive"],
                ),
                DomainletCapability(
                    name="ifds-uninit",
                    description="Detect possibly uninitialized variables via IFDS",
                    input_kind=InputKind.AST_JSON,
                    tags=["ifds", "uninitialized", "defect"],
                ),
                DomainletCapability(
                    name="ide-solve",
                    description="Run an IDE analysis with edge functions",
                    input_kind=InputKind.AST_JSON,
                    tags=["ide", "distributive", "environment"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "ifds-uninit")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.analyses.distrib import (
            IFDSSolver,
            IDESolver,
            PossiblyUninitializedIFDS,
            TaintFact,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                try:
                    if self._capability == "ifds-uninit":
                        ifds = PossiblyUninitializedIFDS(cfg)
                        solver = IFDSSolver(ifds)
                        result = solver.solve()
                    elif self._capability == "ifds-taint":
                        taint_fact = TaintFact()
                        solver = IFDSSolver(taint_fact)
                        result = solver.solve()
                    else:
                        solver = IDESolver()
                        result = solver.solve(cfg)

                    if result is not None:
                        summary = getattr(result, "summary", str(
                            len(result)) if hasattr(result, "__len__") else str(result))
                        issues.append(
                            AnalysisIssue(
                                rule_id="distrib.result",
                                message=f"Analysis for '{func_name}': {summary}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="distrib.error",
                            message=f"Distributive analysis failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Interprocedural Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class InterProcBindlet(AbstractDomainlet):
    """Bindlet for interprocedural analysis strategies."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.interproc",
            version="0.6.0",
            description="Interprocedural analysis — context-insensitive, call-string, functional approaches",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="context-insensitive",
                    description="Context-insensitive interprocedural analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["interprocedural", "context-insensitive", "callgraph"],
                ),
                DomainletCapability(
                    name="call-string",
                    description="Call-string-sensitive interprocedural analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["interprocedural", "call-string", "k-limiting"],
                ),
                DomainletCapability(
                    name="functional",
                    description="Functional approach to interprocedural analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["interprocedural", "functional", "summary"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._strategy = config.get("strategy", "context-insensitive")
        valid = {"context-insensitive", "call-string", "functional"}
        if self._strategy not in valid:
            from domainlet.bindlet_base import BindletError, BindletErrorCode
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown strategy '{self._strategy}'; choose from {valid}",
                source="InterProcBindlet",
            )
        self._k_limit = int(config.get("k", "2"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.graphs.callgraph import CallGraph
        from domainlet.analyses.interproc import (
            ContextInsensitiveAnalysis,
            CallStringSensitiveAnalysis,
            FunctionalAnalysis,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            cfgs = build_all_cfgs(configuration)
            cg = CallGraph(configuration)

            try:
                if self._strategy == "context-insensitive":
                    analysis = ContextInsensitiveAnalysis(cg, cfgs)
                elif self._strategy == "call-string":
                    analysis = CallStringSensitiveAnalysis(
                        cg, cfgs, k=self._k_limit)
                else:
                    analysis = FunctionalAnalysis(cg, cfgs)

                result = analysis.analyze()
                summary = getattr(result, "summary", str(result))
                issues.append(
                    AnalysisIssue(
                        rule_id="interproc.result",
                        message=f"Interprocedural analysis ({self._strategy}): {summary}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.MEDIUM,
                    )
                )
            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="interproc.error",
                        message=f"Interprocedural analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Path Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class PathBindlet(AbstractDomainlet):
    """Bindlet for path-sensitive analysis via path enumeration."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.path",
            version="0.6.0",
            description="Path analysis — enumerate feasible paths with predicate constraints",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="enumerate-paths",
                    description="Enumerate paths through a CFG with path predicates",
                    input_kind=InputKind.AST_JSON,
                    tags=["path", "enumeration", "feasibility"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._max_paths = int(config.get("max_paths", "100"))
        self._max_depth = int(config.get("max_depth", "20"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.analyses.path import PathEnumerator

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                try:
                    enumerator = PathEnumerator(cfg, max_paths=self._max_paths)
                    paths = list(enumerator.enumerate(
                        max_depth=self._max_depth))
                    issues.append(
                        AnalysisIssue(
                            rule_id="path.count",
                            message=f"Function '{func_name}': {len(paths)} path(s) enumerated",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                    for i, path in enumerate(paths[:10]):
                        cond = getattr(path, "predicate", "")
                        issues.append(
                            AnalysisIssue(
                                rule_id="path.item",
                                message=f"Path {i}: nodes={getattr(path, 'nodes', path)}, "
                                        f"predicate={cond}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="path.error",
                            message=f"Path enumeration failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Taint Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class TaintBindlet(AbstractDomainlet):
    """Bindlet for taint analysis with configurable sources/sinks/sanitizers."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.taint",
            version="0.6.0",
            description="Taint tracking — configurable sources, sinks, sanitizers, and propagation rules",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="taint-track",
                    description="Run taint analysis from sources to sinks",
                    input_kind=InputKind.AST_JSON,
                    tags=["taint", "security", "dataflow"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._sources = config.get("sources", "").split(
            ",") if config.get("sources") else None
        self._sinks = config.get("sinks", "").split(
            ",") if config.get("sinks") else None
        self._sanitizers = config.get("sanitizers", "").split(
            ",") if config.get("sanitizers") else None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.analyses.taint import TaintAnalyzer, TaintConfig

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        taint_config = TaintConfig()
        if self._sources:
            for s in self._sources:
                if s.strip():
                    taint_config.add_source(s.strip())
        if self._sinks:
            for s in self._sinks:
                if s.strip():
                    taint_config.add_sink(s.strip())
        if self._sanitizers:
            for s in self._sanitizers:
                if s.strip():
                    taint_config.add_sanitizer(s.strip())

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                try:
                    analyzer = TaintAnalyzer(cfg, config=taint_config)
                    findings = analyzer.analyze()
                    for finding in findings:
                        issues.append(
                            AnalysisIssue(
                                rule_id="taint.flow",
                                message=str(finding),
                                severity=Severity.WARNING,
                                confidence=Confidence.MEDIUM,
                                cwe_ids=[20],
                            )
                        )
                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="taint.error",
                            message=f"Taint analysis failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Type Analysis Bindlet
# ══════════════════════════════════════════════════════════════════════

class TypingBindlet(AbstractDomainlet):
    """Bindlet for type inference and constraint-based type analysis."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="analyses.typing",
            version="0.6.0",
            description="Type analysis — C type inference, constraint solving, union-find based type environment",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer-types",
                    description="Infer C types across a function or translation unit",
                    input_kind=InputKind.AST_JSON,
                    tags=["type", "inference", "constraint"],
                ),
                DomainletCapability(
                    name="check-types",
                    description="Check for type mismatches and implicit conversion issues",
                    input_kind=InputKind.AST_JSON,
                    tags=["type", "checking", "mismatch"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check-types")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.analyses.typing import (
            CType,
            TypeEnvironment,
            TypeConstraint,
            UnionFind,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                env = TypeEnvironment()
                for func in getattr(configuration, "functions", []):
                    env.register_function(func)

                if self._capability == "infer-types":
                    result = env.infer()
                    for var_name, ctype in result.items():
                        issues.append(
                            AnalysisIssue(
                                rule_id="typing.inferred",
                                message=f"Variable '{var_name}': inferred type = {ctype}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                else:
                    mismatches = env.check()
                    for mismatch in mismatches:
                        issues.append(
                            AnalysisIssue(
                                rule_id="typing.mismatch",
                                message=str(mismatch),
                                severity=Severity.WARNING,
                                confidence=Confidence.MEDIUM,
                            )
                        )
            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="typing.error",
                        message=f"Type analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues
