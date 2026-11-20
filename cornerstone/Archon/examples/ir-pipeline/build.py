#!/usr/bin/env python3
"""Build and exercise the generated RV32I IR pipeline without shell quoting."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shlex
import subprocess
import sys


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ARCHONC = ROOT / "tools" / "archonc"
IRGEN = ROOT / "tools" / "archon-irgen"
RV32I = ROOT / "stdspec" / "riscv" / "rv32i.arch"
ANF_FIXTURE = HERE / "fixtures" / "v1" / "anf"
SELECTOR_FIXTURE = HERE / "expected" / "v1" / "selector"
EMITTER_FIXTURE = HERE / "expected" / "v1" / "emitter" / "rv32i"
REGALLOC_FIXTURE = HERE / "expected" / "v1" / "regalloc"
SCHEDULER_EXPECTED = HERE / "expected" / "v1" / "scheduler" / "MIR.i"
PIPELINE_MIR_EXPECTED = HERE / "expected" / "v1" / "pipeline" / "MIR.i"
MANIFEST = ".archon-ir-pipeline-manifest.json"
DRIVER_INTERMEDIATES = (
    "Rewritten.i",
    "MIR.i",
    "Selected.i",
    "InstrMap.i",
    "Scheduled.i",
    "RegMap.i",
    "Regallocated.i",
)


class BuildError(RuntimeError):
    pass


def _run(command: list[str], *, cwd: Path | None = None, stdout=None) -> None:
    display = " ".join(shlex.quote(part) for part in command)
    print(f"+ {display}")
    try:
        subprocess.run(command, cwd=cwd, stdout=stdout, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BuildError(f"command failed: {display}") from exc


def _compiler() -> list[str]:
    command = shlex.split(os.environ.get("CC", "cc"))
    if not command:
        raise BuildError("CC expands to an empty command")
    return command


def _inside(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
    except ValueError:
        return False
    return True


def _safe_build_directory(raw: str) -> Path:
    path = Path(raw).expanduser().resolve(strict=False)
    filesystem_root = Path(path.anchor)
    if (
        path == filesystem_root
        or path in {HERE, ROOT}
        or _inside(HERE, path)
        or not path.name
    ):
        raise BuildError("refusing to use a filesystem or source-tree root as a build directory")
    return path


def _record(build: Path, paths: set[Path]) -> None:
    rows = []
    for path in sorted(paths):
        if path.is_symlink():
            raise BuildError(f"refusing to record a symbolic link: {path}")
        resolved = path.resolve(strict=False)
        if not _inside(resolved, build):
            raise BuildError(f"refusing to record path outside build tree: {path}")
        rows.append(resolved.relative_to(build).as_posix())
    manifest = build / MANIFEST
    if manifest.is_symlink():
        raise BuildError(f"refusing to replace symbolic-link manifest: {manifest}")
    manifest.write_text(
        json.dumps({"schema": 1, "files": rows}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _read_manifest(build: Path) -> list[Path]:
    manifest = build / MANIFEST
    if manifest.is_symlink():
        raise BuildError(f"refusing to read symbolic-link manifest: {manifest}")
    if not manifest.is_file():
        return []
    try:
        value = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise BuildError(f"cannot read cleanup manifest {manifest}: {exc}") from exc
    if (
        not isinstance(value, dict)
        or value.get("schema") != 1
        or not isinstance(value.get("files"), list)
        or any(not isinstance(item, str) for item in value["files"])
    ):
        raise BuildError(f"invalid cleanup manifest: {manifest}")
    result = []
    for item in value["files"]:
        relative = PurePosixPath(item)
        if (
            not item
            or relative.is_absolute()
            or relative.as_posix() != item
            or any(part in {"", ".", ".."} for part in relative.parts)
        ):
            raise BuildError(f"unsafe cleanup manifest entry: {item!r}")
        candidate = build.joinpath(*relative.parts)
        parent = candidate.parent.resolve(strict=False)
        if not _inside(parent, build) or candidate == build:
            raise BuildError(f"unsafe cleanup manifest entry: {item!r}")
        result.append(candidate)
    return result


def clean(build: Path) -> None:
    if not build.exists():
        return
    if build.is_symlink() or not build.is_dir():
        raise BuildError(f"build path is not a real directory: {build}")
    for path in _read_manifest(build):
        if path.is_symlink() or path.is_file():
            path.unlink()
    manifest = build / MANIFEST
    if manifest.is_file() or manifest.is_symlink():
        manifest.unlink()
    for directory in sorted(
        (path for path in build.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    ):
        try:
            directory.rmdir()
        except OSError:
            pass
    try:
        build.rmdir()
    except OSError:
        pass


def _compile(
    compiler: list[str],
    source: Path,
    output: Path,
    include_directories: list[Path],
    definitions: list[str] | None = None,
) -> None:
    command = [
        *compiler,
        "-std=c99",
        "-pedantic-errors",
        "-Wall",
        "-Wextra",
        "-Werror",
        *(definitions or ()),
    ]
    for directory in include_directories:
        command.extend(("-I", str(directory)))
    command.extend(("-c", str(source), "-o", str(output)))
    _run(command)


def _link(compiler: list[str], objects: list[Path], output: Path) -> None:
    _run([*compiler, *(str(path) for path in objects), "-o", str(output)])


def build(build: Path) -> None:
    if build.is_symlink() or (build.exists() and not build.is_dir()):
        raise BuildError(f"build path is not a real directory: {build}")
    if (
        build.is_dir()
        and any(build.iterdir())
        and not (build / MANIFEST).is_file()
    ):
        raise BuildError(
            "nonempty build directory has no ir-pipeline cleanup manifest"
        )
    build.mkdir(parents=True, exist_ok=True)
    previous_outputs = set(_read_manifest(build))
    _record(build, previous_outputs)
    generated = build / "generated"
    objects = build / "objects"
    objects.mkdir(exist_ok=True)
    database = build / "rv32i.archdb"
    built: set[Path] = {database, *previous_outputs}
    _record(build, built)

    _run([str(ARCHONC), str(RV32I), "-o", str(database)])
    _run(
        [
            str(IRGEN),
            "anf",
            str(database),
            "-o",
            str(generated),
            "--force",
        ]
    )

    include_directories = [
        generated,
        ANF_FIXTURE,
        SELECTOR_FIXTURE,
        EMITTER_FIXTURE,
    ]
    compiler = _compiler()
    built.update(path for path in generated.rglob("*") if path.is_file())
    _record(build, built)
    object_by_stem: dict[str, Path] = {}
    for source in sorted(generated.glob("*.c")):
        output = objects / f"{source.stem}.o"
        built.add(output)
        _record(build, built)
        definitions = []
        if source.stem == "IR":
            definitions.append(
                "-DARCHON_IR_PROGRAM_FILE="
                + json.dumps(str(ANF_FIXTURE / "Program.i"))
            )
        elif source.stem == "ISel":
            definitions.append(
                "-DARCHON_MIR_FILE="
                + json.dumps(str(SELECTOR_FIXTURE / "MIR.i"))
            )
        _compile(
            compiler,
            source,
            output,
            include_directories,
            definitions,
        )
        object_by_stem[source.stem] = output

    required = {
        "ArchonEmbed",
        "Driver",
        "Emitter",
        "Graphs",
        "IR",
        "ISched",
        "ISel",
        "Oracle",
        "Pipeline",
        "Regalloc",
        "Rewrite",
        "Scheduler",
    }
    missing = sorted(required - object_by_stem.keys())
    if missing:
        raise BuildError(f"generator omitted required C99 source(s): {', '.join(missing)}")

    scheduler_main = objects / "scheduler_demo.o"
    built.add(scheduler_main)
    _record(build, built)
    _compile(compiler, HERE / "scheduler_demo.c", scheduler_main, include_directories)

    embed_main = objects / "embed_client.o"
    built.add(embed_main)
    _record(build, built)
    _compile(
        compiler,
        HERE / "embed_client.c",
        embed_main,
        include_directories,
    )

    regalloc_library = objects / "Regalloc.library.o"
    built.add(regalloc_library)
    _record(build, built)
    _compile(
        compiler,
        generated / "Regalloc.c",
        regalloc_library,
        include_directories,
        ["-DARCHON_REGALLOC_NO_MAIN"],
    )

    isel_library = objects / "ISel.library.o"
    built.add(isel_library)
    _record(build, built)
    _compile(
        compiler,
        generated / "ISel.c",
        isel_library,
        include_directories,
        ["-DARCHON_ISEL_NO_MAIN"],
    )

    executables = {
        "embedded-pipeline": [
            object_by_stem["ArchonEmbed"],
            object_by_stem["Emitter"],
            object_by_stem["Graphs"],
            object_by_stem["IR"],
            object_by_stem["ISched"],
            isel_library,
            object_by_stem["Oracle"],
            object_by_stem["Pipeline"],
            regalloc_library,
            object_by_stem["Rewrite"],
            embed_main,
        ],
        "pipeline-driver": [
            object_by_stem["Driver"],
            object_by_stem["Emitter"],
            object_by_stem["Graphs"],
            object_by_stem["IR"],
            object_by_stem["ISched"],
            isel_library,
            object_by_stem["Oracle"],
            object_by_stem["Pipeline"],
            regalloc_library,
            object_by_stem["Rewrite"],
        ],
        "selector": [object_by_stem["ISel"], object_by_stem["Oracle"]],
        "regalloc": [object_by_stem["Regalloc"], object_by_stem["Oracle"]],
        "scheduler-demo": [
            object_by_stem["IR"],
            object_by_stem["Oracle"],
            object_by_stem["Scheduler"],
            scheduler_main,
        ],
    }
    for name, inputs in executables.items():
        output = build / name
        built.add(output)
        _record(build, built)
        _link(compiler, inputs, output)
    _record(build, built)


def _same_file(actual: Path, expected: Path) -> None:
    actual_bytes = actual.read_bytes()
    expected_bytes = expected.read_bytes()
    if actual_bytes != expected_bytes:
        raise BuildError(f"{actual} does not match versioned expectation {expected}")


def demo(build_directory: Path) -> None:
    build(build_directory)
    actual = build_directory / "actual"
    actual.mkdir(exist_ok=True)
    outputs: set[Path] = set(_read_manifest(build_directory))

    scheduler_output = actual / "MIR.scheduler.i"
    outputs.add(scheduler_output)
    _record(build_directory, outputs)
    with scheduler_output.open("wb") as stream:
        _run([str(build_directory / "scheduler-demo")], stdout=stream)
    _same_file(scheduler_output, SCHEDULER_EXPECTED)

    selector_output = actual / "InstrMap.i"
    outputs.add(selector_output)
    _record(build_directory, outputs)
    _run(
        [str(build_directory / "selector"), "RISCV32", str(selector_output)],
        cwd=actual,
    )
    _same_file(selector_output, SELECTOR_FIXTURE / "InstrMap.i")

    regalloc_output = actual / "Regallocated.report.i"
    outputs.add(regalloc_output)
    _record(build_directory, outputs)
    _run(
        [
            str(build_directory / "regalloc"),
            str(REGALLOC_FIXTURE / "Scheduled.i"),
            str(REGALLOC_FIXTURE / "MIR.i"),
            str(REGALLOC_FIXTURE / "RegMap.i"),
            str(regalloc_output),
        ],
        cwd=actual,
    )
    _same_file(regalloc_output, REGALLOC_FIXTURE / "Regallocated.i")

    # A generated driver treats an existing Regallocated.i as a deliberate
    # emitter restart point. Remove only prior, manifest-owned intermediates so
    # repeated demos exercise the complete ANF pipeline again.
    for name in DRIVER_INTERMEDIATES:
        artifact = actual / name
        if artifact in outputs and (artifact.is_file() or artifact.is_symlink()):
            artifact.unlink()

    embedded_output = actual / "Program.embed.s"
    outputs.add(embedded_output)
    _record(build_directory, outputs)
    _run([str(build_directory / "embedded-pipeline")], cwd=actual)
    _same_file(embedded_output, EMITTER_FIXTURE / "Program.gas.s")

    for dialect, filename, expectation in (
        ("gas", "Program.gas.s", EMITTER_FIXTURE / "Program.gas.s"),
        (
            "sandstone",
            "Program.sandstone.asm",
            EMITTER_FIXTURE / "Program.sandstone.asm",
        ),
    ):
        output = actual / filename
        outputs.add(output)
        outputs.update(actual / name for name in DRIVER_INTERMEDIATES)
        _record(build_directory, outputs)
        _run(
            [
                str(build_directory / "pipeline-driver"),
                "--dialect",
                dialect,
                "--output",
                str(output),
                "--keep-artifacts",
            ],
            cwd=actual,
        )
        _same_file(output, expectation)

    _same_file(actual / "MIR.i", PIPELINE_MIR_EXPECTED)
    allocated = (actual / "Regallocated.i").read_text(encoding="utf-8")
    if "REGALLOC_EMISSION_BEGIN(1)" not in allocated:
        raise BuildError("full pipeline omitted its executable allocation section")
    _record(build_directory, outputs)
    print(f"RV32I demonstration passed; artifacts are in {actual}")


def digest_generated(build: Path) -> str:
    digest = hashlib.sha256()
    generated = build / "generated"
    for path in sorted(path for path in generated.rglob("*") if path.is_file()):
        digest.update(path.relative_to(generated).as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()


def rebuild_check(build_directory: Path) -> None:
    build(build_directory)
    first = digest_generated(build_directory)
    build(build_directory)
    second = digest_generated(build_directory)
    if first != second:
        raise BuildError("generated tree changed across identical rebuilds")
    print(f"deterministic generated-tree digest: {second}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "action",
        nargs="?",
        choices=("build", "demo", "rebuild-check", "clean"),
        default="demo",
    )
    parser.add_argument(
        "--build-dir",
        default=str(HERE / "build"),
        help="dedicated output directory (default: %(default)s)",
    )
    args = parser.parse_args(argv)
    try:
        directory = _safe_build_directory(args.build_dir)
        if args.action == "clean":
            clean(directory)
        elif args.action == "build":
            build(directory)
        elif args.action == "rebuild-check":
            rebuild_check(directory)
        else:
            demo(directory)
    except BuildError as exc:
        print(f"ir-pipeline: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
