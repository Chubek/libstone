from __future__ import annotations

import json
from pathlib import Path
from typing import Any, cast

import pytest

from domainlet.bindlet_base import AnalysisContext, BindletError, InputKind
from domainlet.bindlets.vmm_virtio import VirtioBindlet
from domainlet.vmm.virtio import REGISTRY
from domainlet.vmm.virtio.catalog import DEVICE_PAIRS, get_pair


def _result(bindlet: VirtioBindlet, data: object = None) -> dict[str, Any]:
    issues = bindlet.execute(AnalysisContext(
        InputKind.SOURCE_TEXT, data or {}))
    assert len(issues) == 1
    return cast(dict[str, Any], json.loads(issues[0].extra["result"]))


def test_catalog_is_unique_and_points_at_bundled_spec() -> None:
    assert len(REGISTRY.devices) == len(DEVICE_PAIRS) == 23
    assert len({item.device_id for item in REGISTRY.devices}
               ) == len(REGISTRY.devices)

    for device in REGISTRY.devices:
        assert Path(device.spec_ref).is_file()
        assert device.validation_errors() == ()
        for queue in device.queues:
            if queue.optional_feature:
                assert device.feature(queue.optional_feature) is not None


def test_registry_resolves_names_aliases_and_numeric_ids() -> None:
    assert REGISTRY.require("net").device_id == 1
    assert REGISTRY.require("network").device_id == 1
    assert REGISTRY.require("1").device_id == 1
    assert REGISTRY.require(28).name == "rpmb"
    assert get_pair("blk")[0].name == "block"


def test_bindlet_lists_descriptors_as_json() -> None:
    bindlet = VirtioBindlet()
    bindlet.configure({"capability": "list-devices", "transport": "pci"})

    result = _result(bindlet)

    assert result["schema_version"] == 1
    assert result["count"] == 23
    assert len(result["devices"]) == 23


def test_bindlet_describes_device_from_payload_alias() -> None:
    bindlet = VirtioBindlet()
    bindlet.configure({"capability": "describe-device"})

    result = _result(bindlet, {"device": "vsock"})

    assert result["device"]["device_id"] == 19
    assert result["kernel"]["module"] == "domainlet.vmm.virtio.vsock"


def test_bindlet_validates_numeric_device_id() -> None:
    bindlet = VirtioBindlet()
    bindlet.configure({"capability": "validate-device", "device": "2"})

    result = _result(bindlet)

    assert result["device_id"] == 2
    assert result["name"] == "block"
    assert result["valid"] is True
    assert result["errors"] == []


def test_bindlet_rejects_unknown_configuration() -> None:
    with pytest.raises(BindletError):
        VirtioBindlet().configure({"capability": "list-devices", "bogus": "1"})


def test_bindlet_rejects_unknown_device() -> None:
    bindlet = VirtioBindlet()
    bindlet.configure({"capability": "describe-device", "device": "missing"})

    with pytest.raises(BindletError):
        bindlet.execute(AnalysisContext(InputKind.SOURCE_TEXT, {}))
