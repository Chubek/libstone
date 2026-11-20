"""
symbolic_bindlets — Bindlet wrappers for symbolic.* domainlets.

Exposes symbolic execution, constraint solving, equality saturation,
and transition-system safety checking through the unified interface.
"""

from __future__ import annotations

import tempfile
import os
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


def _parse_context(context: AnalysisContext) -> Any:
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


# ══════════════════════════════════════════════════════════════════════
#  Symbolic Executor Bindlet
# ══════════════════════════════════════════════════════════════════════

class SymbolicExecBindlet(AbstractDomainlet):
    """Bindlet for symbolic execution — explore paths, collect path conditions."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="symbolic.exec",
            version="0.6.0",
            description="Symbolic execution engine — explore program paths with symbolic inputs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="explore-paths",
                    description="Symbolically execute a function, exploring all feasible paths",
                    input_kind=InputKind.AST_JSON,
                    tags=["symbolic", "execution", "path-exploration"],
                ),
                DomainletCapability(
                    name="collect-constraints",
                    description="Collect path constraints from symbolic execution",
                    input_kind=InputKind.AST_JSON,
                    tags=["symbolic", "constraints", "path-condition"],
                ),
                DomainletCapability(
                    name="concolic",
                    description="Run concolic execution (concrete + symbolic)",
                    input_kind=InputKind.AST_JSON,
                    tags=["symbolic", "concolic", "hybrid"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "explore-paths")
        self._max_paths = int(config.get("max_paths", "100"))
        self._max_depth = int(config.get("max_depth", "50"))
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.symbolic.exec import SymbolicExecutor

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                try:
                    executor = SymbolicExecutor(cfg, max_paths=self._max_paths)
                    results = executor.explore(max_depth=self._max_depth)

                    path_count = len(results) if isinstance(
                        results, list) else 1
                    issues.append(
                        AnalysisIssue(
                            rule_id="symexec.paths",
                            message=f"Function '{func_name}': {path_count} symbolic path(s) explored",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                    if self._capability == "collect-constraints":
                        for i, path_result in enumerate(results[:20]):
                            constraints = getattr(
                                path_result, "constraints", [])
                            state = getattr(path_result, "state", None)
                            issues.append(
                                AnalysisIssue(
                                    rule_id="symexec.constraints",
                                    message=f"Path {i}: {len(constraints)} constraint(s), "
                                            f"state={state}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="symexec.error",
                            message=f"Symbolic execution failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Constraint Engine Bindlet
# ══════════════════════════════════════════════════════════════════════

class ConstraintsBindlet(AbstractDomainlet):
    """Bindlet for constraint solving — SMT-LIB generation, Z3 integration."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="symbolic.constraints",
            version="0.6.0",
            description="Constraint engine — build, simplify, solve logical constraints via Z3",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="solve",
                    description="Solve a set of constraints and check satisfiability",
                    input_kind=InputKind.AST_JSON,
                    tags=["constraints", "smt", "sat"],
                ),
                DomainletCapability(
                    name="simplify",
                    description="Simplify a constraint expression",
                    input_kind=InputKind.AST_JSON,
                    tags=["constraints", "simplification", "rewrite"],
                ),
                DomainletCapability(
                    name="to-smt",
                    description="Convert constraints to SMT-LIB format",
                    input_kind=InputKind.AST_JSON,
                    tags=["constraints", "smt-lib", "export"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "solve")
        self._solver_timeout = int(config.get("timeout_ms", "5000"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.symbolic.constraints import ConstraintEngine

        issues: list[AnalysisIssue] = []

        try:
            engine = ConstraintEngine(timeout=self._solver_timeout)

            if self._capability == "solve":
                # Attempt to parse constraints from context data
                try:
                    import json
                    constraint_data = json.loads(
                        context.data) if context.data.strip().startswith("{") else {}
                except (json.JSONDecodeError, ValueError):
                    constraint_data = {}

                result = engine.check(constraint_data)
                sat = str(getattr(result, "sat", result))
                issues.append(
                    AnalysisIssue(
                        rule_id="constraints.result",
                        message=f"Constraint check result: {sat}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "simplify":
                simplified = engine.simplify_all()
                issues.append(
                    AnalysisIssue(
                        rule_id="constraints.simplified",
                        message=f"Simplified constraints: {simplified}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "to-smt":
                smt_str = engine.to_smt()
                issues.append(
                    AnalysisIssue(
                        rule_id="constraints.smt",
                        message=f"SMT-LIB output: {smt_str[:500]}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="constraints.error",
                    message=f"Constraint engine failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Equality Saturation Bindlet (e-graph rewriting)
# ══════════════════════════════════════════════════════════════════════

class EqSatBindlet(AbstractDomainlet):
    """Bindlet for equality saturation — e-graph construction, rewrite rules, extraction."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="symbolic.rweqsat",
            version="0.6.0",
            description="Equality saturation — e-graph construction, term rewriting, cost-based extraction",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-egraph",
                    description="Build an e-graph from a set of expressions",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "equality", "rewriting"],
                ),
                DomainletCapability(
                    name="saturate",
                    description="Run equality saturation with rewrite rules until fixpoint",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "saturation", "fixpoint"],
                ),
                DomainletCapability(
                    name="extract-best",
                    description="Extract the best (cheapest) expression from an e-class",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "extraction", "cost"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "saturate")
        self._rule_file: str | None = config.get("rule_file") or None
        self._max_iter = int(config.get("max_iterations", "10"))
        self._max_size = int(config.get("max_egraph_size", "10000"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.symbolic.rweqsat import EGraph, EqualitySaturation, RewriteRule

        issues: list[AnalysisIssue] = []

        try:
            egraph = EGraph()

            if self._rule_file:
                try:
                    import json
                    with open(self._rule_file) as rf:
                        rules_data = json.load(rf)
                except Exception:
                    rules_data = []

                rules = [RewriteRule(**r) if isinstance(r, dict)
                         else r for r in rules_data]
            else:
                rules = []

            if self._capability == "build-egraph":
                issues.append(
                    AnalysisIssue(
                        rule_id="egraph.built",
                        message=f"E-graph: {egraph.size()} e-classes, {len(rules)} rules loaded",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "saturate":
                sat = EqualitySaturation(
                    egraph, rules, max_iter=self._max_iter)
                result = sat.run()
                applied = getattr(result, "matches_applied", result)
                issues.append(
                    AnalysisIssue(
                        rule_id="egraph.saturated",
                        message=f"Equality saturation complete: {applied} rule application(s), "
                                f"{egraph.size()} e-classes",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "extract-best":
                best_expressions = []
                for eclass_id in egraph.eclasses():
                    best = egraph.extract(eclass_id)
                    if best is not None:
                        best_expressions.append(str(best))
                issues.append(
                    AnalysisIssue(
                        rule_id="egraph.extracted",
                        message=f"Extracted {len(best_expressions)} best expression(s)",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="egraph.error",
                    message=f"E-graph operation failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Transition System Bindlet (safety checking, bounded model checking)
# ══════════════════════════════════════════════════════════════════════

class TransitionBindlet(AbstractDomainlet):
    """Bindlet for transition-system analysis — safety checking, bounded exploration."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="symbolic.transition",
            version="0.6.0",
            description="Transition system analysis — safety property checking, bounded model checking",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="safety-check",
                    description="Check a safety property on a transition system",
                    input_kind=InputKind.AST_JSON,
                    tags=["transition", "safety", "verification"],
                ),
                DomainletCapability(
                    name="bounded-explore",
                    description="Bounded exploration of state space up to depth k",
                    input_kind=InputKind.AST_JSON,
                    tags=["transition", "bmc", "bounded"],
                ),
                DomainletCapability(
                    name="invariant-check",
                    description="Check whether a candidate invariant holds",
                    input_kind=InputKind.AST_JSON,
                    tags=["transition", "invariant", "inductive"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "safety-check")
        self._bound = int(config.get("bound", "20"))
        self._property: str | None = config.get("property") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.symbolic.transition import SafetyChecker, BoundedExplorer, Transition

        issues: list[AnalysisIssue] = []

        try:
            if self._capability == "safety-check":
                checker = SafetyChecker()
                result = checker.check(self._property)

                if getattr(result, "safe", False):
                    issues.append(
                        AnalysisIssue(
                            rule_id="transition.safe",
                            message=f"Safety property '{self._property}' holds",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                else:
                    counterexample = getattr(
                        result, "counterexample", str(result))
                    issues.append(
                        AnalysisIssue(
                            rule_id="transition.unsafe",
                            message=f"Safety property '{self._property}' violated: {counterexample}",
                            severity=Severity.ERROR,
                            confidence=Confidence.HIGH,
                        )
                    )

            elif self._capability == "bounded-explore":
                explorer = BoundedExplorer(bound=self._bound)
                reachable = explorer.explore()

                issues.append(
                    AnalysisIssue(
                        rule_id="transition.bounded",
                        message=f"Bounded exploration (k={self._bound}): "
                                f"{len(reachable)} reachable state(s)",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "invariant-check":
                checker = SafetyChecker()
                inductive = checker.is_inductive(self._property)

                issues.append(
                    AnalysisIssue(
                        rule_id="transition.invariant",
                        message=f"Invariant '{self._property}' is {'inductive' if inductive else 'not inductive'}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH if inductive else Confidence.MEDIUM,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="transition.error",
                    message=f"Transition analysis failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues
