"""DomCC-facing bindlet for the CCWeave compiler pipeline."""

from __future__ import annotations

import json
from typing import Any, Mapping

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


def _require_ccweave():
    """Import the legacy ccweave pipeline lazily.

    ``domainlet.ccweave`` is not shipped in this tree, so the import must
    not run at module load time; otherwise the whole module (including the
    fekern-based DomCC frontend below) becomes unimportable.
    """
    try:
        from domainlet import ccweave as _ccweave
    except ModuleNotFoundError as exc:
        raise BindletError(
            BindletErrorCode.NOT_FOUND,
            "legacy domainlet.ccweave pipeline is unavailable",
            "CCWeaveBindlet",
        ) from exc
    return _ccweave


def _payload(context: AnalysisContext) -> Mapping[str, Any]:
    data = context.data
    if isinstance(data, Mapping):
        return data
    if isinstance(data, str):
        try:
            value = json.loads(data)
        except json.JSONDecodeError as exc:
            raise BindletError(
                BindletErrorCode.INPUT_MALFORMED,
                f"invalid DomCC JSON: {exc.msg}",
                "CCWeaveBindlet",
            ) from exc
        if isinstance(value, Mapping):
            return value
    raise BindletError(
        BindletErrorCode.INPUT_MALFORMED,
        "DomCC input must be a JSON object or mapping",
        "CCWeaveBindlet",
    )


def _module_from_payload(payload: Mapping[str, Any]):
    _ccweave = _require_ccweave()
    BasicBlock, IRBuilder, SSAFunction, SSAModule = (
        _ccweave.BasicBlock, _ccweave.IRBuilder, _ccweave.SSAFunction,
        _ccweave.SSAModule)

    module = SSAModule()
    for function_data in payload.get("functions", []):
        function = SSAFunction(str(function_data.get("name", "anonymous")))
        for block_data in function_data.get("blocks", []):
            block = BasicBlock(str(block_data.get("label", "entry")))
            function.blocks.append(block)
            builder = IRBuilder(block)
            for inst in block_data.get("instructions", []):
                opcode = str(inst.get("opcode", ""))
                if opcode == "const":
                    builder.build_const(inst.get("value", 0))
                else:
                    created = builder.build_const(inst.get("value", 0))
                    created.opcode = opcode
                    created.operands = list(inst.get("operands", []))
                    created.name = str(inst.get("name", created.name))
            if not block.instructions:
                builder.build_ret(builder.build_const(0))
        module.functions.append(function)
    return module


class DomCCFrontendBindlet(AbstractDomainlet):
    """Compatibility bindlet for clients of the former DomCC frontend.

    Delegates serialization to Domgate. Native DomCC now calls Domgate's
    C++ API directly; existing Python bindlet consumers keep this lifecycle.
    """

    _CAPABILITIES = {"parse"}

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="domcc.frontend",
            version="0.1.0",
            description="Domgate C parser/frontend compatibility bindlet",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    "parse", "Parse C source via Domgate",
                    InputKind.SOURCE_TEXT),
            ],
            config_schema={
                "capability": {"type": "string", "enum": sorted(self._CAPABILITIES)},
                "gate": {"type": "string", "default": "C"},
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        capability = config.get("capability", "parse")
        if capability not in self._CAPABILITIES:
            raise BindletError(BindletErrorCode.CONFIG_INVALID,
                               f"unknown capability: {capability}",
                               "DomCCFrontendBindlet")
        self._capability = capability
        self._gate = (config.get("gate", "C") or "C").strip() or "C"

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        gate = (context.options.get("gate", "") or "").strip() or getattr(
            self, "_gate", "C")
        source = context.data if isinstance(context.data, str) else ""
        filename = context.source_file or "<memory>"
        if not source and filename and filename != "<memory>":
            with open(filename, encoding="utf-8", errors="replace") as handle:
                source = handle.read()

        try:
            from domgate.native_bridge import parse_source
            parsed = parse_source(source, filename=filename, language=gate,
                                  preprocessed=True)
        except Exception as exc:
            raise BindletError(BindletErrorCode.EXECUTION_FAILED,
                               f"Domgate parse failed: {exc}",
                               "DomCCFrontendBindlet") from exc

        issues: list[AnalysisIssue] = [
            AnalysisIssue(
                "domcc.frontend.parsed",
                f"parsed {filename} via Domgate language {gate!r}",
                Severity.INFORMATION,
                Confidence.HIGH,
                extra={
                    "domgate_json": str(parsed.get("json", "")),
                    "error_count": str(parsed.get("error_count", 0)),
                    "warning_count": str(parsed.get("warning_count", 0)),
                },
            )
        ]
        try:
            import json as _json

            payload = _json.loads(parsed.get("json", "{}"))
            for diag in payload.get("diagnostics", []):
                severity = str(diag.get("severity", "error"))
                issues.append(AnalysisIssue(
                    "domcc.frontend.diagnostic",
                    str(diag.get("message", "parse diagnostic")),
                    Severity.ERROR if severity == "error" else Severity.WARNING,
                    Confidence.HIGH,
                ))
        except Exception:
            pass
        return issues


class CCWeaveBindlet(AbstractDomainlet):
    _CAPABILITIES = {"optimize", "codegen"}

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="ccweave",
            version="0.1.0",
            description="DomCC lowering, optimization, and code generation",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    "optimize", "Run SSA optimizations", InputKind.AST_JSON),
                DomainletCapability(
                    "codegen", "Emit target assembly", InputKind.AST_JSON),
            ],
            config_schema={
                "capability": {"type": "string", "enum": sorted(self._CAPABILITIES)},
                "target": {"type": "string", "default": "c"},
                "passes": {"type": "string", "default": "sccp,gvn,dce"},
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        capability = config.get("capability", "optimize")
        if capability not in self._CAPABILITIES:
            raise BindletError(BindletErrorCode.CONFIG_INVALID,
                               f"unknown capability: {capability}", "CCWeaveBindlet")
        target = config.get("target", "c").strip().lower()
        passes = [item.strip().lower() for item in config.get(
            "passes", "sccp,gvn,dce").split(",") if item.strip()]
        self._capability = capability
        self._target = target
        self._passes = passes

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        try:
            _ccweave = _require_ccweave()
            module = _module_from_payload(_payload(context))
            if self._capability == "optimize":
                pm = _ccweave.PassManager()
                for name in self._passes:
                    _pass_cls = {"sccp": _ccweave.SCCPPass, "gvn": _ccweave.GVNPass,
                                 "dce": _ccweave.DCEPass, "mem2reg": _ccweave.Mem2RegPass,
                                 "licm": _ccweave.LICMPass}
                    pm.add_pass(_pass_cls[name]())
                pm.run(module)
                return [AnalysisIssue(
                    "domcc.optimize",
                    "optimized",
                    Severity.INFORMATION,
                    Confidence.HIGH,
                    extra={"module_json": context.data if isinstance(context.data, str) else "{}"},
                )]
            output = _ccweave.CodegenPipeline(self._target).run(module)
            assembly = "\n".join(
                f"{fname}:" + ("\n" if items else "")
                + "\n".join(f"  {item.opcode} {', '.join(map(str, item.operands))}".rstrip()
                            for item in items)
                for fname, items in output.items()
            )
            return [AnalysisIssue(
                "domcc.codegen",
                assembly,
                Severity.INFORMATION,
                Confidence.HIGH,
                extra={"assembly": assembly},
            )]
        except BindletError:
            raise
        except Exception as exc:
            raise BindletError(BindletErrorCode.EXECUTION_FAILED,
                               str(exc), "CCWeaveBindlet") from exc
