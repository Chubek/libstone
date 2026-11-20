"""VirtIO CAN device domainlet."""

from __future__ import annotations

from .catalog import get_pair

DEVICE, _kernel = get_pair("can")
KERNEL = _kernel

__all__ = ["DEVICE", "KERNEL"]
