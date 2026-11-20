"""Kernel command-line helpers."""

from __future__ import annotations

from dataclasses import dataclass


def build_cmdline(*parts: str) -> str:
    """Join and validate a Linux cmdline."""

    cmdline = " ".join(part.strip() for part in parts if part.strip())
    if "\x00" in cmdline:
        raise ValueError("cmdline cannot contain NUL bytes")
    return cmdline


@dataclass(frozen=True, slots=True)
class Cmdline:
    """Validated command line."""

    value: str
