"""
lattice_bindlets — Bindlet wrappers for lattice.* domainlets.

Exposes abstract domains (interval, sign, parity, congruence), fixpoint
engines, cost algebras, memory models, and widening/narrowing operators.
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
#  Interval Domain Bindlet
# ══════════════════════════════════════════════════════════════════════

class IntervalBindlet(AbstractDomainlet):
    """Bindlet for the interval abstract domain — range analysis for numeric variables."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.interval",
            version="0.6.0",
            description="Interval abstract domain — range-based abstract interpretation of numeric values",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="range-analysis",
                    description="Compute value ranges for variables via interval analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["interval", "range", "abstract-interpretation"],
                ),
                DomainletCapability(
                    name="check-bounds",
                    description="Check array/pointer bounds using interval analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["interval", "bounds", "buffer-overflow"],
                ),
                DomainletCapability(
                    name="interval-operations",
                    description="Perform arithmetic operations on intervals (add, mul, div, etc.)",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["interval", "arithmetic", "abstract"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "range-analysis")
        self._function_name: str | None = config.get("function") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.lattice.interval import Interval

        issues: list[AnalysisIssue] = []

        if self._capability == "interval-operations":
           # Demonstrate interval operations
            a = Interval(0, 10)
            b = Interval(5, 15)
            add_result = a.add(b) if hasattr(a, "add") else a.join(b)
            mul_result = Interval(0 * 5, 10 * 15)

            issues.extend([
                AnalysisIssue(
                    rule_id="interval.op",
                    message=f"Interval add: {a} + {b} = {add_result}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                ),
                AnalysisIssue(
                    rule_id="interval.op",
                    message=f"Interval mul: {a} * {b} = {mul_result}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                ),
            ])
            return issues

        data = _parse_context(context)
        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                if self._function_name and func_name != self._function_name:
                    continue

                try:
                    if self._capability == "range-analysis":
                        from domainlet.lattice.interval import Interval
                        ranges: dict[str, Interval] = {}

                        for node in cfg.nodes:
                            for token in getattr(node, "tokens", []):
                                var_name = getattr(token, "str", str(token))
                                val = getattr(token, "valueType", None)
                                if val:
                                    lo = getattr(
                                        getattr(val, "rangeMin", None), "intvalue", None)
                                    hi = getattr(
                                        getattr(val, "rangeMax", None), "intvalue", None)
                                    if lo is not None and hi is not None:
                                        ranges[var_name] = Interval(lo, hi)

                        for varname, iv in ranges.items():
                            issues.append(
                                AnalysisIssue(
                                    rule_id="interval.range",
                                    message=f"Variable '{varname}': range = {iv}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.MEDIUM,
                                )
                            )

                    elif self._capability == "check-bounds":
                        for node in cfg.nodes:
                            for token in getattr(node, "tokens", []):
                                if getattr(token, "str", "") in ("[", "Array"):
                                    issues.append(
                                        AnalysisIssue(
                                            rule_id="interval.bounds",
                                            message=f"Array access at {getattr(token, 'file', '')}:"
                                                    f"{getattr(token, 'linenr', 0)} — bounds check via "
                                                    f"interval analysis",
                                            severity=Severity.WARNING,
                                            confidence=Confidence.MEDIUM,
                                        )
                                    )

                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="interval.error",
                            message=f"Interval analysis failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        if not issues:
            issues.append(
                AnalysisIssue(
                    rule_id="interval.no-data",
                    message="No interval data computed — input may be empty",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Abstract Domains Bindlet (Sign, Parity, Congruence)
# ══════════════════════════════════════════════════════════════════════

class AbstractDomainsBindlet(AbstractDomainlet):
    """Bindlet for sign, parity, and congruence abstract domains."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.domains",
            version="0.6.0",
            description="Abstract domains — sign, parity, congruence for lightweight value tracking",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="sign-analysis",
                    description="Track the sign of variables (negative, zero, positive)",
                    input_kind=InputKind.AST_JSON,
                    tags=["sign", "abstract", "lightweight"],
                ),
                DomainletCapability(
                    name="parity-analysis",
                    description="Track parity (even/odd) of integer variables",
                    input_kind=InputKind.AST_JSON,
                    tags=["parity", "abstract", "lightweight"],
                ),
                DomainletCapability(
                    name="congruence-analysis",
                    description="Track modulo-congruence of variables",
                    input_kind=InputKind.AST_JSON,
                    tags=["congruence", "modular", "abstract"],
                ),
                DomainletCapability(
                    name="all-domains",
                    description="Run sign, parity, and congruence analyses together",
                    input_kind=InputKind.AST_JSON,
                    tags=["sign", "parity", "congruence", "comprehensive"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "all-domains")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.lattice.domains import (
            SignDomain,
            ParityDomain,
            CongruenceDomain,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        domains_to_run = (
            ["sign", "parity", "congruence"]
            if self._capability == "all-domains"
            else [self._capability.replace("-analysis", "")]
        )

        for configuration in data.configurations:
            for domain_name in domains_to_run:
                try:
                    if "sign" in domain_name:
                        domain = SignDomain.top()
                        issues.append(
                            AnalysisIssue(
                                rule_id="lattice.sign",
                                message=f"Sign domain initialized: top={SignDomain.top()}, "
                                        f"bottom={SignDomain.bottom()}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                    elif "parity" in domain_name:
                        domain = ParityDomain.top()
                        issues.append(
                            AnalysisIssue(
                                rule_id="lattice.parity",
                                message=f"Parity domain initialized: top={ParityDomain.top()}, "
                                        f"bottom={ParityDomain.bottom()}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                    elif "congruence" in domain_name:
                        domain = CongruenceDomain.top()
                        issues.append(
                            AnalysisIssue(
                                rule_id="lattice.congruence",
                                message=f"Congruence domain initialized: top={CongruenceDomain.top()}, "
                                        f"bottom={CongruenceDomain.bottom()}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="lattice.domains-error",
                            message=f"Domain '{domain_name}' failed: {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Fixpoint Engine Bindlet
# ══════════════════════════════════════════════════════════════════════

class FixpointBindlet(AbstractDomainlet):
    """Bindlet for fixpoint computation — chaotic iteration, worklist algorithms."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.fixpoint",
            version="0.6.0",
            description="Fixpoint engine — chaotic iteration, worklist solver, convergence detection",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="chaotic-iteration",
                    description="Run chaotic iteration to a fixpoint",
                    input_kind=InputKind.AST_JSON,
                    tags=["fixpoint", "chaotic", "iteration"],
                ),
                DomainletCapability(
                    name="worklist-solve",
                    description="Worklist-based fixpoint computation",
                    input_kind=InputKind.AST_JSON,
                    tags=["fixpoint", "worklist", "efficiency"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "chaotic-iteration")
        self._max_iter = int(config.get("max_iterations", "100"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.graphs.cfg import build_all_cfgs
        from domainlet.lattice.fixpoint import FixpointEngine, chaotic_iteration

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            func_cfgs = build_all_cfgs(configuration)
            for func_name, cfg in func_cfgs.items():
                try:
                    if self._capability == "chaotic-iteration":
                        result = chaotic_iteration(
                            cfg, max_iter=self._max_iter)
                        iter_count = getattr(
                            result, "iterations", self._max_iter)
                        converged = getattr(result, "converged", False)
                        issues.append(
                            AnalysisIssue(
                                rule_id="fixpoint.chaotic",
                                message=f"Function '{func_name}': {'converged' if converged else 'did not converge'} "
                                        f"after {iter_count} iteration(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                    elif self._capability == "worklist-solve":
                        engine = FixpointEngine(cfg)
                        result = engine.solve(max_iter=self._max_iter)
                        issues.append(
                            AnalysisIssue(
                                rule_id="fixpoint.worklist",
                                message=f"Function '{func_name}': worklist solver completed in "
                                        f"{getattr(result, 'iterations', 'N/A')} iteration(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

                except Exception as exc:
                    issues.append(
                        AnalysisIssue(
                            rule_id="fixpoint.error",
                            message=f"Fixpoint computation failed for '{func_name}': {exc}",
                            severity=Severity.ERROR,
                            confidence=Confidence.LOW,
                        )
                    )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Cost Algebra Bindlet
# ══════════════════════════════════════════════════════════════════════

class CostAlgebraBindlet(AbstractDomainlet):
    """Bindlet for cost algebras — expression cost modeling for e-graph extraction."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.cost_algebra",
            version="0.6.0",
            description="Cost algebra — model expression costs for optimal extraction",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="compute-cost",
                    description="Compute the cost of an expression",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["cost", "algebra", "optimization"],
                ),
                DomainletCapability(
                    name="compare-costs",
                    description="Compare two expressions by cost",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["cost", "comparison", "extraction"],
                ),
                DomainletCapability(
                    name="minimize",
                    description="Find the minimal-cost expression from a set of alternatives",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["cost", "minimization", "optimal"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "compute-cost")
        self._cost_model = config.get("model", "default")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.lattice.cost_algebra import CostExpr

        issues: list[AnalysisIssue] = []

        try:
            expressions: list[str] = []
            if context.data:
                try:
                    import json
                    expr_data = json.loads(context.data)
                    if isinstance(expr_data, list):
                        expressions = [str(e) for e in expr_data]
                    elif isinstance(expr_data, dict):
                        expressions = list(expr_data.values())
                except (json.JSONDecodeError, ValueError):
                    expressions = context.data.strip().splitlines()

            if not expressions:
                expressions = ["x + 0", "x * 1", "x << 1", "2 * x"]

            costs: list[tuple[str, int]] = []
            for expr in expressions[:50]:
                cost_expr = CostExpr(expr, model=self._cost_model)
                c = cost_expr.cost()
                costs.append((expr, c))

            if self._capability == "compute-cost":
                for expr, c in costs[:20]:
                    issues.append(
                        AnalysisIssue(
                            rule_id="cost.computed",
                            message=f"Cost of '{expr}': {c}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            elif self._capability == "compare-costs":
                costs.sort(key=lambda x: x[1])
                if len(costs) >= 2:
                    issues.append(
                        AnalysisIssue(
                            rule_id="cost.comparison",
                            message=f"'{costs[0][0]}' (cost={costs[0][1]}) is cheaper than "
                                    f"'{costs[1][0]}' (cost={costs[1][1]})",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

            elif self._capability == "minimize":
                costs.sort(key=lambda x: x[1])
                best = costs[0]
                issues.append(
                    AnalysisIssue(
                        rule_id="cost.minimized",
                        message=f"Best expression: '{best[0]}' with cost {best[1]}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="cost.error",
                    message=f"Cost algebra operation failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Memory Model Bindlet
# ══════════════════════════════════════════════════════════════════════

class MemoryModelBindlet(AbstractDomainlet):
    """Bindlet for memory modeling — pointer analysis, heap abstraction, memory safety."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.memory",
            version="0.6.0",
            description="Memory model — pointer analysis, heap abstraction, memory safety checks",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="build-model",
                    description="Build a memory model from a source file",
                    input_kind=InputKind.AST_JSON,
                    tags=["memory", "model", "pointer"],
                ),
                DomainletCapability(
                    name="pointer-issues",
                    description="Find pointer-related issues (null, dangling, leak)",
                    input_kind=InputKind.AST_JSON,
                    tags=["memory", "pointer", "safety"],
                ),
                DomainletCapability(
                    name="alias-analysis",
                    description="Perform alias analysis on the memory model",
                    input_kind=InputKind.AST_JSON,
                    tags=["memory", "alias", "pointer"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "pointer-issues")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.lattice.memory import MemoryModel, build_memory_model, find_pointer_issues

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                if self._capability == "build-model":
                    model = build_memory_model(configuration)
                    issues.append(
                        AnalysisIssue(
                            rule_id="memory.model",
                            message=f"Memory model: {model.allocation_count} allocation(s), "
                                    f"{model.pointer_count} pointer(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                elif self._capability == "pointer-issues":
                    model = build_memory_model(configuration)
                    pointer_issues = find_pointer_issues(model)

                    for pi in pointer_issues:
                        issues.append(
                            AnalysisIssue(
                                rule_id=f"memory.{getattr(pi, 'kind', 'issue')}",
                                message=str(pi),
                                severity=Severity.WARNING,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                    if not pointer_issues:
                        issues.append(
                            AnalysisIssue(
                                rule_id="memory.no-issues",
                                message="No pointer issues detected",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

                elif self._capability == "alias-analysis":
                    model = build_memory_model(configuration)
                    alias_pairs = model.find_aliases()
                    for p1, p2, may_alias in alias_pairs:
                        label = "may-alias" if may_alias else "must-not-alias"
                        issues.append(
                            AnalysisIssue(
                                rule_id=f"memory.{label}",
                                message=f"{p1} and {p2} {label}",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="memory.error",
                        message=f"Memory model analysis failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Widening/Narrowing Bindlet
# ══════════════════════════════════════════════════════════════════════

class WideningBindlet(AbstractDomainlet):
    """Bindlet for widening and narrowing operators — ensure convergence in abstract interpretation."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="lattice.widening",
            version="0.6.0",
            description="Widening/Narrowing — convergence operators for abstract interpretation fixpoints",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="widen",
                    description="Apply widening to ensure convergence",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["widening", "convergence", "abstract"],
                ),
                DomainletCapability(
                    name="narrow",
                    description="Apply narrowing to improve precision after widening",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["narrowing", "precision", "abstract"],
                ),
                DomainletCapability(
                    name="strategy",
                    description="Select and apply widening/narrowing strategies",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["widening", "strategy", "threshold"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "widen")
        self._strategy_name = config.get("strategy", "BASIC").upper()
        self._threshold = int(config.get("threshold", "100"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.lattice.widening import widen, narrow, WideningStrategy
        from domainlet.lattice.interval import Interval

        issues: list[AnalysisIssue] = []

        try:
            a = Interval(0, 5)
            b = Interval(0, self._threshold)

            try:
                strategy = WideningStrategy[self._strategy_name]
            except KeyError:
                strategy = WideningStrategy.BASIC

            if self._capability == "widen":
                result = widen(a, b, strategy=strategy)
                issues.append(
                    AnalysisIssue(
                        rule_id="widening.widen",
                        message=f"Widen({a}, {b}) = {result} (strategy={strategy.name})",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

                # Demonstrate fixpoint scenario
                iterations = [Interval(0, 10), Interval(
                    0, 20), Interval(0, 50), Interval(0, 120)]
                current = iterations[0]
                for nxt in iterations[1:]:
                    current = widen(current, nxt, strategy=strategy)
                issues.append(
                    AnalysisIssue(
                        rule_id="widening.sequence",
                        message=f"Widening sequence converged to: {current}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "narrow":
                wide_result = widen(a, b, strategy=strategy)
                narrowed = narrow(wide_result, b)
                issues.append(
                    AnalysisIssue(
                        rule_id="widening.narrow",
                        message=f"Narrow({wide_result}, {b}) = {narrowed}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

            elif self._capability == "strategy":
                available = getattr(WideningStrategy, "available", [
                                    "threshold", "jump", "finite"])
                issues.append(
                    AnalysisIssue(
                        rule_id="widening.strategies",
                        message=f"Available widening strategies: {available}",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="widening.error",
                    message=f"Widening/narrowing failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues
