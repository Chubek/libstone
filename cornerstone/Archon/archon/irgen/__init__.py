"""Generate standalone C99 implementations of Archon IR representations.

Representation modules implement ``NAME``, ``ALIASES`` and
``emit_ir(context)``.  Pipeline-stage modules implement ``emit(context)``.
See :mod:`archon.irgen.core` for the public orchestration API.
"""

from .core import (
    EXPECTED_REPRESENTATIONS,
    available_representations,
    build_context,
    compile_standard_architectures,
    generate,
    generate_ir,
    get_representation,
    pipeline_stages,
    standard_spec_entry_points,
    write_directory,
    write_generated_directory,
)

__all__ = [
    "EXPECTED_REPRESENTATIONS",
    "available_representations",
    "build_context",
    "compile_standard_architectures",
    "generate",
    "generate_ir",
    "get_representation",
    "pipeline_stages",
    "standard_spec_entry_points",
    "write_directory",
    "write_generated_directory",
]
