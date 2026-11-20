"""VirtIO I2C adapter domainlet."""

from __future__ import annotations

from .catalog import get_pair

DEVICE, _kernel = get_pair("i2c")
KERNEL = _kernel

__all__ = ["DEVICE", "KERNEL"]
