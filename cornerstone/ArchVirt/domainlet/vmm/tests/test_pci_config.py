from __future__ import annotations

from domainlet.vmm.io.pci.config import PciConfigSpace


def test_pci_config_header_fields_roundtrip() -> None:
    config = PciConfigSpace(0x1AF4, 0x1041, class_code=0x02)
    assert config.read(0, 4) == b"\xf4\x1aA\x10"
