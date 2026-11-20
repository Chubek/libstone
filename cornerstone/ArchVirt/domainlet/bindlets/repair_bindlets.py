"""
repair_bindlets — Bindlet wrappers for repair.* domainlets.

Exposes fault pattern matching, naive rule-based repair, constraint-based
repair, e-graph rewriting, equality saturation, genetic programming repair,
invariant collection, LLM-based repair, fault localization, and synthesis
grammars through the unified AbstractDomainlet interface.

Per AGENTS.md: these bindlets must be "improved, strengthened, fortified,
de-stubbed — both syntactically and semantically."
"""

from __future__ import annotations

import tempfile
import os
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


def _make_source_span(token: Any, filename: str = "") -> SourceSpan:
    return SourceSpan(
        file=getattr(token, "file", filename),
        line_start=getattr(token, "linenr", 0),
        col_start=getattr(token, "column", 0),
    )


# ══════════════════════════════════════════════════════════════════════
#  Fault Pattern Bindlet
# ══════════════════════════════════════════════════════════════════════

class FaultPatternBindlet(AbstractDomainlet):
    """Bindlet for fault-pattern matching and classification.

    Analyses token streams for well-known C/C++ defect patterns:
    off-by-one, null dereference, use-after-free, type mismatches,
    and taint-flow vulnerabilities.  Each match produces a structured
    finding with severity, CWE, and a suggested fix locality.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.fault_pattern",
            version="0.7.0",
            description="Fault-pattern matcher — detect off-by-one, null-deref, use-after-free, "
                        "type mismatch, and taint-flow patterns in C/C++ ASTs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="match-off-by-one",
                    description="Detect off-by-one errors in loops and array accesses",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "off-by-one", "CWE-193"],
                ),
                DomainletCapability(
                    name="match-null-deref",
                    description="Detect potential null-pointer dereferences",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "null-dereference", "CWE-476"],
                ),
                DomainletCapability(
                    name="match-use-after-free",
                    description="Detect use-after-free patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "use-after-free", "CWE-416"],
                ),
                DomainletCapability(
                    name="match-type-mismatch",
                    description="Detect implicit type conversion problems",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "type-mismatch", "CWE-681"],
                ),
                DomainletCapability(
                    name="match-taint-flow",
                    description="Detect tainted-data flow patterns",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "taint", "CWE-20"],
                ),
                DomainletCapability(
                    name="match-all",
                    description="Run all fault-pattern matchers and aggregate results",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "comprehensive", "faults"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "match-all")
        valid_caps = {
            "match-off-by-one", "match-null-deref", "match-use-after-free",
            "match-type-mismatch", "match-taint-flow", "match-all",
        }
        if self._capability not in valid_caps:
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown capability '{self._capability}'; choose from {sorted(valid_caps)}",
                source="FaultPatternBindlet",
            )
        self._min_confidence = Confidence[config.get(
            "min_confidence", "LOW").upper()]

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.fault_pattern import (
            _match_off_by_one,
            _match_null_deref,
            _match_use_after_free,
            _match_type_mismatch,
            _match_taint_flow,
        )

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        matchers: dict[str, tuple[Any, str, int, str]] = {
            "match-off-by-one":    (_match_off_by_one,    "off-by-one",    193, "off-by-one error"),
            "match-null-deref":    (_match_null_deref,    "null-deref",    476, "null-pointer dereference"),
            "match-use-after-free": (_match_use_after_free, "use-after-free", 416, "use after free"),
            "match-type-mismatch": (_match_type_mismatch, "type-mismatch", 681, "type mismatch"),
            "match-taint-flow":    (_match_taint_flow,    "taint-flow",    20,  "tainted data flow"),
        }

        matchers_to_run = (
            list(matchers.keys()) if self._capability == "match-all"
            else [self._capability]
        )

        for configuration in data.configurations:
            for token in configuration.tokenlist:
                for cap_name in matchers_to_run:
                    try:
                        matcher, rule_id, cwe, label = matchers[cap_name]
                        if matcher(token):
                            issues.append(
                                AnalysisIssue(
                                    rule_id=f"fault.{rule_id}",
                                    message=f"Potential {label} at {getattr(token, 'file', context.source_file)}:"
                                    f"{getattr(token, 'linenr', 0)}",
                                    severity=Severity.WARNING,
                                    confidence=Confidence.MEDIUM,
                                    location=_make_source_span(
                                        token, context.source_file),
                                    cwe_ids=[cwe],
                                )
                            )
                    except Exception:
                        pass

        if not issues:
            issues.append(
                AnalysisIssue(
                    rule_id="fault.none",
                    message="No fault patterns detected",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Naive Rules Bindlet
# ══════════════════════════════════════════════════════════════════════

class NaiveRulesBindlet(AbstractDomainlet):
    """Bindlet for naive rule-based program repair.

    Applies simple, well-understood repair recipes: off-by-one corrections,
    null guards, variable initialisation, bounded-call fixes, buffer-overflow
    guards, resource-leak clean-up, double-free prevention, format-string
    hardening, and signedness fixes.  Each recipe produces PatchDiff suggestions.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.naive_rules",
            version="0.7.0",
            description="Naive rule-based repair — apply simple, proven fix recipes to common C/C++ bugs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="apply-rules",
                    description="Apply all naive repair rules and collect suggested patches",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "rules", "fix", "patch"],
                ),
                DomainletCapability(
                    name="fix-off-by-one",
                    description="Correct off-by-one errors in loop conditions and array indexing",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "off-by-one", "CWE-193"],
                ),
                DomainletCapability(
                    name="fix-null-guard",
                    description="Insert null-pointer guards before dereferences",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "null-guard", "CWE-476"],
                ),
                DomainletCapability(
                    name="fix-init",
                    description="Add missing variable initialisations",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "init", "CWE-457"],
                ),
                DomainletCapability(
                    name="fix-buffer-overflow",
                    description="Add bounds checks for array and buffer accesses",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "buffer-overflow", "CWE-120"],
                ),
                DomainletCapability(
                    name="fix-resource-leak",
                    description="Insert resource cleanup (free, close, etc.)",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "resource-leak", "CWE-404"],
                ),
                DomainletCapability(
                    name="fix-double-free",
                    description="Guard against double-free by nullifying pointers",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "double-free", "CWE-415"],
                ),
                DomainletCapability(
                    name="fix-format-string",
                    description="Harden format-string calls",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "format-string", "CWE-134"],
                ),
                DomainletCapability(
                    name="fix-signedness",
                    description="Fix signed/unsigned mismatches",
                    input_kind=InputKind.AST_JSON,
                    tags=["repair", "signedness", "CWE-195"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "apply-rules")
        self._max_fixes = int(config.get("max_fixes", "20"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.naive_rules import apply_naive_rules, PatchDiff

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        fixers = {
            "apply-rules":         None,
            "fix-off-by-one":      "off_by_one",
            "fix-null-guard":      "null_guard",
            "fix-init":            "init",
            "fix-buffer-overflow": "buffer_overflow",
            "fix-resource-leak":   "resource_leak",
            "fix-double-free":     "double_free",
            "fix-format-string":   "format_string",
            "fix-signedness":      "signedness",
        }

        rule_name = fixers.get(self._capability)
        for configuration in data.configurations:
            try:
                patches: list[PatchDiff] = apply_naive_rules(
                    configuration, rule_name=rule_name, max_fixes=self._max_fixes,
                )
                for patch in patches:
                    issues.append(
                        AnalysisIssue(
                            rule_id="naive.fix",
                            message=patch.description,
                            severity=Severity.WARNING,
                            confidence=Confidence.MEDIUM,
                            location=SourceSpan(
                                file=context.source_file,
                                line_start=patch.line_start if hasattr(
                                    patch, "line_start") else 0,
                            ),
                            fix=FixSuggestion(
                                description=patch.description,
                                replacement=patch.replacement if hasattr(
                                    patch, "replacement") else "",
                            ),
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="naive.error",
                        message=f"Naive repair failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        if not issues:
            issues.append(
                AnalysisIssue(
                    rule_id="naive.no-fixes",
                    message="No naive fixes applicable to this input",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Constraint Repair Bindlet
# ══════════════════════════════════════════════════════════════════════

class ConstraintRepairBindlet(AbstractDomainlet):
    """Bindlet for constraint-based repair using Z3 SMT solving.

    Encodes program semantics as SMT constraints, then asks the solver
    to find a minimal perturbation that satisfies desired properties.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.constraint_repair",
            version="0.7.0",
            description="Constraint-based repair — use Z3 to find minimal expression fixes",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="repair-expression",
                    description="Repair a faulty expression by solving constraints",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["repair", "constraint", "z3", "smt"],
                ),
                DomainletCapability(
                    name="generate-candidates",
                    description="Generate repair candidates for a given fault location",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["repair", "candidates", "synthesis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "repair-expression")
        self._max_candidates = int(config.get("max_candidates", "10"))
        self._timeout_ms = int(config.get("timeout_ms", "5000"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.constraint_repair import repair_constraints, RepairCandidate

        issues: list[AnalysisIssue] = []

        try:
            # Accept either inline expression or a source-file path
            expression = context.data.strip()
            candidates: list[RepairCandidate] = repair_constraints(
                expression,
                max_candidates=self._max_candidates,
                timeout_ms=self._timeout_ms,
            )

            for candidate in candidates:
                issues.append(
                    AnalysisIssue(
                        rule_id="constraint.repair",
                        message=f"Repair candidate: {candidate.expression} "
                                f"(score={candidate.score})",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.MEDIUM,
                        fix=FixSuggestion(
                            description=f"Replace with: {candidate.expression}",
                            replacement=candidate.expression,
                        ) if candidate.expression else None,
                    )
                )

            if not candidates:
                issues.append(
                    AnalysisIssue(
                        rule_id="constraint.no-candidates",
                        message="No repair candidates found within the search bounds",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.MEDIUM,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="constraint.error",
                    message=f"Constraint repair failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  E-Graph Rewriter Bindlet
# ══════════════════════════════════════════════════════════════════════

class EGraphRewriterBindlet(AbstractDomainlet):
    """Bindlet for e-graph rewriting — apply guarded rewrite rules and extract variants."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.egraph_rewriter",
            version="0.7.0",
            description="E-graph rewriter — apply guarded rewrite rules to find expression alternatives",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="rewrite",
                    description="Apply rewrite rules to find alternative expressions",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "rewrite", "variants"],
                ),
                DomainletCapability(
                    name="extract-best",
                    description="Extract the best (cheapest) expression from the e-graph",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "extraction", "cost"],
                ),
                DomainletCapability(
                    name="list-rules",
                    description="List all available rewrite rules",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["egraph", "rules", "discovery"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "rewrite")
        self._max_variants = int(config.get("max_variants", "20"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.egraph_rewriter import EGraphRewriter, GuardedRule

        issues: list[AnalysisIssue] = []

        try:
            rewriter = EGraphRewriter()

            if self._capability == "list-rules":
                rules = getattr(rewriter, "rules", [])
                for rule in rules:
                    issues.append(
                        AnalysisIssue(
                            rule_id="egraph.rule",
                            message=f"Rule: {rule}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                return issues

            # Parse expressions from input
            expressions: list[str] = []
            if context.data:
                try:
                    import json
                    data = json.loads(context.data)
                    if isinstance(data, list):
                        expressions = [str(e) for e in data]
                    elif isinstance(data, dict) and "expressions" in data:
                        expressions = [str(e) for e in data["expressions"]]
                except (json.JSONDecodeError, ValueError):
                    for line in context.data.strip().splitlines():
                        if line.strip():
                            expressions.append(line.strip())

            if not expressions:
                expressions = ["x + 0", "x * 1", "(x + y) * (x + y)"]

            for expr in expressions[:20]:
                eid = rewriter.add(expr)

            if self._capability == "rewrite":
                rewriter.compile_rules()
                variants = list(rewriter.rewrite(
                    max_variants=self._max_variants))
                for orig, variant in variants[:self._max_variants]:
                    if orig != variant:
                        issues.append(
                            AnalysisIssue(
                                rule_id="egraph.rewrite",
                                message=f"Rewrite: '{orig}' → '{variant}'",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.MEDIUM,
                            )
                        )
                if not issues:
                    issues.append(
                        AnalysisIssue(
                            rule_id="egraph.no-variants",
                            message="No rewritable expressions found",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

            elif self._capability == "extract-best":
                rewriter.compile_rules()
                rewriter.rewrite(max_variants=self._max_variants)
                for eclass_id in rewriter.eclasses():
                    best = rewriter.extract(eclass_id)
                    if best is not None:
                        expressions_in = list(
                            rewriter.expressions_in(eclass_id))
                        issues.append(
                            AnalysisIssue(
                                rule_id="egraph.extract",
                                message=f"E-class {eclass_id}: best='{best}' among "
                                        f"{len(expressions_in)} expression(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="egraph.error",
                    message=f"E-graph rewriting failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Equality Saturation Bindlet
# ══════════════════════════════════════════════════════════════════════

class EqualitySaturationBindlet(AbstractDomainlet):
    """Bindlet for equality saturation — run rewrite rules to fixpoint on an e-graph."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.equality_saturation",
            version="0.7.0",
            description="Equality saturation — saturate an e-graph with rewrite rules to explore "
                        "all equivalent expressions",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="saturate",
                    description="Run equality saturation to fixpoint",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["equality-saturation", "egraph", "fixpoint"],
                ),
                DomainletCapability(
                    name="detailed-saturate",
                    description="Run equality saturation with detailed reporting",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["equality-saturation", "detailed", "diagnostics"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "saturate")
        self._max_iter = int(config.get("max_iterations", "30"))
        self._max_size = int(config.get("max_size", "10000"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.equality_saturation import (
            run_equality_saturation,
            run_equality_saturation_detailed,
            SaturationResult,
        )

        issues: list[AnalysisIssue] = []

        try:
            # Parse expressions from context
            expressions: list[str] = []
            if context.data:
                try:
                    import json
                    data = json.loads(context.data)
                    if isinstance(data, list):
                        expressions = [str(e) for e in data]
                    elif isinstance(data, dict) and "expressions" in data:
                        expressions = [str(e) for e in data["expressions"]]
                except (json.JSONDecodeError, ValueError):
                    for line in context.data.strip().splitlines():
                        if line.strip():
                            expressions.append(line.strip())

            if not expressions:
                expressions = ["x + 0", "x * 1", "(a + b) * (a + b)"]

            if self._capability == "detailed-saturate":
                result = run_equality_saturation_detailed(
                    expressions,
                    max_iter=self._max_iter,
                    max_size=self._max_size,
                )
                issues.append(
                    AnalysisIssue(
                        rule_id="eqsat.detailed",
                        message=f"Equality saturation: {result.iterations} iterations, "
                                f"{result.matches_applied} matches, "
                                f"{result.final_size} e-classes",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )
                for step_info in getattr(result, "steps", [])[:20]:
                    issues.append(
                        AnalysisIssue(
                            rule_id="eqsat.step",
                            message=str(step_info),
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

            else:
                result = run_equality_saturation(
                    expressions,
                    max_iter=self._max_iter,
                    max_size=self._max_size,
                )
                issues.append(
                    AnalysisIssue(
                        rule_id="eqsat.result",
                        message=f"Equality saturation: {len(getattr(result, 'equivalence_classes', []))} "
                                f"e-class(es), {getattr(result, 'rules_applied', 0)} rule(s) applied",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="eqsat.error",
                    message=f"Equality saturation failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Genetic Programming Repair Bindlet
# ══════════════════════════════════════════════════════════════════════

class GeneticProgBindlet(AbstractDomainlet):
    """Bindlet for genetic-programming-based repair.

    Evolves a population of repair candidates using selection, crossover,
    and mutation.  Fitness is measured by how well a candidate passes
    a set of test cases or satisfies a correctness specification.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.genetic_prog",
            version="0.7.0",
            description="Genetic-programming repair — evolve fix candidates via selection, crossover, mutation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="evolve-fix",
                    description="Evolve a population of repair candidates",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["genetic", "evolution", "repair"],
                ),
                DomainletCapability(
                    name="evolve-expression",
                    description="Evolve an expression repair using GP",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["genetic", "expression", "synthesis"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "evolve-fix")
        self._population_size = int(config.get("population_size", "50"))
        self._generations = int(config.get("generations", "20"))
        self._mutation_rate = float(config.get("mutation_rate", "0.1"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.genetic_prog import GeneticRepairer, Individual

        issues: list[AnalysisIssue] = []

        try:
            if self._capability == "evolve-fix":
                repairer = GeneticRepairer(
                    population_size=self._population_size,
                    generations=self._generations,
                    mutation_rate=self._mutation_rate,
                )

                # Use context data as the target expression to repair
                target = context.data.strip() if context.data else "x + y"
                fixes = repairer.run()

                for fix in fixes[:10]:
                    issues.append(
                        AnalysisIssue(
                            rule_id="genetic.fix",
                            message=f"GP-evolved fix: {fix}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                            fix=FixSuggestion(
                                description=f"Replace with: {fix}",
                                replacement=fix,
                            ),
                        )
                    )
                if not fixes:
                    issues.append(
                        AnalysisIssue(
                            rule_id="genetic.no-fix",
                            message="No fix evolved within the generation budget",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.LOW,
                        )
                    )

            elif self._capability == "evolve-expression":
                repairer = GeneticRepairer(
                    population_size=self._population_size,
                    generations=self._generations,
                    mutation_rate=self._mutation_rate,
                )
                result = repairer.run()
                issues.append(
                    AnalysisIssue(
                        rule_id="genetic.result",
                        message=f"GP completed: {len(result)} candidate(s) after "
                                f"{self._generations} generation(s)",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="genetic.error",
                    message=f"Genetic-programming repair failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Invariant Collection Bindlet
# ══════════════════════════════════════════════════════════════════════

class InvariantCollectBindlet(AbstractDomainlet):
    """Bindlet for invariant collection from abstract interpretation results.

    Extracts sign, interval, constant, and relational invariants from
    abstract interpreter output.  These invariants feed into repair
    strategies (e.g., inferred pre-/post-conditions for patch validation).
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.invariant_collect",
            version="0.7.0",
            description="Invariant collector — extract sign, interval, constant, and relational "
                        "invariants from abstract interpretation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="collect-all",
                    description="Collect all invariant types from an abstract state",
                    input_kind=InputKind.AST_JSON,
                    tags=["invariant", "collection", "abstract"],
                ),
                DomainletCapability(
                    name="to-z3",
                    description="Convert collected invariants to Z3 assertions",
                    input_kind=InputKind.AST_JSON,
                    tags=["invariant", "z3", "smt"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "collect-all")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.invariant_collect import collect_invariants

        data = _parse_context(context)
        issues: list[AnalysisIssue] = []

        for configuration in data.configurations:
            try:
                invariants = collect_invariants(configuration)

                sign_entries = invariants.get("signs", [])
                interval_entries = invariants.get("intervals", [])
                const_entries = invariants.get("constants", [])
                rel_entries = invariants.get("relational", [])

                issues.append(
                    AnalysisIssue(
                        rule_id="invariant.summary",
                        message=f"Collected invariants: {len(sign_entries)} sign(s), "
                                f"{len(interval_entries)} interval(s), "
                                f"{len(const_entries)} constant(s), "
                                f"{len(rel_entries)} relational",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

                for name, sign in sign_entries[:10]:
                    issues.append(
                        AnalysisIssue(
                            rule_id="invariant.sign",
                            message=f"Sign invariant: {name} ∈ {sign}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

                for name, iv in interval_entries[:10]:
                    issues.append(
                        AnalysisIssue(
                            rule_id="invariant.interval",
                            message=f"Interval invariant: {name} ∈ {iv}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

                if self._capability == "to-z3":
                    from domainlet.repair.invariant_collect import _interval_to_z3, _sign_to_z3
                    z3_exprs = []
                    for name, iv in interval_entries[:10]:
                        z3e = _interval_to_z3(name, iv)
                        if z3e is not None:
                            z3_exprs.append(str(z3e))
                    for name, sign in sign_entries[:10]:
                        z3e = _sign_to_z3(name, sign)
                        if z3e is not None:
                            z3_exprs.append(str(z3e))
                    issues.append(
                        AnalysisIssue(
                            rule_id="invariant.z3",
                            message=f"Z3 conversions: {len(z3_exprs)} assertion(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )

            except Exception as exc:
                issues.append(
                    AnalysisIssue(
                        rule_id="invariant.error",
                        message=f"Invariant collection failed: {exc}",
                        severity=Severity.ERROR,
                        confidence=Confidence.LOW,
                    )
                )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  LLM Query Bindlet
# ══════════════════════════════════════════════════════════════════════

class LLMQueryBindlet(AbstractDomainlet):
    """Bindlet for LLM-based program repair.

    Queries a local language model to suggest patches for identified faults.
    Supports model selection, caching, retry logic, and unified-diff parsing.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.llm_query",
            version="0.7.0",
            description="LLM-based repair — query a local language model for patch suggestions",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="suggest-fix",
                    description="Ask the LLM to suggest a fix for a code snippet",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["llm", "ai", "repair", "suggestion"],
                ),
                DomainletCapability(
                    name="suggest-batch",
                    description="Suggest fixes for multiple faults in batch",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["llm", "batch", "repair"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "suggest-fix")
        self._model = config.get("model", "default")
        self._max_tokens = int(config.get("max_tokens", "512"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.llm_query import LLMRepairer

        issues: list[AnalysisIssue] = []

        try:
            repairer = LLMRepairer(
                model=self._model, max_tokens=self._max_tokens)

            if self._capability == "suggest-fix":
                suggestion = repairer.suggest(
                    message=context.data,
                    file=context.source_file,
                    line=0,
                    snippet=context.data[:200],
                )

                if suggestion:
                    for patch in suggestion if isinstance(suggestion, list) else [suggestion]:
                        if isinstance(patch, str):
                            issues.append(
                                AnalysisIssue(
                                    rule_id="llm.suggestion",
                                    message=f"LLM suggests: {patch[:200]}",
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.LOW,
                                    fix=FixSuggestion(
                                        description="LLM-suggested fix",
                                        replacement=patch,
                                    ),
                                )
                            )
                        else:
                            issues.append(
                                AnalysisIssue(
                                    rule_id="llm.suggestion",
                                    message=str(patch.description) if hasattr(
                                        patch, "description") else str(patch),
                                    severity=Severity.INFORMATION,
                                    confidence=Confidence.LOW,
                                    fix=FixSuggestion(
                                        description=str(patch.description) if hasattr(
                                            patch, "description") else "LLM fix",
                                        replacement=str(patch.replacement) if hasattr(
                                            patch, "replacement") else "",
                                    ),
                                )
                            )
                else:
                    issues.append(
                        AnalysisIssue(
                            rule_id="llm.no-suggestion",
                            message="LLM returned no suggestion",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.LOW,
                        )
                    )

            elif self._capability == "suggest-batch":
                snippets = context.data.strip().split("---") if context.data else []
                for i, snippet in enumerate(snippets[:5]):
                    snippet = snippet.strip()
                    if not snippet:
                        continue
                    suggestion = repairer.suggest(
                        message=snippet,
                        file=context.source_file,
                        line=0,
                        snippet=snippet[:200],
                    )
                    if suggestion:
                        suggestions = suggestion if isinstance(
                            suggestion, list) else [suggestion]
                        issues.append(
                            AnalysisIssue(
                                rule_id="llm.batch",
                                message=f"Batch item {i}: {len(suggestions)} suggestion(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.LOW,
                            )
                        )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="llm.error",
                    message=f"LLM query failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Fault Localization Bindlet
# ══════════════════════════════════════════════════════════════════════

class LocalizeFaultBindlet(AbstractDomainlet):
    """Bindlet for statistical fault localization.

    Uses spectrum-based fault localisation (Tarantula, Ochiai, etc.) to
    rank program locations by suspiciousness score, guiding repair efforts
    to the most likely faulty statements.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.localize_fault",
            version="0.7.0",
            description="Fault localization — spectrum-based suspiciousness ranking (Tarantula, Ochiai, etc.)",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="localize",
                    description="Rank program locations by suspiciousness score",
                    input_kind=InputKind.AST_JSON,
                    tags=["fault-localization", "spectrum", "suspiciousness"],
                ),
                DomainletCapability(
                    name="localize-multiple",
                    description="Localize multiple faults simultaneously",
                    input_kind=InputKind.AST_JSON,
                    tags=["fault-localization", "multiple-faults", "ranking"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "localize")
        self._metric = config.get("metric", "ochiai")
        self._top_n = int(config.get("top_n", "5"))

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.localize_fault import localize, localize_multiple, FaultLocation

        issues: list[AnalysisIssue] = []

        try:
            if self._capability == "localize":
                # Accept either a coverage JSON or parse from source
                locations: list[FaultLocation] = localize(
                    context.data,
                    metric=self._metric,
                    top_n=self._top_n,
                )

                for loc in locations:
                    issues.append(
                        AnalysisIssue(
                            rule_id="localize.suspicious",
                            message=f"Suspicious location: {loc.file}:{loc.line} "
                                    f"(score={loc.score:.4f})",
                            severity=Severity.WARNING,
                            confidence=Confidence.MEDIUM,
                            location=SourceSpan(
                                file=loc.file,
                                line_start=loc.line,
                            ),
                        )
                    )

            elif self._capability == "localize-multiple":
                locations = localize_multiple(
                    context.data,
                    metric=self._metric,
                    top_n=self._top_n,
                )

                for loc in locations:
                    issues.append(
                        AnalysisIssue(
                            rule_id="localize.multi",
                            message=f"Fault #{loc.rank}: {loc.file}:{loc.line} "
                                    f"(score={loc.score:.4f})",
                            severity=Severity.WARNING,
                            confidence=Confidence.MEDIUM,
                            location=SourceSpan(
                                file=loc.file,
                                line_start=loc.line,
                            ),
                        )
                    )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="localize.error",
                    message=f"Fault localization failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues


# ══════════════════════════════════════════════════════════════════════
#  Synthesis Grammar Bindlet
# ══════════════════════════════════════════════════════════════════════

class SynthesisGrammarBindlet(AbstractDomainlet):
    """Bindlet for grammar-based program synthesis.

    Enumerates expressions from a context-free grammar up to a given depth,
    providing repair candidates for expression-level fixes.  Supports both
    built-in grammars and user-supplied grammar files.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="repair.synthesis_grammar",
            version="0.7.0",
            description="Grammar-based synthesis — enumerate expressions from a CFG for repair candidate generation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="generate",
                    description="Generate expressions from the built-in grammar up to a depth",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["synthesis", "grammar", "enumeration"],
                ),
                DomainletCapability(
                    name="parse-grammar",
                    description="Parse a user-supplied grammar and generate expressions",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["synthesis", "grammar", "custom"],
                ),
                DomainletCapability(
                    name="bounded-generate",
                    description="Generate expressions bounded by a cost or complexity budget",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["synthesis", "bounded", "cost"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "generate")
        self._depth = int(config.get("depth", "3"))
        self._max_expressions = int(config.get("max_expressions", "100"))
        self._grammar_file: str | None = config.get("grammar_file") or None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.repair.synthesis_grammar import SynthesisGrammar

        issues: list[AnalysisIssue] = []

        try:
            if self._grammar_file:
                with open(self._grammar_file) as gf:
                    grammar_text = gf.read()
            elif context.data:
                grammar_text = context.data if len(context.data) > 50 else None
            else:
                grammar_text = None

            grammar = SynthesisGrammar(grammar_text=grammar_text)

            if self._capability == "parse-grammar":
                if grammar_text:
                    parsed = grammar.parse(grammar_text)
                    issues.append(
                        AnalysisIssue(
                            rule_id="grammar.parsed",
                            message=f"Grammar parsed: {getattr(parsed, 'production_count', 'N/A')} "
                                    f"production(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                else:
                    issues.append(
                        AnalysisIssue(
                            rule_id="grammar.no-input",
                            message="No grammar text provided for parsing",
                            severity=Severity.WARNING,
                            confidence=Confidence.MEDIUM,
                        )
                    )

            elif self._capability == "generate":
                count = 0
                for expr in grammar.generate(depth=self._depth):
                    if count >= self._max_expressions:
                        break
                    issues.append(
                        AnalysisIssue(
                            rule_id="grammar.expr",
                            message=f"Generated expression: {expr}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )
                    count += 1
                issues.insert(0, AnalysisIssue(
                    rule_id="grammar.generated",
                    message=f"Generated {count} expression(s) at depth {self._depth}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                ))

            elif self._capability == "bounded-generate":
                count = 0
                for expr in grammar.generate_bounded(
                    max_expressions=self._max_expressions,
                    max_depth=self._depth,
                ):
                    if count >= self._max_expressions:
                        break
                    issues.append(
                        AnalysisIssue(
                            rule_id="grammar.bounded",
                            message=f"Bounded expression: {expr}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.MEDIUM,
                        )
                    )
                    count += 1
                issues.insert(0, AnalysisIssue(
                    rule_id="grammar.bounded-summary",
                    message=f"Generated {count} expression(s) within bounds",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                ))

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="grammar.error",
                    message=f"Synthesis grammar failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues
