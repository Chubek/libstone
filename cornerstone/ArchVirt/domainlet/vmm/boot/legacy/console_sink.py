"""Console sinks for serial output."""

from __future__ import annotations

import sys
from dataclasses import dataclass


@dataclass(slots=True)
class ConsoleSink:
    """A minimal writable byte sink."""

    stream: object = sys.stdout.buffer

    def write(self, data: bytes) -> int:
        self.stream.write(data)
        if hasattr(self.stream, "flush"):
            self.stream.flush()
        return len(data)
