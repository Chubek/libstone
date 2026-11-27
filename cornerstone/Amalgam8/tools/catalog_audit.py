#!/usr/bin/env python3
"""Audit the Amalgam8 catalog against the Libstone checkout and public install."""

import argparse
import csv
import io
import pathlib
import re
import subprocess
import sys
import tempfile


AM8 = pathlib.Path(__file__).resolve().parents[1]
ROOT = AM8.parents[1]
CATALOG = AM8 / "catalog"
FIELDS = ("family", "name", "source_path", "include", "cmake_target",
          "status", "evidence", "bundle", "license")
FAMILIES = ("firestone", "tourchstone", "moonstone")
BUNDLED = frozenset(("libbignum/BigInt.hpp", "libbignum/BigFloat.hpp",
                     "libbignum/Complex128.hpp"))
PROBED = {"libbignum/BigInt.hpp": "firestone::bignum::BigInt{3}",
          "libbignum/BigFloat.hpp": "firestone::bignum::BigFloat{}",
          "libbignum/Complex128.hpp": "firestone::bignum::Complex128{}",
          "libdsl/LibDSL.hpp": 'dsl::synth::SpecBuilder{"catalog-probe"}'}


def run(*argv):
    subprocess.run(argv, cwd=ROOT, check=True)


def git_tracked():
    raw = subprocess.check_output(["git", "ls-files", "-z", "--",
                                   *FAMILIES, "LICENSE"], cwd=ROOT)
    return {path.decode() for path in raw.split(b"\0") if path}


def firestone_rows(tracked):
    cmake = (ROOT / "firestone/CMakeLists.txt").read_text(encoding="utf-8")
    # Fail closed if this install/export structure changes; don't label a
    # tracked source header as a public API based on its name alone.
    install_statements = (
        "add_library(Firestone::${component} ALIAS firestone_${component})",
        "set_target_properties(firestone_${component} PROPERTIES EXPORT_NAME ${component})",
        "list(APPEND firestone_directories lib${component})",
        "install(TARGETS firestone firestone_dsl ${firestone_targets} EXPORT FirestoneTargets)",
        "install(FILES Firestone.hpp FireIR.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/firestone)",
        "install(FILES libdsl/LibDSL.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/firestone/libdsl)",
        'install(DIRECTORY ${firestone_directories} DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/firestone FILES_MATCHING PATTERN "*.hpp")',
        "install(EXPORT FirestoneTargets NAMESPACE Firestone::",
    )
    match = re.search(r"^set\(firestone_components\s+([a-z\s]+)\)", cmake, re.M)
    if not match or any(statement not in cmake for statement in install_statements):
        raise ValueError("Firestone CMake public install/export rules changed; review the catalog")
    components = {"lib" + name: name for name in match[1].split()}
    for source in sorted(tracked):
        if not source.startswith("firestone/") or not source.endswith(".hpp"):
            continue
        name = source.removeprefix("firestone/")
        parts = name.split("/")
        if name in ("Firestone.hpp", "FireIR.hpp"):
            component, evidence = "firestone", "CMakeLists.txt:install(FILES aggregate)"
        elif name == "libdsl/LibDSL.hpp":
            component, evidence = "dsl", "CMakeLists.txt:install(FILES dsl)"
        elif len(parts) > 1 and parts[0] in components:
            component, evidence = components[parts[0]], "CMakeLists.txt:install(DIRECTORY components)"
        else:
            continue
        if not all(re.fullmatch(r"[a-zA-Z0-9_.-]+", part) and part not in (".", "..") for part in parts):
            raise ValueError(f"unsupported Firestone public include spelling: {name}")
        verified = name in PROBED
        if verified:
            evidence += "; installed CMake consumer probe"
        yield dict(zip(FIELDS, ("firestone", name, source, source, "Firestone::" + component,
                                "available" if verified else "unverified", evidence,
                                "yes" if name in BUNDLED else "no",
                                "MIT:LICENSE" if name in BUNDLED else "-")))


def as_tsv(rows):
    buffer = io.StringIO(newline="")
    writer = csv.DictWriter(buffer, FIELDS, delimiter="\t", lineterminator="\n")
    writer.writeheader()
    writer.writerows(rows)
    return buffer.getvalue()


def read_rows(path, family, tracked):
    with path.open(encoding="utf-8", newline="") as stream:
        reader = csv.DictReader(stream, delimiter="\t")
        if reader.fieldnames != list(FIELDS):
            raise ValueError(f"{path.name}: incorrect catalog columns")
        rows = []
        for row in reader:
            prefix = f"{path.name}:{reader.line_num}"
            if set(row) != set(FIELDS) or any(not row[field] for field in FIELDS):
                raise ValueError(f"{prefix}: malformed row")
            if row["family"] != family or row["name"] == "-":
                raise ValueError(f"{prefix}: invalid catalog identity")
            if row["status"] not in {"available", "unverified", "unavailable"}:
                raise ValueError(f"{prefix}: invalid availability")
            if row["bundle"] not in {"yes", "no"} or (
                    row["bundle"] == "yes" and row["license"] == "-"):
                raise ValueError(f"{prefix}: invalid bundling/license evidence")
            source = row["source_path"]
            if source != "-" and source not in tracked:
                raise ValueError(f"{prefix}: not a Git-tracked catalog source: {source}")
            if row["status"] == "available" and "-" in (
                    source, row["include"], row["cmake_target"], row["evidence"]):
                raise ValueError(f"{prefix}: available row is missing public evidence")
            rows.append(row)
    return rows


def cpp_rows(rows):
    # All fields consumed here are derived from validated Git/CMake metadata.
    return "// Generated by tools/catalog_audit.py --refresh-firestone; do not edit.\n" + "".join(
        f'{{"{row["name"]}", "{row["cmake_target"]}", '
        f'{"true" if row["status"] == "available" else "false"}}},\n' for row in rows)


def installed_probe(rows):
    with tempfile.TemporaryDirectory(prefix="am8-catalog-") as tmp:
        base = pathlib.Path(tmp)
        build, prefix, consumer = (base / name for name in ("build", "prefix", "consumer"))
        run("cmake", "-S", str(ROOT / "firestone"), "-B", str(build),
            "-DFIRESTONE_BUILD_TESTS=OFF", "-DLIBSTONE_BUILD_TESTS=OFF")
        run("cmake", "--install", str(build), "--prefix", str(prefix))
        consumer.mkdir()
        project = ["cmake_minimum_required(VERSION 3.20)",
                   "project(FirestoneCatalog LANGUAGES CXX)",
                   "find_package(Firestone CONFIG REQUIRED)"]
        for i, (header, expr) in enumerate(PROBED.items()):
            target = next(row["cmake_target"] for row in rows if row["name"] == header)
            unit = f"probe_{i}"
            # The native target spelling is deliberately distinct from the
            # historical Amalgam8 spelling, which expects the prefix's include root.
            include = "LibDSL.hpp" if target == "Firestone::dsl" else header
            (consumer / (unit + ".cpp")).write_text(
                f"#include <{include}>\nint main() {{ (void)({expr}); }}\n",
                encoding="utf-8")
            project.extend((f"add_executable({unit} {unit}.cpp)",
                            f"target_link_libraries({unit} PRIVATE {target})"))
        (consumer / "CMakeLists.txt").write_text("\n".join(project) + "\n", encoding="utf-8")
        run("cmake", "-S", str(consumer), "-B", str(consumer / "build"),
            f"-DCMAKE_PREFIX_PATH={prefix}")
        run("cmake", "--build", str(consumer / "build"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", choices=FAMILIES, help="audit one catalog rather than all")
    parser.add_argument("--refresh-firestone", action="store_true", help="regenerate the Firestone TSV and C++ index")
    parser.add_argument("--probe-firestone", action="store_true", help="install Firestone and build the available-row probes")
    parser.add_argument("--tsv", action="store_true", help="print the combined rows, sorted by family and name")
    args = parser.parse_args()
    tracked = git_tracked()
    expected = list(firestone_rows(tracked))
    tsv = CATALOG / "firestone.tsv"
    cpp = CATALOG / "firestone.inc"
    if args.refresh_firestone:
        CATALOG.mkdir(exist_ok=True)
        tsv.write_text(as_tsv(expected), encoding="utf-8")
        cpp.write_text(cpp_rows(expected), encoding="utf-8")
    rows = []
    for family in (args.family,) if args.family else FAMILIES:
        family_rows = read_rows(CATALOG / f"{family}.tsv", family, tracked)
        if family == "firestone" and (family_rows != expected or cpp.read_text(encoding="utf-8") != cpp_rows(expected)):
            raise ValueError("Firestone inventory drift; review and run --refresh-firestone")
        rows.extend(family_rows)
    rows.sort(key=lambda row: (row["family"], row["name"]))
    keys = {(row["family"], row["name"]) for row in rows}
    if len(keys) != len(rows):
        raise ValueError("duplicate catalog identity")
    if args.probe_firestone:
        if args.tsv:
            parser.error("--tsv and --probe-firestone cannot be used together")
        installed_probe(expected)
    if args.tsv:
        print(as_tsv(rows), end="")
    else:
        print(f"Audited {len(rows)} catalog records.")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as exc:
        sys.exit(f"catalog audit: {exc}")
