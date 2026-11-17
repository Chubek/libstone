#!/usr/bin/env python3
"""Idempotently register QMR with moonstone/CMakeLists.txt; no other changes."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[2]
cmake = root / "CMakeLists.txt"
if not cmake.exists():
    raise SystemExit(f"Root Moonstone CMakeLists.txt not found at {cmake}")
text = cmake.read_text(encoding="utf-8")
option = 'option(MOONSTONE_BUILD_QMR "Build Lua-backed QMR template engine" OFF)\n'
anchor = 'option(MOONSTONE_BUILD_PROLIFIC '
if option not in text:
    idx = text.find(anchor)
    if idx < 0:
        raise SystemExit("Cannot find MOONSTONE_BUILD_PROLIFIC option; edit manually")
    idx = text.find('\n', idx) + 1
    text = text[:idx] + option + text[idx:]
block = '\nif(MOONSTONE_BUILD_QMR)\n  add_subdirectory(qmr)\nendif()\n'
if block not in text:
    anchor = 'if(MOONSTONE_BUILD_PROLIFIC)\n  add_subdirectory(prolific)\nendif()'
    if anchor not in text:
        raise SystemExit("Cannot find Prolific inclusion; edit manually")
    text = text.replace(anchor, anchor + '\n' + block, 1)
cmake.write_text(text, encoding="utf-8")
print(f"Registered QMR in {cmake} (default OFF, enable with -DMOONSTONE_BUILD_QMR=ON)")
