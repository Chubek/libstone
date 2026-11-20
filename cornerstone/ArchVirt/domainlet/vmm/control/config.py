"""VMM configuration dataclasses."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True, slots=True)
class VmConfig:
    """Immutable VM configuration."""

    kernel_path: Path | str
    initrd_path: Path | str | None = None
    cmdline: str = "console=ttyS0"
    memory_size: int = 128 * 1024 * 1024
    vcpus: int = 1
    vm_type: int = 0

    def __post_init__(self) -> None:
        if self.memory_size <= 0:
            raise ValueError("memory_size must be positive")
        if self.vcpus <= 0:
            raise ValueError("vcpus must be positive")
        if self.vm_type < 0:
            raise ValueError("vm_type must be non-negative")
        if not str(self.kernel_path).strip():
            raise ValueError("kernel_path cannot be empty")
