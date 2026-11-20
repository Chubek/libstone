"""
reporting_bindlets — Bindlet wrappers for reporting.* domainlets.

Exposes the checker framework (NullDerefChecker, BufferOverflowChecker, etc.)
and plus_reporter utilities through the unified AbstractDomainlet interface.
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


def _parse_context_dumpfile(context: AnalysisContext) -> str:
    """Write context data to a temp file and return the path.

    Caller is responsible for cleanup.
    """
    fd, path = tempfile.mkstemp(suffix=".dump")
    with os.fdopen(fd, "w") as f:
        f.write(context.data)
    return path


def _diag_to_issue(diag: Any) -> AnalysisIssue:
    """Convert a domainlet.reporting.checkers Diagnostic to an AnalysisIssue."""
    sev_map = {
        "error": Severity.ERROR,
        "warning": Severity.WARNING,
        "style": Severity.STYLE,
        "performance": Severity.INFORMATION,
        "portability": Severity.INFORMATION,
    }
    conf_map = {
        "high": Confidence.HIGH,
        "medium": Confidence.MEDIUM,
        "low": Confidence.LOW,
    }

    severity = sev_map.get(
        str(getattr(diag, "severity", "warning")).lower(), Severity.WARNING)
    confidence = conf_map.get(
        str(getattr(diag, "confidence", "medium")).lower(), Confidence.MEDIUM)

    location = SourceSpan(
        file=getattr(diag, "file", ""),
        line_start=getattr(diag, "line", 0),
        col_start=getattr(diag, "column", 0),
    )

    return AnalysisIssue(
        rule_id=getattr(diag, "error_id", "checker.unknown"),
        message=getattr(diag, "message", str(diag)),
        severity=severity,
        confidence=confidence,
        location=location,
        cwe_ids=[getattr(diag, "cwe_id", 0)] if hasattr(
            diag, "cwe_id") and diag.cwe_id else [],
    )


# ══════════════════════════════════════════════════════════════════════
#  Checkers Bindlet — run the full checker suite
# ══════════════════════════════════════════════════════════════════════

class CheckersBindlet(AbstractDomainlet):
    """Bindlet for running Domgate-style checkers (null deref, buffer overflow, etc.).

    Bridges the CheckerRunner / CheckerRegistry framework into the bindlet
    lifecycle so Domfix can invoke individual or batched checkers.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="reporting.checkers",
            version="0.6.0",
            description="Domgate-style checker suite — null deref, buffer overflow, use-after-free, division-by-zero, etc.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="run-checkers",
                    description="Run all registered checkers and return diagnostics as issues",
                    input_kind=InputKind.AST_JSON,
                    tags=["checker", "diagnostic", "bug-detection"],
                ),
                DomainletCapability(
                    name="run-specific",
                    description="Run only specified checkers by name",
                    input_kind=InputKind.AST_JSON,
                    tags=["checker", "selective", "diagnostic"],
                ),
                DomainletCapability(
                    name="list-checkers",
                    description="List all registered checker names without running them",
                    input_kind=InputKind.AST_JSON,
                    tags=["checker", "discovery", "registry"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "run-checkers")
        checker_names = config.get("checkers", "")
        self._checker_names: list[str] | None = (
            [n.strip() for n in checker_names.split(",") if n.strip()]
            if checker_names
            else None
        )
        self._suppress = config.get("suppress", "")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.reporting.checkers import (
            CheckerRunner,
            CheckerRegistry,
            SuppressionManager,
            _DEFAULT_REGISTRY,
        )

        if self._capability == "list-checkers":
            registry: Any = _DEFAULT_REGISTRY
            checker_names = list(registry._checkers.keys()) if hasattr(
                registry, "_checkers") else []
            return [
                AnalysisIssue(
                    rule_id="checkers.list",
                    message=f"Available checker: {name}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
                for name in sorted(checker_names)
            ]

        dump_path = _parse_context_dumpfile(context)
        try:
            from domainlet.domgate_shim import parsedump
            data = parsedump(dump_path)
        except ImportError:
            from domainlet.domgate_feed import parsedump
            data = parsedump(dump_path)

        sm = SuppressionManager()
        if self._suppress:
            for eid in self._suppress.split(","):
                eid = eid.strip()
                if eid:
                    sm.add_global_suppression(eid)

        runner = CheckerRunner(suppressions=sm)

        results = runner.run_all_configurations(
            data, checkers=self._checker_names
        )

        issues: list[AnalysisIssue] = []
        for diag in results.diagnostics:
            if not sm.is_suppressed(diag):
                issues.append(_diag_to_issue(diag))

        os.unlink(dump_path)
        return issues


# ══════════════════════════════════════════════════════════════════════
#  PlusReporter Bindlet — advanced reporting utilities
# ══════════════════════════════════════════════════════════════════════

class PlusReporterBindlet(AbstractDomainlet):
    """Bindlet for the plus_reporter — enhanced diagnostic formatting and aggregation."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="reporting.plus_reporter",
            version="0.6.0",
            description="Plus Reporter — advanced diagnostic formatting, aggregation, and CWE classification",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="aggregate",
                    description="Aggregate diagnostics by category, severity, and CWE",
                    input_kind=InputKind.AST_JSON,
                    tags=["reporting", "aggregation", "metrics"],
                ),
                DomainletCapability(
                    name="format-report",
                    description="Produce a formatted report from a set of diagnostics",
                    input_kind=InputKind.AST_JSON,
                    tags=["reporting", "formatting", "output"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "aggregate")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.reporting.plus_reporter import (
            AggregateReporter,
            format_summary,
        )

        # First, run checkers to get diagnostics
        dump_path = _parse_context_dumpfile(context)
        try:
            from domainlet.reporting.checkers import CheckerRunner, SuppressionManager
            from domainlet.domgate_feed import parsedump

            data = parsedump(dump_path)
            runner = CheckerRunner(suppressions=SuppressionManager())
            results = runner.run_all_configurations(data)
            diagnostics = results.diagnostics
        finally:
            os.unlink(dump_path)

        issues: list[AnalysisIssue] = []

        if self._capability == "aggregate":
            reporter = AggregateReporter(diagnostics)
            summary = reporter.summarize()
            if isinstance(summary, dict):
                for category, count in summary.items():
                    issues.append(
                        AnalysisIssue(
                            rule_id="plus_reporter.aggregate",
                            message=f"Category '{category}': {count} finding(s)",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
            else:
                issues.append(
                    AnalysisIssue(
                        rule_id="plus_reporter.aggregate",
                        message=str(summary),
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        elif self._capability == "format-report":
            formatted = format_summary(diagnostics)
            issues.append(
                AnalysisIssue(
                    rule_id="plus_reporter.report",
                    message=str(formatted),
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                )
            )

        return issues
