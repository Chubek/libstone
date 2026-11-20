from __future__ import annotations

import json

import domainlet.bindlets  # noqa: F401
from domainlet.bindlet_base import AbstractDomainlet, AnalysisContext, InputKind
from domainlet.bindlets.vmm_boot_bindlets import VmmBootBindlet
from domainlet.bindlets.vmm_control_bindlets import VmmControlBindlet
from domainlet.bindlets.vmm_core_bindlets import VmmCoreBindlet
from domainlet.bindlets.vmm_interrupts_bindlets import VmmInterruptsBindlet
from domainlet.bindlets.vmm_io_bindlets import VmmIoBindlet
from domainlet.bindlets.vmm_virtio_adapter_bindlets import VmmVirtioAdapterBindlet


def _result(bindlet: object, data: object = None) -> dict[str, object]:
    # type: ignore[attr-defined]
    issues = bindlet.execute(AnalysisContext(
        InputKind.SOURCE_TEXT, data or {}))
    assert len(issues) == 1
    return json.loads(issues[0].extra["result"])


def test_vmm_bindlets_are_discoverable() -> None:
    names = {info.name for info in AbstractDomainlet.discover()}
    assert {"vmm.core", "vmm.boot", "vmm.interrupts", "vmm.io",
            "vmm.control", "vmm.virtio_adapter", "vmm.virtio"} <= names


def test_vmm_core_lists_and_describes_modules() -> None:
    bindlet = VmmCoreBindlet()
    bindlet.configure({"capability": "list-modules"})
    listed = _result(bindlet)
    assert listed["package"] == "domainlet.vmm.core"
    assert listed["module_count"] >= 1

    bindlet.configure(
        {"capability": "describe-module", "module": "guest_memory"})
    described = _result(bindlet)
    assert described["module"]["name"] == "domainlet.vmm.core.guest_memory"


def test_vmm_control_validates_imports() -> None:
    bindlet = VmmControlBindlet()
    bindlet.configure({"capability": "validate-imports"})
    validated = _result(bindlet)
    assert validated["valid"] is True
    assert validated["package"] == "domainlet.vmm.control"


def test_vmm_category_bindlets_accept_scoped_modules() -> None:
    for bindlet in (
        VmmBootBindlet(),
        VmmInterruptsBindlet(),
        VmmIoBindlet(),
        VmmVirtioAdapterBindlet(),
    ):
        bindlet.configure({"capability": "describe-module"})
        result = _result(bindlet)
        assert result["module"]["is_package"] is True
