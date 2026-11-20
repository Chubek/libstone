"""
fekern_bindlets — Bindlet wrappers for fekern.* domainlets.

Exposes AST, gate, rewrite, semantic, and validation frontend compiler
kernels through the unified AbstractDomainlet interface.
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
)


# ═══════════════════════════════════════════════════════════════════
#  Base Fekern Bindlet
# ═══════════════════════════════════════════════════════════════════

class BaseFekernBindlet(AbstractDomainlet):
    """Shared base for all fekern bindlets."""

    _kernel_module: str = ""
    _category: str = "fekern"

    def _get_kernel(self):
        import importlib
        return importlib.import_module(self._kernel_module).KERNEL

    def _run_operation(self, operation: str, payload: dict[str, Any]) -> dict[str, Any]:
        kernel = self._get_kernel()
        result = kernel.run(payload, operation=operation)
        if result["result"]["status"] != "ok":
            diags = result["result"].get("diagnostics", [])
            msg = "; ".join(d.get("message", "unknown") for d in diags)
            raise RuntimeError(f"{self._kernel_module}: {msg}")
        return result["result"]["value"]


# ═══════════════════════════════════════════════════════════════════
#  AST Bindlets
# ═══════════════════════════════════════════════════════════════════

class ASTSchemaBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.ast.schema"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.ast.schema",
            version="1.0.0",
            description="Define, construct, and validate language-specific AST shapes",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="describe_schema",
                    description="Describe the AST schema for a language",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "schema"],
                ),
                DomainletCapability(
                    name="build_node",
                    description="Build an AST node from a schema definition",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "build"],
                ),
                DomainletCapability(
                    name="validate_ast",
                    description="Validate an AST against its schema",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "validation"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "describe_schema")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.ast.schema.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class ASTVisitorBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.ast.visitor"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.ast.visitor",
            version="1.0.0",
            description="Traverse and transform language ASTs with stable kind dispatch",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="walk",
                    description="Walk the AST and yield nodes",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "traversal"],
                ),
                DomainletCapability(
                    name="collect_kinds",
                    description="Collect all AST node kinds",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "kinds"],
                ),
                DomainletCapability(
                    name="count_nodes",
                    description="Count AST nodes by kind",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "ast", "count"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "walk")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.ast.visitor.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Gate Bindlets
# ═══════════════════════════════════════════════════════════════════

class GateSelectionBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.gate.selection"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.gate.selection",
            version="1.0.0",
            description="Define and select a Domgate parser gate explicitly",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="list_gates",
                    description="List available Domgate parser gates",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["frontend", "gate", "discovery"],
                ),
                DomainletCapability(
                    name="select_gate",
                    description="Select a specific Domgate parser gate",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["frontend", "gate", "selection"],
                ),
                DomainletCapability(
                    name="parse_source",
                    description="Parse source text through a selected gate",
                    input_kind=InputKind.SOURCE_TEXT,
                    tags=["frontend", "gate", "parsing"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "list_gates")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.gate.selection.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Rewrite Bindlets
# ═══════════════════════════════════════════════════════════════════

class RewriteSystemBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.rewrite.system"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.rewrite.system",
            version="1.0.0",
            description="Apply guarded structural rewrite rules to frontend ASTs",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="validate_rules",
                    description="Validate rewrite rules for correctness",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "rewrite", "rules"],
                ),
                DomainletCapability(
                    name="apply_once",
                    description="Apply rewrite rules once to an AST",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "rewrite", "apply"],
                ),
                DomainletCapability(
                    name="rewrite_fixpoint",
                    description="Apply rewrite rules to a fixpoint",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "rewrite", "fixpoint"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "rewrite_fixpoint")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.rewrite.system.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TermRewriteBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.rewrite.term_rewrite"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.rewrite.term_rewrite",
            version="1.0.0",
            description="Term-rewriting engine: pattern-match subterms against rewrite rules and apply substitutions to a fixpoint",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="match_pattern",
                    description="Match a pattern against a term DAG",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "term", "pattern"],
                ),
                DomainletCapability(
                    name="apply_rewrite",
                    description="Apply a rewrite rule to a term",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "term", "rewrite"],
                ),
                DomainletCapability(
                    name="normalize_terms",
                    description="Normalize terms to canonical form",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "term", "normalize"],
                ),
                DomainletCapability(
                    name="reduce_fixpoint",
                    description="Reduce terms to a fixpoint",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "term", "fixpoint"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "reduce_fixpoint")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.rewrite.term_rewrite.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Semantic Bindlets
# ═══════════════════════════════════════════════════════════════════

class NameResolutionBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.name_resolution"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.name_resolution",
            version="1.0.0",
            description="Resolve identifiers and attach bindings",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="resolve_names",
                    description="Resolve identifiers to their declarations",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "name"],
                ),
                DomainletCapability(
                    name="bind_symbols",
                    description="Bind resolved symbols to the symbol table",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "symbols"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "resolve_names")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.name_resolution.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TypeCheckBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.type_check"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.type_check",
            version="1.0.0",
            description="Infer and validate expression and declaration types",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer_types",
                    description="Infer types for expressions and declarations",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "type"],
                ),
                DomainletCapability(
                    name="check_constraints",
                    description="Check type constraints on the AST",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "constraints"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_constraints")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.type_check.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class ScopeAnalysisBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.scope_analysis"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.scope_analysis",
            version="1.0.0",
            description="Track lexical scopes and nesting rules",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="enter_scope",
                    description="Enter a new lexical scope",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "scope"],
                ),
                DomainletCapability(
                    name="leave_scope",
                    description="Leave the current lexical scope",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "scope"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "enter_scope")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.scope_analysis.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class SymbolBindingBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.symbol_binding"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.symbol_binding",
            version="1.0.0",
            description="Attach declarations to their resolved symbols",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="bind_symbols",
                    description="Bind symbols to declarations",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "binding"],
                ),
                DomainletCapability(
                    name="annotate_definitions",
                    description="Annotate definitions with symbol information",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "annotation"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "bind_symbols")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.symbol_binding.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class ConstantEvaluationBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.constant_evaluation"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.constant_evaluation",
            version="1.0.0",
            description="Evaluate compile-time constants and literals",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="fold_constants",
                    description="Fold constant expressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "constant"],
                ),
                DomainletCapability(
                    name="evaluate_expr",
                    description="Evaluate a compile-time expression",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "evaluation"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "fold_constants")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.constant_evaluation.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class OverloadResolutionBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.overload_resolution"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.overload_resolution",
            version="1.0.0",
            description="Choose the best overload for each call site",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="resolve_overloads",
                    description="Resolve overloaded function calls",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "overload"],
                ),
                DomainletCapability(
                    name="select_candidate",
                    description="Select the best candidate from overloads",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "candidate"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "resolve_overloads")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.overload_resolution.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TraitConformanceBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.trait_conformance"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.trait_conformance",
            version="1.0.0",
            description="Check trait/interface conformance for types",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_trait_impl",
                    description="Check that a type implements a trait",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "trait"],
                ),
                DomainletCapability(
                    name="collect_trait_bounds",
                    description="Collect all trait bounds on a type",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "bounds"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_trait_impl")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.trait_conformance.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class VarianceBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.variance"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.variance",
            version="1.0.0",
            description="Check type parameter variance (covariant, contravariant, invariant)",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer_variance",
                    description="Infer variance of type parameters",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "variance"],
                ),
                DomainletCapability(
                    name="check_variance",
                    description="Check variance constraints on type parameters",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "variance"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_variance")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.variance.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class DiagnosticEmissionBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.diagnostic_emission"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.diagnostic_emission",
            version="1.0.0",
            description="Emit structured diagnostics with source locations",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="emit_diagnostic",
                    description="Emit a structured diagnostic",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "diagnostic"],
                ),
                DomainletCapability(
                    name="collect_diagnostics",
                    description="Collect all diagnostics from analysis",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "diagnostic"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "collect_diagnostics")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.diagnostic_emission.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class EffectAnalysisBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.semantic.effect_analysis"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.semantic.effect_analysis",
            version="1.0.0",
            description="Summarize side effects and purity boundaries",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="infer_effects",
                    description="Infer side effects of expressions",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "effects"],
                ),
                DomainletCapability(
                    name="check_purity",
                    description="Check that a function is pure",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "semantic", "purity"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "infer_effects")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.semantic.effect_analysis.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Validation Bindlets
# ═══════════════════════════════════════════════════════════════════

class ScopingValidationBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.validation.scoping"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.validation.scoping",
            version="1.0.0",
            description="Validate scoping rules: resolve identifiers, detect shadowing, reject redeclaration",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="resolve_scopes",
                    description="Resolve scopes for all identifiers",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "scope"],
                ),
                DomainletCapability(
                    name="bind_decls",
                    description="Bind declarations to scopes",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "binding"],
                ),
                DomainletCapability(
                    name="detect_shadowing",
                    description="Detect identifier shadowing",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "shadowing"],
                ),
                DomainletCapability(
                    name="check_redeclaration",
                    description="Check for redeclaration in the same scope",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "redeclaration"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "detect_shadowing")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.validation.scoping.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class TypeMismatchBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.validation.type_mismatch"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.validation.type_mismatch",
            version="1.0.0",
            description="Validate type consistency: binary operators, call arguments, return statements",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="check_binop_types",
                    description="Check binary operator operand compatibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "types"],
                ),
                DomainletCapability(
                    name="check_call_args",
                    description="Check call-site argument arity and types",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "calls"],
                ),
                DomainletCapability(
                    name="check_return_type",
                    description="Check return statement type conformance",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "return"],
                ),
                DomainletCapability(
                    name="check_assignment",
                    description="Check assignment type compatibility",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "assignment"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_binop_types")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.validation.type_mismatch.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


class PatternExhaustivenessBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.validation.pattern_exhaustiveness"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.validation.pattern_exhaustiveness",
            version="1.0.0",
            description="Pattern-match exhaustiveness and redundancy checking",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="collect_variants",
                    description="Collect all variants of an algebraic data type",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "pattern"],
                ),
                DomainletCapability(
                    name="check_coverage",
                    description="Check pattern match coverage",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "coverage"],
                ),
                DomainletCapability(
                    name="warn_non_exhaustive",
                    description="Warn about non-exhaustive pattern matches",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "exhaustive"],
                ),
                DomainletCapability(
                    name="detect_redundant_arms",
                    description="Detect redundant pattern match arms",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "validation", "redundant"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "check_coverage")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.validation.pattern_exhaustiveness.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Pipeline Bindlets
# ═══════════════════════════════════════════════════════════════════

class PipelineBindlet(BaseFekernBindlet):
    _kernel_module = "domainlet.fekern.pipeline"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="fekern.pipeline",
            version="1.0.0",
            description="Compose parsing and language-authored frontend passes around an explicit gate",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="describe_pipeline",
                    description="Describe the frontend pipeline composition",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "pipeline", "description"],
                ),
                DomainletCapability(
                    name="run_pipeline",
                    description="Run the frontend pipeline end-to-end",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "pipeline", "execution"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "run_pipeline")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [
            AnalysisIssue(
                rule_id=f"fekern.pipeline.{self._capability}",
                message=json.dumps(value),
                severity=Severity.INFORMATION,
                confidence=Confidence.HIGH,
            )
        ]


# ═══════════════════════════════════════════════════════════════════
#  Registry
# ═══════════════════════════════════════════════════════════════════

FEKERN_BINDLETS: dict[str, type[BaseFekernBindlet]] = {
    "ast_schema": ASTSchemaBindlet,
    "ast_visitor": ASTVisitorBindlet,
    "gate_selection": GateSelectionBindlet,
    "rewrite_system": RewriteSystemBindlet,
    "term_rewrite": TermRewriteBindlet,
    "name_resolution": NameResolutionBindlet,
    "type_check": TypeCheckBindlet,
    "scope_analysis": ScopeAnalysisBindlet,
    "symbol_binding": SymbolBindingBindlet,
    "constant_evaluation": ConstantEvaluationBindlet,
    "overload_resolution": OverloadResolutionBindlet,
    "trait_conformance": TraitConformanceBindlet,
    "variance": VarianceBindlet,
    "diagnostic_emission": DiagnosticEmissionBindlet,
    "effect_analysis": EffectAnalysisBindlet,
    "scoping": ScopingValidationBindlet,
    "type_mismatch": TypeMismatchBindlet,
    "pattern_exhaustiveness": PatternExhaustivenessBindlet,
    "pipeline": PipelineBindlet,
}

class ExtraFekernBindlet(BaseFekernBindlet):
    """Generic adapter for newly added FEKERN kernels."""
    _kernel_module = ""
    def probe(self) -> DomainletInfo:
        kernel = self._get_kernel()
        return DomainletInfo(name=kernel.name, version="1.0.0", description=kernel.description,
            author="domweave", capabilities=[DomainletCapability(name=op, description=op,
            input_kind=InputKind.AST_JSON, tags=["frontend", kernel.category]) for op in kernel.operations])
    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", self._get_kernel().operations[0])
    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        value = self._run_operation(self._capability, {"data": context.data} if context.data else {})
        return [AnalysisIssue(rule_id=f"{self._get_kernel().name}.{self._capability}", message=json.dumps(value), severity=Severity.INFORMATION, confidence=Confidence.HIGH)]

for _name, _module in {
    "ast_normalization":"domainlet.fekern.ast.normalization", "ast_source_map":"domainlet.fekern.ast.source_map",
    "parsing_error_recovery":"domainlet.fekern.parsing.error_recovery", "parsing_token_stream":"domainlet.fekern.parsing.token_stream",
    "parsing_grammar_analysis":"domainlet.fekern.parsing.grammar_analysis", "parsing_incremental":"domainlet.fekern.parsing.incremental",
    "semantic_constraint_solver":"domainlet.fekern.semantic.constraint_solver", "semantic_generic_instantiation":"domainlet.fekern.semantic.generic_instantiation",
    "semantic_capture_analysis":"domainlet.fekern.semantic.capture_analysis", "semantic_module_resolution":"domainlet.fekern.semantic.module_resolution",
    "semantic_const_propagation":"domainlet.fekern.semantic.const_propagation", "validation_irrefutable_patterns":"domainlet.fekern.validation.irrefutable_patterns",
    "validation_cfg_validation":"domainlet.fekern.validation.cfg_validation", "gate_adapter_registry":"domainlet.fekern.gate.adapter_registry",
    "rewrite_rewrite_scheduler":"domainlet.fekern.rewrite.rewrite_scheduler"}.items():
    FEKERN_BINDLETS[_name] = type(f"{_name.title().replace('_','')}Bindlet", (ExtraFekernBindlet,), {"_kernel_module": _module})


def get_fekern_bindlet(name: str) -> BaseFekernBindlet:
    """Factory for creating fekern bindlets by name."""
    cls = FEKERN_BINDLETS.get(name)
    if cls is None:
        raise ValueError(f"Unknown fekern bindlet: {name!r}. Available: {sorted(FEKERN_BINDLETS)}")
    return cls()
