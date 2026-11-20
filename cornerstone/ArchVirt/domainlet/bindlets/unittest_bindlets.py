"""Bindlet adapter for :mod:`domainlet.unittest`.

The adapter gives Domfix (and dompybind hosts) a small, structured entry point
for discovering and running Python unittest kernels.  Test failures are
returned as ``AnalysisIssue`` records so they can be consumed by the normal
reporting pipeline.
"""
from __future__ import annotations

import importlib
from pathlib import Path
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
    InputKind,
    Severity,
)


class UnittestBindlet(AbstractDomainlet):
    """Run and report ``domainlet.unittest`` suites."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="unittest",
            version="0.1.0",
            description="Test discovery and execution kernels",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="run",
                    description="Discover and execute tests from a module or path",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "test-results"],
                    tags=["testing", "unittest", "pytest"],
                ),
                DomainletCapability(
                    name="discover",
                    description="Discover tests without executing them",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["issues", "test-results"],
                    tags=["testing", "discovery"],
                ),
            ],
            config_schema={
                "capability": "run|discover",
                "module": "importable module name or .py path",
                "pattern": "discovery pattern (default: test*.py)",
                "start_dir": "discovery directory (default: .)",
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        capability = config.get("capability", "run").lower()
        if capability not in {"run", "discover"}:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown capability {capability!r}; expected 'run' or 'discover'",
                "UnittestBindlet",
            )
        self._capability = capability
        self._module = config.get("module") or None
        self._start_dir = config.get("start_dir", ".")
        self._pattern = config.get("pattern", "test*.py")

    @staticmethod
    def _module_from_data(data: Any, configured: str | None) -> str | None:
        if configured:
            return configured
        if isinstance(data, dict):
            value = data.get("module") or data.get("path")
            return str(value) if value else None
        if isinstance(data, (str, Path)):
            value = str(data)
            # A source blob is not importable; an existing .py path is.
            return value if value.endswith(".py") or "." in value else None
        return None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.unittest.loader import TestLoader

        loader = TestLoader()
        module_name = self._module_from_data(
            getattr(context, "data", None), self._module)
        try:
            if module_name:
                if module_name.endswith(".py") and Path(module_name).exists():
                    import sys
                    path = str(Path(module_name).resolve())
                    parent = str(Path(path).parent)
                    if parent not in sys.path:
                        sys.path.insert(0, parent)
                    module_name = Path(path).stem
                suite = loader.load_module(
                    importlib.import_module(module_name))
            else:
                suite = loader.discover(self._start_dir, self._pattern)
        except Exception as exc:
            raise BindletError(
                BindletErrorCode.EXECUTION_FAILED,
                f"unable to load tests: {exc}",
                "UnittestBindlet",
                detail=repr(exc),
            ) from exc

        tests = list(suite)
        if self._capability == "discover":
            return [
                AnalysisIssue(
                    rule_id="unittest.discovered",
                    message=f"Discovered test: {getattr(test, 'id', lambda: str(test))()}",
                    severity=Severity.INFORMATION,
                    confidence=Confidence.HIGH,
                    extra={"outcome": "discovered"},
                )
                for test in tests
            ]

        issues: list[AnalysisIssue] = []
        try:
            results = suite.run()
        except Exception as exc:
            raise BindletError(
                BindletErrorCode.EXECUTION_FAILED,
                f"test execution failed: {exc}",
                "UnittestBindlet",
                detail=repr(exc),
            ) from exc
        for result in results:
            if result.outcome == "passed":
                continue
            severity = Severity.INFORMATION if result.outcome in {
                "skipped", "xfail"} else Severity.ERROR
            issues.append(
                AnalysisIssue(
                    rule_id=f"unittest.{result.outcome}",
                    message=f"{result.name}: {result.error or result.outcome}",
                    severity=severity,
                    confidence=Confidence.HIGH,
                    extra={
                        "outcome": result.outcome,
                        "duration": f"{result.duration:.6f}",
                        **({"traceback": result.traceback} if result.traceback else {}),
                    },
                )
            )
        return issues


UnittestDomainlet = UnittestBindlet

__all__ = ["UnittestBindlet", "UnittestDomainlet"]
