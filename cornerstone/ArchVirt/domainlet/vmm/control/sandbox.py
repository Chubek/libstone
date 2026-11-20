"""Host sandbox helpers."""

from __future__ import annotations

import os


def install_sandbox(enabled: bool = True) -> None:
    """Best-effort sandbox hook."""

    if not enabled:
        return
    if hasattr(os, "prctl"):
        return


def drop_privileges(uid: int | None = None, gid: int | None = None) -> None:
    """Drop process privileges when requested."""

    if gid is not None and hasattr(os, "setgid"):
        os.setgid(gid)
    if uid is not None and hasattr(os, "setuid"):
        os.setuid(uid)
