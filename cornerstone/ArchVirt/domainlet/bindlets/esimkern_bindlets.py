"""Bindlet wrappers for esimkern.* domainlet packages."""

from __future__ import annotations

import importlib
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
from domainlet.esimkern._catalog import ESIMKERN_CATALOG


def _payload(context: AnalysisContext) -> dict[str, Any]:
    value: Any = context.data
    if isinstance(value, dict):
        return dict(value)
    if isinstance(value, bytes):
        value = value.decode("utf-8")
    if value is None or value == "":
        return {}
    if not isinstance(value, str):
        raise ValueError("esimkern bindlet input must be a mapping or JSON object")
    decoded = json.loads(value)
    if not isinstance(decoded, dict):
        raise ValueError("esimkern bindlet input must decode to a JSON object")
    return dict(decoded)


def _resolve_module_name(base_package: str, module_name: str) -> str:
    value = module_name.strip().lstrip(".")
    if not value:
        raise ValueError("module name is required")
    if value.startswith(base_package):
        return value
    if value.startswith("domainlet."):
        return value
    return f"{base_package}.{value}"


def _module_records(scope_name: str, recursive: bool) -> list[dict[str, Any]]:
    root = "domainlet.esimkern"
    if scope_name == root:
        records: list[dict[str, Any]] = [{"name": root, "is_package": True}]
        for category, spec in ESIMKERN_CATALOG.items():
            category_name = f"{root}.{category}"
            records.append({"name": category_name, "is_package": True})
            if recursive:
                for stem in spec["modules"]:
                    records.append({"name": f"{category_name}.{stem}", "is_package": False})
        return records
    if scope_name.startswith(root + "."):
        rel = scope_name[len(root) + 1 :]
        parts = rel.split(".")
        if parts and parts[0] in ESIMKERN_CATALOG:
            category_name = f"{root}.{parts[0]}"
            if len(parts) == 1:
                records = [{"name": category_name, "is_package": True}]
                if recursive:
                    for stem in ESIMKERN_CATALOG[parts[0]]["modules"]:
                        records.append({"name": f"{category_name}.{stem}", "is_package": False})
                return records
            module_name = f"{category_name}." + ".".join(parts[1:])
            return [{"name": module_name, "is_package": False}]
    return [{"name": scope_name, "is_package": False}]


def _describe_module(module: Any) -> dict[str, Any]:
    exports = list(getattr(module, "__all__", []))
    if not exports:
        exports = [name for name in dir(module) if not name.startswith("_")]
    return {
        "name": module.__name__,
        "package": module.__package__ or "",
        "file": getattr(module, "__file__", ""),
        "is_package": hasattr(module, "__path__"),
        "exports": exports,
    }


class EsimPackageBindlet(AbstractDomainlet):
    bindlet_name = "esimkern"
    package_name = "domainlet.esimkern"
    description = "Electronics simulation kernel package discovery and validation"
    version = "1.0.0"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name=self.bindlet_name,
            version=self.version,
            description=self.description,
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name="list-modules",
                    description="List kernels under domainlet.esimkern",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["esim", "package", "discovery"],
                ),
                DomainletCapability(
                    name="describe-module",
                    description="Describe one esimkern module",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["esim", "package", "discovery"],
                ),
                DomainletCapability(
                    name="validate-imports",
                    description="Import every esimkern module and report failures",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["esim", "package", "discovery"],
                ),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._capability = config.get("capability", "list-modules")
        self._module = config.get("module") or None
        self._recursive = str(config.get("recursive", "true")).strip().lower() not in {"0", "false", "no", "off"}

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        capability = getattr(self, "_capability", "list-modules")
        module_filter = getattr(self, "_module", None)
        recursive = getattr(self, "_recursive", True)
        payload = _payload(context)

        scope_name = self.package_name if module_filter is None else _resolve_module_name(self.package_name, module_filter)
        requested_module = payload.get("module", scope_name)
        requested_module = _resolve_module_name(scope_name, str(requested_module))

        if capability == "list-modules":
            modules = _module_records(scope_name, recursive)
            result = {"schema_version": 1, "package": self.package_name, "scope": scope_name, "module_count": len(modules), "modules": modules}
            message = f"listed {len(modules)} module(s) under {scope_name}"
            severity = Severity.INFORMATION
        elif capability == "describe-module":
            try:
                module = importlib.import_module(requested_module)
                result = {"schema_version": 1, "package": self.package_name, "scope": scope_name, "module": _describe_module(module)}
                message = f"described {requested_module}"
                severity = Severity.INFORMATION
            except ModuleNotFoundError as exc:
                result = {"schema_version": 1, "package": self.package_name, "scope": scope_name, "error": str(exc)}
                message = str(exc)
                severity = Severity.ERROR
        else:
            modules = _module_records(scope_name, recursive)
            failures: list[dict[str, str]] = []
            for record in modules:
                name = record["name"]
                try:
                    importlib.import_module(name)
                except Exception as exc:  # pragma: no cover
                    failures.append({"module": name, "error": str(exc)})
            result = {
                "schema_version": 1,
                "package": self.package_name,
                "scope": scope_name,
                "recursive": recursive,
                "valid": not failures,
                "module_count": len(modules),
                "modules": modules,
                "errors": failures,
            }
            message = f"validated {len(modules)} module(s) under {scope_name}" if not failures else f"{len(failures)} module import(s) failed under {scope_name}"
            severity = Severity.INFORMATION if not failures else Severity.ERROR

        return [
            AnalysisIssue(
                rule_id=f"{self.bindlet_name}.{capability}",
                message=message,
                severity=severity,
                confidence=Confidence.HIGH,
                extra={"result": json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=False)},
            )
        ]


class BaseEsimKernelBindlet(AbstractDomainlet):
    """Execute one of the language-neutral electronics simulation kernels."""

    _category = ""
    _kernel_module = ""

    def _kernel(self):
        return importlib.import_module(self._kernel_module).KERNEL

    def probe(self) -> DomainletInfo:
        kernel = self._kernel()
        return DomainletInfo(
            name=self._kernel_module,
            version="1.0.0",
            description=kernel.description,
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name=operation,
                    description=f"Run {operation} on {self._kernel_module}",
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["esim", self._category, kernel.algorithm],
                )
                for operation in kernel.operations
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        kernel = self._kernel()
        requested = config.get("capability", config.get("operation", kernel.operations[0]))
        if requested not in kernel.operations:
            raise ValueError(f"unknown operation {requested!r}; expected one of {kernel.operations}")
        self._operation = requested

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        payload = _payload(context)
        operation = getattr(self, "_operation", self._kernel().operations[0])
        result = self._kernel().run(payload, operation=operation)
        outcome = result["result"]
        ok = outcome["status"] == "ok"
        message = json.dumps(outcome["value"] if ok else outcome["diagnostics"], sort_keys=True)
        return [
            AnalysisIssue(
                rule_id=f"{self._kernel_module}.{operation}",
                message=message,
                severity=Severity.INFORMATION if ok else Severity.ERROR,
                confidence=Confidence.HIGH,
                extra={"result": json.dumps(result, sort_keys=True, separators=(",", ":"), ensure_ascii=False)},
            )
        ]


AbstractDomainlet._registry.pop(f"{__name__}.BaseEsimKernelBindlet", None)


def _make_kernel_bindlet(category: str, stem: str) -> type[BaseEsimKernelBindlet]:
    module_name = f"domainlet.esimkern.{category}.{stem}"
    return type(
        f"{category.title().replace('_', '')}{stem.title().replace('_', '')}Bindlet",
        (BaseEsimKernelBindlet,),
        {"_category": category, "_kernel_module": module_name},
    )


ESIMKERN_BINDLETS: dict[str, type[BaseEsimKernelBindlet]] = {}
for _category, _spec in ESIMKERN_CATALOG.items():
    for _stem in _spec["modules"]:
        ESIMKERN_BINDLETS[f"{_category}.{_stem}"] = _make_kernel_bindlet(_category, _stem)


def get_esimkern_bindlet(name: str) -> BaseEsimKernelBindlet:
    key = name.removeprefix("domainlet.esimkern.")
    if key in ESIMKERN_CATALOG:
        key = f"{key}.{ESIMKERN_CATALOG[key]['modules'][0]}"
    try:
        return ESIMKERN_BINDLETS[key]()
    except KeyError as exc:
        raise ValueError(f"unknown esimkern bindlet {name!r}") from exc


__all__ = [
    "BaseEsimKernelBindlet",
    "EsimPackageBindlet",
    "ESIMKERN_BINDLETS",
    "get_esimkern_bindlet",
    *[bindlet.__name__ for bindlet in ESIMKERN_BINDLETS.values()],
]
