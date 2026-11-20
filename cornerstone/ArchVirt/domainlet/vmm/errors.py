"""Error hierarchy for VMM domainlets."""

from __future__ import annotations

import errno


class VmmError(RuntimeError):
    """Base error for VMM failures."""


class KvmError(VmmError):
    """Raised when KVM ioctls or syscalls fail."""


class BusError(VmmError):
    """Raised when a bus access cannot be resolved."""


class MemoryError(VmmError):
    """Raised when guest memory access fails."""


class InterruptError(VmmError):
    """Raised when interrupt delivery fails."""


class EventLoopError(VmmError):
    """Raised when the reactor fails."""


class AllocatorError(VmmError):
    """Raised when an address-space allocation fails."""


class BootError(VmmError):
    """Raised when boot image preparation fails."""


def errno_text(value: int | None = None) -> str:
    """Return a stable errno description."""

    code = errno.EINVAL if value is None else value
    return f"errno={code} ({errno.errorcode.get(code, 'UNKNOWN')})"


def wrap_os_error(exc: OSError, message: str) -> VmmError:
    """Convert an :class:`OSError` to a VMM-specific exception."""

    detail = f"{message}: {errno_text(exc.errno)}"
    return VmmError(detail)
