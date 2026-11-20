"""Public, in-process IRGen API, separate from :mod:`archon.api`.

Generation returns source artifacts; it does not build or run them. The native
ELF bridges explicitly execute the caller's configured backend, assembler and
linker. Those tools and the Python modules on the installed emitter search
path must be trusted. See the generated artifacts' documentation for their
standalone C APIs.
"""

from ..diagnostics import ArchonError
from . import core, macros
from .core import (
    EXPECTED_REPRESENTATIONS,
    available_representations,
    build_context,
    compile_standard_architectures,
    generate_ir,
    standard_spec_entry_points,
    write_generated_directory,
)
from .native_target import resolve_native_target
from .objectwriter import assemble_object, link_executable
from .targetfacts import normalize as normalize_target_facts
from .tool_templates.proof_engine import solve as solve_smt


def get_representation(name: str) -> dict:
    """Resolve a representation name or alias to its public metadata."""
    module = core.get_representation(name)
    return {"name": module.NAME, "aliases": tuple(getattr(module, "ALIASES", ()))}


def _stage_name(module) -> str:
    return module.__name__.removeprefix(__package__ + ".").replace("_", "-")


def pipeline_stages() -> tuple[str, ...]:
    """List installed stage IDs in execution order, without exposing modules."""
    return tuple(_stage_name(module) for module in core.pipeline_stages())


def emit_stage(name: str, context: dict) -> dict[str, str]:
    """Generate just one named stage using a context from :func:`build_context`."""
    if not isinstance(context, dict) or not {"ir", "architectures", "options"} <= context.keys():
        raise ArchonError("expected an IRGen context from build_context")
    key = core._normalise_name(name)
    for stage in core.pipeline_stages():
        if _stage_name(stage) == key:
            validated = build_context(
                context["ir"], context["architectures"], options=context["options"],
            )
            return core._invoke(stage, "emit", validated)
    raise ArchonError(f"unknown IRGen stage {name!r}")


_EXPORTS = {
    "ArchonError": ArchonError,
    "EXPECTED_REPRESENTATIONS": EXPECTED_REPRESENTATIONS,
    "normalize_target_facts": normalize_target_facts,
    "solve_smt": solve_smt,
    **{
        item.__name__: item
        for item in (
            available_representations, get_representation, pipeline_stages, emit_stage,
            build_context, generate_ir, write_generated_directory,
            standard_spec_entry_points, compile_standard_architectures,
            resolve_native_target, assemble_object, link_executable,
        )
    },
}
_EXPORTS.update({
    f"macros.{name}": getattr(macros, name)
    for name in macros.__all__
    if name not in {"emit", "PIPELINE_STAGE"}
})


def exports() -> tuple[str, ...]:
    """List public operations and constants, including ``macros.*`` helpers."""
    return tuple(sorted(_EXPORTS))


def get_export(name: str):
    """Return an API export; dotted names are only for the public macro ABI."""
    try:
        return _EXPORTS[name]
    except (KeyError, TypeError):
        raise ArchonError(f"unknown Archon IRGen API export {name!r}") from None


def dispatch(name: str, /, *args, **kwargs):
    """Call a named public export with positional and/or keyword arguments."""
    operation = get_export(name)
    if not callable(operation):
        raise ArchonError(f"Archon IRGen API export {name!r} is not callable")
    return operation(*args, **kwargs)


__all__ = [
    *(name for name in exports() if "." not in name),
    "dispatch", "exports", "get_export", "macros",
]
