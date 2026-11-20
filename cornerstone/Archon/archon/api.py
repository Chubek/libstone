"""Public, in-process Python API for the Archon compiler and runtime.

ArchDBs are ordinary dictionaries with the documented ArchDB schema. Use
``Target`` for instruction and program assembly, codecs, and execution;
``MachineState`` includes registers, scalars, stacks, and memory. IR generation
has a separate API in :mod:`archon.irgen.api`.
"""

import json

from .c99 import generate_codec
from .c99_emulator import generate_emulator, generate_emulator_header
from .compiler import compile_file
from .database import dumps as dump_database
from .database import load as load_database
from .database import loads as parse_database
from .database import save as save_database
from .diagnostics import ArchonError, Location
from .frontend import parse
from .runtime import MachineState, Target


def parse_source(source: str, filename: str = "<source>") -> dict:
    """Parse source text without resolving imports or compiling the result."""
    return parse(source, filename)


def validate_source(path, *, parameters=None, features=None) -> None:
    """Validate an architecture and its imports with the compilation checks."""
    compile_file(path, parameters=parameters, features=features)


def validate_database(db: dict) -> None:
    """Raise :class:`ArchonError` if a database cannot be serialized as ArchDB."""
    dump_database(db)


def inspect_database(db: dict) -> dict:
    """Return a validated, detached, structured view of the entire ArchDB."""
    return json.loads(dump_database(db))


_EXPORTS = {
    item.__name__: item
    for item in (
        ArchonError, Location, MachineState, Target, compile_file, parse_source,
        validate_source, validate_database, inspect_database,
        generate_codec, generate_emulator, generate_emulator_header,
    )
}
_EXPORTS.update(
    dump_database=dump_database,
    load_database=load_database,
    parse_database=parse_database,
    save_database=save_database,
)


def exports() -> tuple[str, ...]:
    """List names available through :func:`get_export` and :func:`dispatch`."""
    return tuple(sorted(_EXPORTS))


def get_export(name: str):
    """Return a documented API export by name (not an arbitrary Python symbol)."""
    try:
        return _EXPORTS[name]
    except (KeyError, TypeError):
        raise ArchonError(f"unknown Archon API export {name!r}") from None


def dispatch(name: str, /, *args, **kwargs):
    """Call a public API export; positional and keyword arguments are unchanged."""
    operation = get_export(name)
    if not callable(operation):
        raise ArchonError(f"Archon API export {name!r} is not callable")
    return operation(*args, **kwargs)


__all__ = [*exports(), "exports", "get_export", "dispatch"]
