from __future__ import annotations

from domainlet.vmm.boot.boot_params import BootParams, E820Entry


def test_boot_params_serializes_e820() -> None:
    params = BootParams(e820_entries=[E820Entry(
        0, 0x9F000), E820Entry(0x100000, 0x100000)])
    blob = params.to_bytes()
    assert len(blob) == 4096
    assert blob[0x1F1:0x1F5]
