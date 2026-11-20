"""Bindlet for the validated VirtIO device catalog.

The bindlet exports a stable JSON boundary for the C/C++ VMM integration:

* ``list-devices`` returns the complete spec-backed catalog;
* ``describe-device`` resolves a device by module name, canonical name,
  alias, or numeric device ID;
* ``validate-device`` reports descriptor consistency errors.
"""

from __future__ import annotations

import json
from collections.abc import Mapping
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
from domainlet.vmm.virtio.base import VirtioTransport
from domainlet.vmm.virtio.catalog import REGISTRY, get_pair

_CAPABILITIES = {
    "list-devices": "List all spec-backed VirtIO device descriptors.",
    "describe-device": "Resolve and return one VirtIO device and kernel descriptor.",
    "validate-device": "Validate one descriptor and report any consistency errors.",
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
        raise ValueError(
            "VirtIO bindlet input must be a JSON object or mapping")
    decoded = json.loads(value)
    if not isinstance(decoded, Mapping):
        raise ValueError("VirtIO bindlet input must decode to a JSON object")
    return dict(decoded)


def _device_key(value: Any) -> int | str:
    if isinstance(value, bool):
        raise ValueError("device must be a name, alias, or integer device ID")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip():
        normalized = value.strip()
        return int(normalized) if normalized.isdecimal() else normalized
    raise ValueError("device must be a name, alias, or integer device ID")


class VirtioBindlet(AbstractDomainlet):
    """Expose the VirtIO catalog through the canonical bindlet lifecycle."""

    bindlet_name = "vmm.virtio"
    version = "1.1.0"

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name=self.bindlet_name,
            version=self.version,
            description="Validated VirtIO device descriptors derived from the bundled OASIS specification.",
            author="domweave",
            capabilities=[
                DomainletCapability(
                    name=name,
                    description=description,
                    input_kind=InputKind.SOURCE_TEXT,
                    output_kinds=["json"],
                    tags=["vmm", "virtio", name.removeprefix(
                        "list-").removeprefix("describe-")],
                )
                for name, description in _CAPABILITIES.items()
            ],
            config_schema={
                "capability": {
                    "type": "string",
                    "default": "list-devices",
                    "enum": list(_CAPABILITIES),
                },
                "device": {
                    "type": "string",
                    "description": "Name, alias, module key, or numeric VirtIO device ID.",
                },
                "transport": {
                    "type": "string",
                    "enum": [item.value for item in VirtioTransport],
                },
            },
        )

    def configure(self, config: dict[str, str]) -> None:
        unknown = set(config) - {"capability", "device", "transport"}
        if unknown:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown configuration key(s): {', '.join(sorted(unknown))}",
                source=type(self).__name__,
            )
        capability = config.get("capability", "list-devices")
        if capability not in _CAPABILITIES:
            raise BindletError(
                BindletErrorCode.CONFIG_INVALID,
                f"unknown capability '{capability}'; choose from {sorted(_CAPABILITIES)}",
                source=type(self).__name__,
            )
        transport = config.get("transport")
        normalized_transport = transport.strip().lower(
        ) if transport and transport.strip() else None
        if normalized_transport is not None:
            try:
                VirtioTransport(normalized_transport)
            except ValueError as exc:
                raise BindletError(
                    BindletErrorCode.CONFIG_INVALID,
                    f"unknown transport '{transport}'",
                    source=type(self).__name__,
                ) from exc
        device = config.get("device")
        self._capability = capability
        self._device = device
        self._transport = VirtioTransport(
            normalized_transport) if normalized_transport else None

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        capability = getattr(self, "_capability", "list-devices")
        try:
            payload = _payload(context)
        except (UnicodeDecodeError, TypeError, ValueError, json.JSONDecodeError) as exc:
            raise BindletError(
                BindletErrorCode.INPUT_MALFORMED,
                str(exc),
                source=type(self).__name__,
            ) from exc
        configured_device = getattr(self, "_device", None)
        selected_transport = getattr(self, "_transport", None)
        requested_device = payload.get("device", configured_device)

        if capability == "list-devices":
            devices = [
                item
                for item in REGISTRY.devices
                if selected_transport is None or selected_transport in item.transports
            ]
            result: dict[str, Any] = {
                "schema_version": 1,
                "count": len(devices),
                "devices": [item.to_dict() for item in devices],
            }
            message = f"listed {len(devices)} VirtIO device descriptors"
        else:
            if requested_device is None:
                raise BindletError(
                    BindletErrorCode.INPUT_MALFORMED,
                    f"capability '{capability}' requires a device",
                    source=type(self).__name__,
                )
            try:
                key = _device_key(requested_device)
                device, kernel = get_pair(str(key))
            except (KeyError, TypeError, ValueError) as exc:
                raise BindletError(
                    BindletErrorCode.NOT_FOUND if isinstance(
                        exc, KeyError) else BindletErrorCode.INPUT_MALFORMED,
                    str(exc),
                    source=type(self).__name__,
                ) from exc
            if selected_transport is not None and selected_transport not in device.transports:
                raise BindletError(
                    BindletErrorCode.INPUT_MALFORMED,
                    f"device '{device.name}' does not support {selected_transport.value}",
                    source=type(self).__name__,
                )
            errors = device.validation_errors()
            result = {
                "schema_version": 1,
                "device": device.to_dict(),
                "kernel": kernel.to_dict() if kernel is not None else None,
                "valid": not errors,
                "errors": list(errors),
            }
            message = f"{device.name} descriptor is {'valid' if not errors else 'invalid'}"
            if capability == "validate-device":
                result = {
                    "schema_version": 1,
                    "device_id": device.device_id,
                    "name": device.name,
                    "valid": not errors,
                    "errors": list(errors),
                }

        return [
            AnalysisIssue(
                rule_id=f"{self.bindlet_name}.{capability}",
                message=message,
                severity=Severity.INFORMATION if result.get(
                    "valid", True) else Severity.ERROR,
                confidence=Confidence.HIGH,
                extra={"result": json.dumps(result, sort_keys=True)},
            )
        ]


__all__ = ["VirtioBindlet"]
