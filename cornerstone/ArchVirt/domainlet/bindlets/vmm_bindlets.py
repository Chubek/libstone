"""Shared helpers for the VMM package bindlets."""

from __future__ import annotations

import importlib
import inspect
import json
import pkgutil
from collections.abc import Mapping
from typing import Any, ClassVar

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

_CAPABILITIES = {
    "list-modules": "List importable modules in the package scope.",
    "describe-module": "Describe one module or package under the package scope.",
    "validate-imports": "Import every module in the package scope and report failures.",
}


def _payload(context: AnalysisContext) -> dict[str, Any]:
    value: Any = context.data
    if isinstance(value, Mapping):
        return dict(value)
    if isinstance(value, bytes):
        value = value.decode("utf-8")
    if value is None or value == "":
        return {}
    if not isinstance(value, str):
        raise ValueError("VMM bindlet input must be a JSON object or mapping")
    decoded = json.loads(value)
    if not isinstance(decoded, Mapping):
        raise ValueError("VMM bindlet input must decode to a JSON object")
    return dict(decoded)


def _as_bool(value: Any, *, default: bool = True) -> bool:
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value != 0
    text = str(value).strip().lower()
    if text in {"1", "true", "yes", "on"}:
        return True
    if text in {"0", "false", "no", "off"}:
        return False
    raise ValueError(f"invalid boolean value {value!r}")


def _resolve_module_name(base_package: str, module_name: str) -> str:
    value = module_name.strip().lstrip(".")
    if not value:
        raise ValueError("module name is required")
    if value == base_package or value.startswith(base_package + "."):
        return value
    if value.startswith("domainlet."):
        return value
    return f"{base_package}.{value}"


def _module_records(package_name: str, recursive: bool) -> list[dict[str, Any]]:
    package = importlib.import_module(package_name)
    records: list[dict[str, Any]] = [
        {
            "name": package.__name__,
            "is_package": hasattr(package, "__path__"),
        }
    ]
    if not hasattr(package, "__path__"):
        return records
    walker = pkgutil.walk_packages if recursive else pkgutil.iter_modules
    for module_info in walker(package.__path__, package.__name__ + "."):
        records.append(
            {
                "name": module_info.name,
                "is_package": module_info.ispkg,
            }
        )
    return records


def _describe_module(module: Any) -> dict[str, Any]:
    exports = list(getattr(module, "__all__", []))
    if not exports:
        exports = [name for name in dir(module) if not name.startswith("_")]
    path = [str(item) for item in getattr(module, "__path__", [])]
    return {
        "name": module.__name__,
        "package": module.__package__ or "",
        "file": getattr(module, "__file__", ""),
        "is_package": hasattr(module, "__path__"),
        "doc": inspect.getdoc(module) or "",
        "exports": exports,
        "path": path,
    }


class _VmmPackageBindlet(AbstractDomainlet):
    """Shared lifecycle for VMM category bindlets."""

    bindlet_name: ClassVar[str]
    package_name: ClassVar[str]
    description: ClassVar[str]
    version: ClassVar[str] = "1.0.0"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name=self.bindlet_name,
            version=self.version,
            description=self.description,
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name=name,
                    description=description,
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["vmm", self.bindlet_name.removeprefix("vmm.")],
                )
                for name, description in _CAPABILITIES.items()
            ],
            config_schema={
                "capability": {
                    "type": "string",
                    "default": "list-modules",
                    "enum": list(_CAPABILITIES),
                },
                "module": {
                    "type": "string",
                    "description": "Relative or absolute module path within the bindlet scope.",
                },
                "recursive": {
                    "type": "boolean",
                    "default": True,
                    "description": "Walk the package tree recursively when listing or validating.",
                },
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        unknown = set(config) - {"capability", "module", "recursive"}
        if unknown:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown configuration key(s): {', '.join(sorted(unknown))}",
                source=type(self).__name__,
            )
        capability = config.get("capability", "list-modules")
        if capability not in _CAPABILITIES:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown capability '{capability}'; choose from {sorted(_CAPABILITIES)}",
                source=type(self).__name__,
            )
        self._capability = capability
        module = config.get("module") or None
        self._module = module.strip() if module is not None and module.strip() else None
        try:
            self._recursive = _as_bool(config.get("recursive"), default=True)
        except ValueError as exc:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                str(exc),
                source=type(self).__name__,
            ) from exc

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        capability = getattr(self, "_capability", "list-modules")
        module_filter = getattr(self, "_module", None)
        recursive = getattr(self, "_recursive", True)
        try:
            payload = _payload(context)
        except (UnicodeDecodeError, TypeError, ValueError, json.JSONDecodeError) as exc:
            raise BindletError(
                BindletErrorCode.INPUT_MALFORMED,
                str(exc),
                source=type(self).__name__,
            ) from exc

        scope_name = self.package_name if module_filter is None else _resolve_module_name(
            self.package_name, module_filter
        )
        requested_module = payload.get("module")
        if requested_module is None:
            requested_module = scope_name
        else:
            requested_module = _resolve_module_name(
                scope_name, str(requested_module))

        if capability == "list-modules":
            modules = _module_records(scope_name, recursive)
            result = {
                "schema_version": 1,
                "package": self.package_name,
                "scope": scope_name,
                "recursive": recursive,
                "module_count": len(modules),
                "modules": modules,
            }
            message = f"listed {len(modules)} module(s) under {scope_name}"
            severity = Severity.INFORMATION
        elif capability == "describe-module":
            try:
                module = importlib.import_module(requested_module)
            except ModuleNotFoundError as exc:
                raise BindletError(
                    BindletErrorCode.NOT_FOUND,
                    str(exc),
                    source=type(self).__name__,
                ) from exc
            except ImportError as exc:
                raise BindletError(
                    BindletErrorCode.EXECUTION_FAILED,
                    str(exc),
                    source=type(self).__name__,
                ) from exc
            result = {
                "schema_version": 1,
                "package": self.package_name,
                "scope": scope_name,
                "module": _describe_module(module),
            }
            message = f"described {requested_module}"
            severity = Severity.INFORMATION
        else:
            modules = _module_records(scope_name, recursive)
            failures: list[dict[str, str]] = []
            for record in modules:
                name = record["name"]
                try:
                    importlib.import_module(name)
                except Exception as exc:  # pragma: no cover - exercised in failure tests
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
            message = (
                f"validated {len(modules)} module(s) under {scope_name}"
                if not failures
                else f"{len(failures)} module import(s) failed under {scope_name}"
            )
            severity = Severity.INFORMATION if not failures else Severity.ERROR

        return [
            AnalysisIssue(
                rule_id=f"{self.bindlet_name}.{capability}",
                message=message,
                severity=severity,
                confidence=Confidence.HIGH,
                extra={"result": json.dumps(
                    result, sort_keys=True, separators=(",", ":"), ensure_ascii=False)},
            )
        ]


AbstractDomainlet._registry.pop(f"{__name__}._VmmPackageBindlet", None)


__all__ = [
    "_VmmPackageBindlet",
]
