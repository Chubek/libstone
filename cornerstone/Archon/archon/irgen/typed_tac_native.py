"""Emit the optional, in-process typed TAC to native backend adapter."""

from pathlib import Path

_SOURCE_DIR = Path(__file__).with_name("native")


def emit_typed_tac_adapter(context: dict) -> dict[str, str]:
    """The typed TAC operation set is independent of the target database."""
    del context
    return {
        name: (_SOURCE_DIR / name).read_text(encoding="utf-8")
        for name in ("TypedTACBackend.c", "TypedTACBackend.h")
    }
