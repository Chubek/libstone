"""Minimal AML byte emitter."""

from __future__ import annotations


def aml_buffer(*bytes_: int) -> bytes:
    """Return raw AML bytes."""

    if any(not 0 <= value <= 0xFF for value in bytes_):
        raise ValueError("AML byte must be in range 0..255")
    return bytes(bytes_)
