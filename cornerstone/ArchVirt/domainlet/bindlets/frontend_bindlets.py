"""
frontend_bindlets — Bindlet wrappers for the domgate_feed domainlet.

Exposes parsedump, reporting, and CLI utilities through the unified
AbstractDomainlet interface, enabling Domfix to parse C/C++ dump files
and interact with the domgate ecosystem.
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


def _write_dump(context: AnalysisContext) -> str:
    fd, path = tempfile.mkstemp(suffix=".dump")
    with os.fdopen(fd, "w") as f:
        f.write(context.data)
    return path


# ══════════════════════════════════════════════════════════════════════
#  Frontend Feed Bindlet
# ══════════════════════════════════════════════════════════════════════

class FrontendFeedBindlet(AbstractDomainlet):
    """Bindlet for the domgate_feed — the unified C/C++ AST frontend.

    Provides parsedump (AST deserialization), error reporting, and
    summary generation through the bindlet lifecycle.  Acts as the
    entry point for all downstream domainlets that consume domgate
    dump data.
    """

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="domgate_feed",
            version="0.7.0",
            description="Unified C/C++ frontend — parse domgate dump files, report errors, "
                        "and provide CLI utilities for linter integration",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="parse-dump",
                    description="Parse a domgate dump file and return structured AST data",
                    input_kind=InputKind.AST_JSON,
                    output_kinds=["ast"],
                    tags=["frontend", "parsedump", "ast"],
                ),
                DomainletCapability(
                    name="report-error",
                    description="Emit a domgate-compatible error message",
                    input_kind=InputKind.TOKEN_STREAM,
                    tags=["frontend", "reporting", "error"],
                ),
                DomainletCapability(
                    name="dump-summary",
                    description="Generate a summary report for a dump file",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "summary", "report"],
                ),
                DomainletCapability(
                    name="extract-tokens",
                    description="Extract all tokens from a dump file as structured data",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "tokens", "extraction"],
                ),
                DomainletCapability(
                    name="language-detect",
                    description="Detect the language (C/C++) of source files",
                    input_kind=InputKind.AST_JSON,
                    tags=["frontend", "language", "detection"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "parse-dump")
        valid_caps = {
            "parse-dump", "report-error", "dump-summary",
            "extract-tokens", "language-detect",
        }
        if self._capability not in valid_caps:
            from domainlet.bindlet_base import BindletError, BindletErrorCode
            raise BindletError(
                code=BindletErrorCode.CONFIG_INVALID,
                message=f"unknown capability '{self._capability}'; choose from {sorted(valid_caps)}",
                source="FrontendFeedBindlet",
            )
        self._severity = config.get("severity", "warning")
        self._error_id = config.get("error_id", "frontend.report")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet import domgate_feed as domgatedata

        issues: list[AnalysisIssue] = []

        try:
            if self._capability == "parse-dump":
                dump_path = _write_dump(context)
                try:
                    data = domgatedata.parsedump(dump_path)
                    config_count = len(data.configurations) if hasattr(
                        data, "configurations") else 0

                    issues.append(
                        AnalysisIssue(
                            rule_id="frontend.parsed",
                            message=f"Parsed dump: {config_count} configuration(s) loaded",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )

                    for cfg in getattr(data, "configurations", [])[:5]:
                        token_count = len(cfg.tokenlist) if hasattr(
                            cfg, "tokenlist") else 0
                        func_count = len(cfg.functions) if hasattr(
                            cfg, "functions") else 0
                        issues.append(
                            AnalysisIssue(
                                rule_id="frontend.config",
                                message=f"Configuration: {token_count} token(s), "
                                        f"{func_count} function(s)",
                                severity=Severity.INFORMATION,
                                confidence=Confidence.HIGH,
                            )
                        )
                finally:
                    os.unlink(dump_path)

            elif self._capability == "report-error":
                # Create a minimal token-like object for reporting
                class _FauxToken:
                    file = context.source_file
                    linenr = 0
                    column = 0
                    str = ""

                domgatedata.reportError(
                    _FauxToken(),
                    self._severity,
                    context.data or "No message provided",
                    "FrontendFeedBindlet",
                    self._error_id,
                )
                issues.append(
                    AnalysisIssue(
                        rule_id=self._error_id,
                        message=context.data or "Error reported via domgate protocol",
                        severity=Severity.WARNING,
                        confidence=Confidence.MEDIUM,
                        location=SourceSpan(file=context.source_file),
                    )
                )

            elif self._capability == "dump-summary":
                dump_path = _write_dump(context)
                try:
                    domgatedata.reportSummary(
                        dump_path,
                        "domgate_feed",
                        "Bindlet-driven summary",
                    )
                    issues.append(
                        AnalysisIssue(
                            rule_id="frontend.summary",
                            message=f"Summary generated for {context.source_file}",
                            severity=Severity.INFORMATION,
                            confidence=Confidence.HIGH,
                        )
                    )
                finally:
                    os.unlink(dump_path)

            elif self._capability == "extract-tokens":
                dump_path = _write_dump(context)
                try:
                    data = domgatedata.parsedump(dump_path)
                    total_tokens = 0
                    for cfg in getattr(data, "configurations", []):
                        for token in cfg.tokenlist:
                            total_tokens += 1
                            if total_tokens <= 20:
                                tok_str = getattr(token, "str", str(token))
                                tok_line = getattr(token, "linenr", 0)
                                issues.append(
                                    AnalysisIssue(
                                        rule_id="frontend.token",
                                        message=f"Token: '{tok_str}':{tok_line}",
                                        severity=Severity.INFORMATION,
                                        confidence=Confidence.HIGH,
                                        location=SourceSpan(
                                            file=getattr(token, "file", ""),
                                            line_start=tok_line,
                                            col_start=getattr(
                                                token, "column", 0),
                                        ),
                                    )
                                )
                    issues.insert(0, AnalysisIssue(
                        rule_id="frontend.tokens",
                        message=f"Extracted {total_tokens} token(s) total",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    ))
                finally:
                    os.unlink(dump_path)

            elif self._capability == "language-detect":
                ext = os.path.splitext(context.source_file)[
                    1].lower() if context.source_file else ""
                lang = context.language
                if not lang:
                    lang_map = {
                        ".c": "c", ".h": "c",
                        ".cpp": "cpp", ".cc": "cpp", ".cxx": "cpp", ".hpp": "cpp",
                        ".C": "cpp", ".H": "cpp",
                    }
                    lang = lang_map.get(ext, "unknown")

                issues.append(
                    AnalysisIssue(
                        rule_id="frontend.language",
                        message=f"Detected language: {lang} (from '{context.source_file}')",
                        severity=Severity.INFORMATION,
                        confidence=Confidence.HIGH,
                    )
                )

        except Exception as exc:
            issues.append(
                AnalysisIssue(
                    rule_id="frontend.error",
                    message=f"Frontend operation failed: {exc}",
                    severity=Severity.ERROR,
                    confidence=Confidence.LOW,
                )
            )

        return issues
