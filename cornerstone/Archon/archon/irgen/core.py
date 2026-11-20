"""Registry, context validation, and atomic output for IR generators."""

from __future__ import annotations

from collections.abc import Iterable, Mapping, Sequence
import hashlib
import importlib
import json
import os
from pathlib import Path, PurePosixPath
import pkgutil
import shutil
import tempfile
from types import ModuleType

from .. import database
from ..compiler import compile_file
from ..diagnostics import ArchonError
from ..frontend import parse


EXPECTED_REPRESENTATIONS = (
    "gated",
    "memory-ssa",
    "sea-of-nodes",
    "anf",
    "cps",
    "typed-tac",
    "untyped-tac",
    "supercombinators",
)

_EXPECTED_MODULES = (
    "gated",
    "memory_ssa",
    "sea_of_nodes",
    "anf",
    "cps",
    "typed_tac",
    "untyped_tac",
    "supercombinators",
)
_RESERVED_MODULES = {"core"}
_MAX_OUTPUT_FILES = 4096
_MAX_OUTPUT_BYTES = 256 * 1024 * 1024
_EXECUTABLE_OUTPUTS = frozenset(("Assembler.sh", "Linker.sh"))
_LAST_IMPORT_ERRORS: dict[str, Exception] = {}


def _normalise_name(name: str) -> str:
    if not isinstance(name, str) or not name.strip():
        raise ArchonError("IR representation name must be a nonempty string")
    return name.strip().lower().replace("_", "-")


def _package_modules() -> list[ModuleType]:
    """Import current package modules in a stable order.

    Discovery deliberately happens on every registry query.  This lets a
    checkout acquire optional peer backends after :mod:`archon.irgen` itself
    has already been imported, and avoids making one missing backend break all
    of the others.
    """

    importlib.invalidate_caches()
    package = importlib.import_module(__package__)
    names = {
        info.name
        for info in pkgutil.walk_packages(package.__path__, package.__name__ + ".")
        if info.name.rsplit(".", 1)[-1] not in _RESERVED_MODULES
    }
    names.update(f"{package.__name__}.{name}" for name in _EXPECTED_MODULES)
    modules = []
    _LAST_IMPORT_ERRORS.clear()
    for qualified in sorted(names):
        try:
            modules.append(importlib.import_module(qualified))
        except ModuleNotFoundError as exc:
            # Missing optional backend modules are expected while a reduced
            # installation is being assembled.  A missing dependency *inside*
            # an existing module is a real error and must not be hidden.
            if exc.name != qualified:
                _LAST_IMPORT_ERRORS[qualified] = exc
        except Exception as exc:
            # One optional plug-in must not disable unrelated representations.
            # Discovery is repeated, so a module that is installed or repaired
            # later becomes available on the next query.
            _LAST_IMPORT_ERRORS[qualified] = exc
    return modules


def _representation_registry() -> tuple[dict[str, ModuleType], dict[str, set[str]]]:
    canonical: dict[str, ModuleType] = {}
    aliases: dict[str, set[str]] = {}
    for module in _package_modules():
        emitter = getattr(module, "emit_ir", None)
        if not callable(emitter):
            continue
        name = _normalise_name(getattr(module, "NAME", ""))
        if name in canonical and canonical[name] is not module:
            raise ArchonError(f"duplicate IR representation name {name!r}")
        canonical[name] = module
        raw_aliases = getattr(module, "ALIASES", ())
        if isinstance(raw_aliases, str) or not isinstance(raw_aliases, Sequence):
            raise ArchonError(f"IR representation {name!r} has invalid ALIASES")
        for raw_alias in (name, *raw_aliases):
            alias = _normalise_name(raw_alias)
            aliases.setdefault(alias, set()).add(name)
    return canonical, aliases


def available_representations() -> tuple[str, ...]:
    """Return canonical names of installed representation backends."""

    canonical, _ = _representation_registry()
    expected = [name for name in EXPECTED_REPRESENTATIONS if name in canonical]
    extra = sorted(set(canonical) - set(expected))
    return tuple(expected + extra)


def get_representation(name: str) -> ModuleType:
    """Resolve a canonical representation name or alias to its module."""

    canonical, aliases = _representation_registry()
    key = _normalise_name(name)
    matches = aliases.get(key)
    if not matches:
        expected_modules = dict(zip(EXPECTED_REPRESENTATIONS, _EXPECTED_MODULES))
        module_name = expected_modules.get(key)
        qualified = f"{__package__}.{module_name}" if module_name else None
        if qualified in _LAST_IMPORT_ERRORS:
            error = _LAST_IMPORT_ERRORS[qualified]
            raise ArchonError(
                f"IR representation {name!r} could not be loaded: "
                f"{type(error).__name__}: {error}"
            ) from error
        installed = ", ".join(available_representations()) or "(none)"
        raise ArchonError(
            f"unknown IR representation {name!r}; installed representations: {installed}"
        )
    if len(matches) != 1:
        choices = ", ".join(sorted(matches))
        raise ArchonError(
            f"ambiguous IR representation alias {name!r}; use one of: {choices}"
        )
    return canonical[next(iter(matches))]


def pipeline_stages() -> tuple[ModuleType, ...]:
    """Return every installed pipeline stage in deterministic execution order."""

    stages = []
    for module in _package_modules():
        if callable(getattr(module, "emit_ir", None)):
            continue
        if getattr(module, "PIPELINE_STAGE", True) is False:
            continue
        if callable(getattr(module, "emit", None)):
            order = getattr(module, "STAGE_ORDER", getattr(module, "ORDER", 1000))
            if type(order) not in (int, float):
                raise ArchonError(f"pipeline stage {module.__name__!r} has a nonnumeric order")
            stages.append((order, module.__name__, module))
    return tuple(module for _, _, module in sorted(stages, key=lambda row: row[:2]))


def _json_copy(value, subject: str):
    try:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)
        return json.loads(encoded)
    except (TypeError, ValueError, RecursionError) as exc:
        raise ArchonError(f"{subject} must be JSON-compatible: {exc}") from exc


def build_context(
    ir: str,
    architectures: Iterable[dict],
    *,
    options: Mapping[str, object] | None = None,
) -> dict:
    """Create the plain, JSON-compatible context shared by all emitters.

    ArchDB inputs are validated at this boundary and copied, so an emitter
    cannot mutate a caller's database.  Generation options live under the
    ``options`` key and must have string keys and JSON-compatible values.  The
    default-on ``oracle`` option is materialized as a strict JSON boolean.
    """

    module = get_representation(ir)
    canonical_name = _normalise_name(module.NAME)
    if isinstance(architectures, (str, bytes, Mapping)):
        raise ArchonError("architectures must be an iterable of ArchDB objects")
    validated = []
    try:
        iterator = iter(architectures)
    except TypeError as exc:
        raise ArchonError("architectures must be an iterable of ArchDB objects") from exc
    for index, archdb in enumerate(iterator):
        if not isinstance(archdb, dict):
            raise ArchonError(f"architectures[{index}] must be an ArchDB object")
        # Round-tripping through the public database boundary both validates
        # and creates a JSON-only deep copy with stable key handling.
        validated.append(database.loads(database.dumps(archdb)))
    architecture_names = [archdb["architecture"] for archdb in validated]
    seen_names = set()
    for name in architecture_names:
        if name in seen_names:
            raise ArchonError(f"duplicate IR-generation architecture {name!r}")
        seen_names.add(name)

    if options is None:
        option_copy = {}
    else:
        if not isinstance(options, Mapping) or any(not isinstance(key, str) for key in options):
            raise ArchonError("generation options must be an object with string keys")
        option_copy = _json_copy(dict(options), "generation options")
    option_copy.setdefault("oracle", True)
    if type(option_copy["oracle"]) is not bool:
        raise ArchonError("generation option 'oracle' must be a boolean")
    pipeline_target = option_copy.get("target")
    if pipeline_target is not None:
        if not isinstance(pipeline_target, str) or not pipeline_target:
            raise ArchonError("generation option 'target' must be a nonempty string")
        if pipeline_target not in seen_names:
            raise ArchonError(
                f"generation option 'target' names unknown architecture "
                f"{pipeline_target!r}"
            )

    context = {
        "ir": canonical_name,
        "architectures": validated,
        "architecture_names": architecture_names,
        "options": option_copy,
    }
    # Keep this explicit invariant: stage and backend contexts may cross a
    # process boundary in future generators.
    _json_copy(context, "IR-generation context")
    return context


def _safe_output_name(name: object) -> str:
    if not isinstance(name, str) or not name:
        raise ArchonError("generated output names must be nonempty strings")
    if "\\" in name or any(ord(character) < 0x20 for character in name):
        raise ArchonError(f"unsafe generated output path {name!r}")
    path = PurePosixPath(name)
    if path.is_absolute() or path.parts in ((), (".",)):
        raise ArchonError(f"unsafe generated output path {name!r}")
    if any(part in {"", ".", ".."} for part in path.parts):
        raise ArchonError(f"unsafe generated output path {name!r}")
    normalized = path.as_posix()
    if normalized != name or ":" in path.parts[0]:
        raise ArchonError(f"unsafe generated output path {name!r}")
    return normalized


def _outputs(value: object, producer: str) -> dict[str, str]:
    if not isinstance(value, dict):
        raise ArchonError(f"{producer} must return a dict[str, str]")
    if len(value) > _MAX_OUTPUT_FILES:
        raise ArchonError(f"{producer} emitted more than {_MAX_OUTPUT_FILES} files")
    result: dict[str, str] = {}
    total = 0
    for raw_name, text in value.items():
        name = _safe_output_name(raw_name)
        if not isinstance(text, str):
            raise ArchonError(f"{producer} emitted non-text content for {name!r}")
        try:
            size = len(text.encode("utf-8"))
        except UnicodeError as exc:
            raise ArchonError(f"{producer} emitted invalid UTF-8 text for {name!r}") from exc
        total += size
        if total > _MAX_OUTPUT_BYTES:
            raise ArchonError(f"generated output exceeds {_MAX_OUTPUT_BYTES} bytes")
        if name in result:
            raise ArchonError(f"{producer} emitted duplicate output path {name!r}")
        result[name] = text

    # A file cannot also be the parent directory of another emitted file.
    names = set(result)
    for name in names:
        parts = PurePosixPath(name).parts
        for length in range(1, len(parts)):
            if PurePosixPath(*parts[:length]).as_posix() in names:
                raise ArchonError(f"generated output paths conflict at {name!r}")
    return dict(sorted(result.items()))


def _invoke(module: ModuleType, function_name: str, context: dict) -> dict[str, str]:
    """Invoke one plug-in with the canonical, JSON-only context contract."""

    producer = f"{module.__name__}.{function_name}"
    plugin_context = _json_copy(context, "IR-generation context")
    architectures = plugin_context["architectures"]
    target_name = plugin_context["options"].get("target")
    if architectures:
        if target_name is None:
            pipeline_database = min(
                architectures,
                key=lambda row: (
                    row["architecture"],
                    json.dumps(row, sort_keys=True, separators=(",", ":")),
                ),
            )
        else:
            pipeline_database = next(
                row for row in architectures
                if row["architecture"] == target_name
            )
    else:
        pipeline_database = None
    # Compatibility spellings stay at the invocation boundary; the public
    # context contract remains the four canonical keys.
    plugin_context.update({
        "databases": architectures,
        "archdbs": architectures,
        "selected_databases": architectures,
        "selected_archdbs": architectures,
        "targets": architectures,
        "database": pipeline_database,
        "db": pipeline_database,
        "archdb": pipeline_database,
        "pipeline_target": (
            pipeline_database["architecture"]
            if pipeline_database is not None else None
        ),
    })
    try:
        result = getattr(module, function_name)(plugin_context)
    except ArchonError:
        raise
    except Exception as exc:
        raise ArchonError(
            f"IR-generation plug-in {producer} failed: {type(exc).__name__}: {exc}"
        ) from exc
    return _outputs(result, producer)


def generate_ir(
    ir: str,
    architectures: Iterable[dict],
    *,
    options: Mapping[str, object] | None = None,
    run_stages: bool = True,
) -> dict[str, str]:
    """Run one representation emitter and every installed pipeline stage."""

    context = build_context(ir, architectures, options=options)
    module = get_representation(context["ir"])
    generated = _invoke(module, "emit_ir", context)
    if "IR.c" not in generated or "IR.h" not in generated:
        raise ArchonError(
            f"IR representation {context['ir']!r} must emit both IR.c and IR.h"
        )

    if run_stages:
        for stage in pipeline_stages():
            additions = _invoke(stage, "emit", context)
            for name, text in additions.items():
                if name in generated and generated[name] != text:
                    raise ArchonError(
                        f"pipeline stage {stage.__name__!r} conflicts on output {name!r}"
                    )
                generated[name] = text
    return dict(sorted(generated.items()))


# A concise spelling for embedding callers.
generate = generate_ir


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _check_destination(destination: Path, input_paths: Iterable[str | os.PathLike[str]]) -> None:
    try:
        resolved = destination.resolve(strict=False)
        cwd = Path.cwd().resolve()
    except (OSError, RuntimeError) as exc:
        raise ArchonError(f"cannot resolve IR output directory {str(destination)!r}: {exc}") from exc
    if resolved == Path(resolved.anchor) or resolved == cwd:
        raise ArchonError("refusing to replace a filesystem root or the current directory")
    for raw_input in input_paths:
        try:
            path = Path(raw_input)
            source = path.resolve(strict=False)
            parent_entry = path.parent.resolve(strict=False) / path.name
            lexical = Path(os.path.abspath(path))
        except (OSError, RuntimeError) as exc:
            raise ArchonError(f"cannot resolve input path {str(raw_input)!r}: {exc}") from exc
        # Protect the reference as well as its target. A caller's symlink
        # inside the replaced tree is still an input path even if it points out.
        if any(_is_relative_to(candidate, resolved) for candidate in (source, parent_entry, lexical)):
            raise ArchonError(
                f"IR output directory {str(destination)!r} would overwrite input {str(raw_input)!r}"
            )


def _tree_digest(root: Path) -> str | None:
    """Hash a regular-file-only directory, or return ``None`` if it is unsafe."""

    digest = hashlib.sha256()
    try:
        entries = sorted(root.rglob("*"), key=lambda path: path.relative_to(root).as_posix())
        for path in entries:
            if path.is_symlink():
                return None
            relative = path.relative_to(root).as_posix()
            if path.is_dir():
                digest.update(b"D\0" + relative.encode("utf-8") + b"\0")
            elif path.is_file():
                digest.update(b"F\0" + relative.encode("utf-8") + b"\0")
                if relative in _EXECUTABLE_OUTPUTS:
                    digest.update(bytes([path.stat().st_mode & 0o111]))
                digest.update(path.read_bytes())
                digest.update(b"\0")
            else:
                return None
    except (OSError, UnicodeError):
        return None
    return digest.hexdigest()


def _write_tree(root: Path, outputs: Mapping[str, str]) -> None:
    os.chmod(root, 0o755)
    directories = {PurePosixPath(name).parent for name in outputs}
    for relative in sorted(directories, key=lambda path: (len(path.parts), path.as_posix())):
        if relative == PurePosixPath("."):
            continue
        path = root.joinpath(*relative.parts)
        path.mkdir(parents=True, exist_ok=True)
        os.chmod(path, 0o755)
    for name, text in sorted(outputs.items()):
        path = root.joinpath(*PurePosixPath(name).parts)
        with path.open("w", encoding="utf-8", newline="\n") as stream:
            stream.write(text)
            stream.flush()
            os.fsync(stream.fileno())
        os.chmod(path, 0o755 if name in _EXECUTABLE_OUTPUTS else 0o644)


def write_generated_directory(
    outputs: Mapping[str, str],
    destination: str | os.PathLike[str],
    *,
    input_paths: Iterable[str | os.PathLike[str]] = (),
    force: bool = False,
) -> Path:
    """Write a generated tree transactionally and return its absolute path.

    Files are first materialized in a sibling temporary directory.  An existing
    tree with identical contents and wrapper execute bits is left untouched.
    Replacing any other nonempty tree requires ``force=True``; input paths
    are protected even with force.
    """

    normalized = _outputs(dict(outputs), "IR generator")
    target = Path(destination)
    if not target.name:
        raise ArchonError("IR output directory must have a final path component")
    _check_destination(target, input_paths)
    try:
        parent = target.parent.resolve(strict=False)
        parent.mkdir(parents=True, exist_ok=True)
        if target.is_symlink():
            raise ArchonError(f"refusing to replace symlink output directory {str(target)!r}")
        if target.exists() and not target.is_dir():
            raise ArchonError(f"IR output path {str(target)!r} is not a directory")
    except ArchonError:
        raise
    except OSError as exc:
        raise ArchonError(f"cannot prepare IR output directory {str(target)!r}: {exc}") from exc

    temporary = Path(tempfile.mkdtemp(prefix=f".{target.name}.tmp-", dir=parent))
    backup: Path | None = None
    try:
        _write_tree(temporary, normalized)
        if target.exists():
            target_digest = _tree_digest(target)
            temporary_digest = _tree_digest(temporary)
            if target_digest is not None and target_digest == temporary_digest:
                shutil.rmtree(temporary)
                return target.resolve()
            try:
                nonempty = next(target.iterdir(), None) is not None
            except OSError as exc:
                raise ArchonError(f"cannot inspect IR output directory {str(target)!r}: {exc}") from exc
            if nonempty and not force:
                raise ArchonError(
                    f"IR output directory {str(target)!r} is nonempty; use --force to replace it"
                )
            backup = Path(tempfile.mkdtemp(prefix=f".{target.name}.old-", dir=parent))
            backup.rmdir()
            os.replace(target, backup)
        try:
            os.replace(temporary, target)
        except BaseException:
            if backup is not None and backup.exists() and not target.exists():
                os.replace(backup, target)
            raise
        if backup is not None:
            shutil.rmtree(backup)
        return target.resolve()
    except ArchonError:
        raise
    except OSError as exc:
        raise ArchonError(f"cannot write IR output directory {str(target)!r}: {exc}") from exc
    finally:
        if temporary.exists():
            shutil.rmtree(temporary, ignore_errors=True)
        if backup is not None and backup.exists():
            # A failed rollback should not delete the only old tree.
            if target.exists():
                shutil.rmtree(backup, ignore_errors=True)


write_directory = write_generated_directory


def standard_spec_entry_points(root: str | os.PathLike[str] | None = None) -> tuple[Path, ...]:
    """Discover every ``architecture`` entry point in the bundled stdspec."""

    directory = (
        Path(__file__).resolve().parents[2] / "stdspec"
        if root is None
        else Path(root)
    )
    if not directory.is_dir():
        raise ArchonError(f"standard specification directory not found: {directory}")
    entries = []
    for path in sorted(directory.rglob("*.arch")):
        try:
            source = path.read_text(encoding="utf-8")
        except (OSError, UnicodeError) as exc:
            raise ArchonError(f"cannot read standard specification {str(path)!r}: {exc}") from exc
        if parse(source, str(path))["kind"] == "architecture":
            entries.append(path)
    if not entries:
        raise ArchonError(f"no standard specification entry points found in {directory}")
    return tuple(entries)


def compile_standard_architectures(
    root: str | os.PathLike[str] | None = None,
) -> list[dict]:
    """Compile all bundled stdspec entry points in stable path order."""

    return [compile_file(path) for path in standard_spec_entry_points(root)]
