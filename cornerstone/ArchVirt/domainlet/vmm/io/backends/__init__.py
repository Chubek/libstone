"""I/O backends."""

from __future__ import annotations

from domainlet.vmm.io.backends.block import BlockBackend
from domainlet.vmm.io.backends.block_raw import RawBlockBackend
from domainlet.vmm.io.backends.net import NetBackend
from domainlet.vmm.io.backends.net_tap import TapBackend

__all__ = ["BlockBackend", "NetBackend", "RawBlockBackend", "TapBackend"]
