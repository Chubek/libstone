"""Legacy boot devices."""

from __future__ import annotations

from domainlet.vmm.boot.legacy.cmos import CmosRtc
from domainlet.vmm.boot.legacy.console_sink import ConsoleSink
from domainlet.vmm.boot.legacy.i8042 import I8042
from domainlet.vmm.boot.legacy.serial import SerialPort

__all__ = ["CmosRtc", "ConsoleSink", "I8042", "SerialPort"]
