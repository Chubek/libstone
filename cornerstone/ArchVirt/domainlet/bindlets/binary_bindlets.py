"""DomPyBind binding definitions for domainlet.binary kernels.

Exposes the binary-analysis domainlets to the C++ Domfix engine through
the bindlet bridge.  Each binding class wraps a domainlet kernel module
and translates its Python API into the typed AnalysisContext /
AnalysisIssue protocol expected by the C host.

Usage (C++ host side)::

    import bindlets.binary_bindlets
    registry = bindlets.binary_bindlets.discover()
"""

from __future__ import annotations

from typing import Any, ClassVar

from domainlet.bindlet_base import (
    AbstractDomainlet,
    AnalysisContext,
    AnalysisIssue,
    BindletError,
    BindletErrorCode,
    DomainletCapability,
    DomainletInfo,
    Severity,
)


# ══════════════════════════════════════════════════════════════════════
#  BinaryFormat bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryFormatBindlet(AbstractDomainlet):
    """Detect binary format from magic bytes without full parsing."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.format",
            version="1.0.0",
            description="Detect binary format (ELF/PE/Mach-O) from magic bytes.",
            capabilities=[
                DomainletCapability("binary.format.detect", "Detect binary format"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary._common import detect_format

        fmt = detect_format(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinaryStats bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryStatsBindlet(AbstractDomainlet):
    """Extract comprehensive statistics from a binary."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.stats",
            version="1.0.0",
            description="Extract statistics (sections, imports, exports, symbols) from binaries.",
            capabilities=[
                DomainletCapability("binary.stats.extract", "Extract binary statistics"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import get_stats

        stats = get_stats(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinarySymbols bindlet
# ══════════════════════════════════════════════════════════════════════

class BinarySymbolsBindlet(AbstractDomainlet):
    """List import and export symbols from a binary."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.symbols",
            version="1.0.0",
            description="List symbols, imports, and exports from binaries.",
            capabilities=[
                DomainletCapability("binary.symbols.imports", "List imported symbols"),
                DomainletCapability("binary.symbols.exports", "List exported symbols"),
                DomainletCapability("binary.symbols.names", "List all symbol names"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import list_imports, list_exports, list_names

        imports = list_imports(context.source_data)
        exports = list_exports(context.source_data)
        names = list_names(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinaryStrings bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryStringsBindlet(AbstractDomainlet):
    """Extract printable strings from a binary."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.strings",
            version="1.0.0",
            description="Extract printable strings from binaries.",
            capabilities=[
                DomainletCapability("binary.strings.extract", "Extract strings"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._min_length = int(config.get("min_length", 4))
        self._encoding = config.get("encoding", "ascii")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import list_strings

        strings = list_strings(
            context.source_data,
            min_length=self._min_length,
            encoding=self._encoding,
        )
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinarySections bindlet
# ══════════════════════════════════════════════════════════════════════

class BinarySectionsBindlet(AbstractDomainlet):
    """List sections and segments from a binary."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.sections",
            version="1.0.0",
            description="Enumerate sections and segments from binaries.",
            capabilities=[
                DomainletCapability("binary.sections.list", "List sections"),
                DomainletCapability("binary.sections.segments", "List segments"),
                DomainletCapability("binary.sections.data", "Extract section data"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import list_sections, list_segments

        sections = list_sections(context.source_data)
        segments = list_segments(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinaryDisassembly bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryDisassemblyBindlet(AbstractDomainlet):
    """Disassemble machine code using Capstone."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.disassembly",
            version="1.0.0",
            description="Architecture-neutral machine-code disassembly.",
            capabilities=[
                DomainletCapability("binary.disassembly.disassemble", "Disassemble machine code"),
                DomainletCapability("binary.disassembly.lite", "Fast tuple-based disassembly"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        self._arch = config.get("arch", "X86")
        self._mode = config.get("mode", "MODE_64")

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import Architecture, Mode, disassemble

        arch = getattr(Architecture, self._arch, Architecture.X86)
        mode = getattr(Mode, self._mode, Mode.MODE_64)
        insns = disassemble(
            context.source_data.encode() if isinstance(context.source_data, str) else context.source_data,
            arch,
            mode,
            address=context.address,
            count=context.count,
        )
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinarySharedLibs bindlet
# ══════════════════════════════════════════════════════════════════════

class BinarySharedLibsBindlet(AbstractDomainlet):
    """Trace shared library dependencies."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.shared_libs",
            version="1.0.0",
            description="Trace shared library dependencies (NEEDED, imports, LC_LOAD_DYLIB).",
            capabilities=[
                DomainletCapability("binary.shared_libs.trace", "Trace shared libraries"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import list_shared_libs

        libs = list_shared_libs(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinaryHeader bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryHeaderBindlet(AbstractDomainlet):
    """Inspect binary headers."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.header",
            version="1.0.0",
            description="Inspect binary headers (ELF, PE, Mach-O).",
            capabilities=[
                DomainletCapability("binary.header.inspect", "Inspect binary headers"),
                DomainletCapability("binary.header.machine", "Detect machine type"),
                DomainletCapability("binary.header.type", "Detect binary type"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import get_header, get_machine_type, get_binary_type

        header = get_header(context.source_data)
        machine = get_machine_type(context.source_data)
        kind = get_binary_type(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  BinaryIntegrity bindlet
# ══════════════════════════════════════════════════════════════════════

class BinaryIntegrityBindlet(AbstractDomainlet):
    """Verify binary integrity (checksums, hashes, entropy)."""

    def probe(self) -> DomainletInfo:
        return DomainletInfo(
            name="binary.integrity",
            version="1.0.0",
            description="Verify binary integrity: checksums, hashes, entropy.",
            capabilities=[
                DomainletCapability("binary.integrity.checksum", "Verify checksums"),
                DomainletCapability("binary.integrity.hashes", "Compute section hashes"),
                DomainletCapability("binary.integrity.entropy", "Compute section entropy"),
            ],
        )

    def configure(self, config: dict[str, str]) -> None:
        pass

    def execute(self, context: AnalysisContext) -> list[AnalysisIssue]:
        from domainlet.binary import (
            verify_checksum,
            compute_sha256,
            get_overall_entropy,
        )

        result = verify_checksum(context.source_data)
        sha256 = compute_sha256(context.source_data)
        entropy = get_overall_entropy(context.source_data)
        return []


# ══════════════════════════════════════════════════════════════════════
#  Discovery helpers
# ══════════════════════════════════════════════════════════════════════

_BINDLET_CLASSES: ClassVar[list[type[AbstractDomainlet]]] = [
    BinaryFormatBindlet,
    BinaryStatsBindlet,
    BinarySymbolsBindlet,
    BinaryStringsBindlet,
    BinarySectionsBindlet,
    BinaryDisassemblyBindlet,
    BinarySharedLibsBindlet,
    BinaryHeaderBindlet,
    BinaryIntegrityBindlet,
]


def discover() -> list[DomainletInfo]:
    """Return metadata for all registered binary domainlet classes."""
    return [cls.probe_static() for cls in _BINDLET_CLASSES]


def get_bindlet(name: str) -> type[AbstractDomainlet] | None:
    """Look up a binary bindlet class by name."""
    for cls in _BINDLET_CLASSES:
        try:
            if cls.probe_static().name == name:
                return cls
        except Exception:
            continue
    return None


__all__ = [
    "BinaryDisassemblyBindlet",
    "BinaryFormatBindlet",
    "BinaryHeaderBindlet",
    "BinaryIntegrityBindlet",
    "BinarySectionsBindlet",
    "BinarySharedLibsBindlet",
    "BinaryStatsBindlet",
    "BinaryStringsBindlet",
    "BinarySymbolsBindlet",
    "discover",
    "get_bindlet",
]
