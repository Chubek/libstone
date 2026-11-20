"""Emit the standalone native Linux sandbox runner."""

from pathlib import Path

STAGE_ORDER = 1800


def emit(context: dict) -> dict[str, str]:
    source = Path(__file__).with_name("tool_templates") / "RunSandbox.c"
    return {"RunSandbox.c": source.read_text(encoding="utf-8")}
