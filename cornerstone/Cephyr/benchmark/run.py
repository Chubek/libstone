#!/usr/bin/env python3
"""Compare Cephyr's baseline with an opt-in QML pipeline using Prolific."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time

from workloads import workloads


ALU = frozenset(("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr"))


def command(argv: list[str], timeout: float) -> int:
    start = time.perf_counter_ns()
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter_ns() - start
    if completed.returncode:
        raise RuntimeError(f"exit {completed.returncode}: {argv!r}\n"
                           f"{completed.stdout}{completed.stderr}")
    return elapsed


def profile(path: Path, *, candidate: bool) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if not data["complete"] or data["rejected"] or data["open_spans"]:
        raise RuntimeError(f"incomplete Prolific collection: {path}")
    results = [event for event in data["events"] if event["name"] == "cephyr.result"]
    if len(results) != 1 or results[0]["kind"] != "instant" or results[0]["value"] != 0:
        raise RuntimeError(f"Prolific did not record successful compilation: {path}")
    names = Counter()
    durations = Counter()
    for event in data["events"]:
        if event["domain"] == "compiler" and event["kind"] == "end":
            names[event["name"]] += 1
            durations[event["name"]] += event["duration_ns"]
    if names["compile"] != 1 or (candidate and names["qml_rewrite"] != 1):
        raise RuntimeError(f"missing or duplicate expected compiler phases: {path}")
    if not candidate and names["qml_rewrite"]:
        raise RuntimeError(f"baseline unexpectedly ran QML (check config): {path}")
    return dict(durations)


def mir_metrics(mir: bytes) -> dict:
    opcodes = Counter()
    instruction_count = 0
    if not mir.startswith(b"archon-mir 1\n"):
        raise RuntimeError("expected Archon MachineIR v1")
    for line in mir.splitlines():
        tokens = line.split()
        if not tokens:
            continue
        op = tokens[0].decode("ascii")
        if op == "block":
            instruction_count += int(tokens[2])
        elif op in ALU:
            opcodes[op] += 1
    return {"bytes": len(mir), "instructions": instruction_count,
            "alu": sum(opcodes.values()), "alu_opcodes": dict(sorted(opcodes.items()))}


def measure(base: list[str], source: Path, directory: Path,
            iteration: str, candidate: bool, timeout: float) -> tuple[dict, bytes]:
    trace = directory / (iteration + ".prolific.json")
    mir_path = directory / (iteration + ".mir")
    wall = command([*base, "--no-cache", f"--moonstone-profile={trace}",
                    "--emit-mir", "-o", str(mir_path), str(source)], timeout)
    return {"wall_ns": wall, "phases_ns": profile(trace, candidate=candidate)}, mir_path.read_bytes()


def statistics_for(samples: list[dict]) -> dict:
    phases = sorted(samples[0]["phases_ns"])
    if any(sorted(sample["phases_ns"]) != phases for sample in samples):
        raise RuntimeError("inconsistent Prolific phase instrumentation across runs")
    return {
        "wall_ns_median": statistics.median(s["wall_ns"] for s in samples),
        "wall_ns_min": min(s["wall_ns"] for s in samples),
        "wall_ns_max": max(s["wall_ns"] for s in samples),
        "phases_ns_median": {
            phase: statistics.median(s["phases_ns"][phase] for s in samples)
            for phase in phases
        },
    }


def benchmark(args: argparse.Namespace) -> None:
    active = [case for case in workloads() if re.search(args.fixture, case.name)]
    if not active:
        raise RuntimeError(f"no fixtures matched {args.fixture!r}")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = args.config.resolve() if args.config else out / "Cephyr.toml"
    if not args.config:
        # Explicit config avoids ambient Cephyr.toml / CEPHYR_CONFIG settings.
        config.write_text('[paths]\ncache_path = ".cache"\n', encoding="utf-8")
    common = [str(args.cephyr.resolve()), f"--config={config}", f"--target={args.target}"]
    selection = args.candidate_arg or ["--qml-pass=canonicalize"]
    modes = {"baseline": common}
    if not args.baseline_only:
        modes["qml"] = [*common, *selection]
    report = {
        "schema": "cephyr.benchmark.v1",
        "compiler": str(args.cephyr.resolve()),
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "release": platform.release(),
            "cpus": sorted(os.sched_getaffinity(0)),
        },
        "host_cc": args.host_cc if not args.no_verify else None,
        "target": args.target,
        "config": str(config),
        "candidate_arguments": selection if "qml" in modes else [],
        "repeat": args.repeat,
        "warmup": args.warmup,
        "verified_native": not args.no_verify,
        "workloads": {},
    }

    print(f"{'workload':24} {'base ms':>9} {'QML ms':>9} {'pass ms':>9} {'ALU base→QML':>15}")
    for case in active:
        case_dir = out / case.name
        case_dir.mkdir()
        source = case_dir / (case.name + ".c")
        source.write_text(case.source, encoding="ascii")
        for mode in modes:
            (case_dir / mode).mkdir()
        # Warm and measure serially, alternating order per pair to reduce bias.
        entries = {mode: {"samples": []} for mode in modes}
        reference_mir = {}
        for iteration in range(-args.warmup, args.repeat):
            order = list(modes) if iteration % 2 == 0 else list(reversed(modes))
            for mode in order:
                label = f"warmup{-iteration:02}" if iteration < 0 else f"run{iteration + 1:02}"
                sample, mir = measure(modes[mode], source, case_dir / mode, label,
                                      mode == "qml", args.timeout)
                if mode in reference_mir and reference_mir[mode] != mir:
                    raise RuntimeError(f"nondeterministic {mode} MIR for {case.name}")
                reference_mir[mode] = mir
                if iteration >= 0:
                    entries[mode]["samples"].append(sample)
        for mode, compile_command in modes.items():
            entries[mode].update(statistics_for(entries[mode]["samples"]))
            entries[mode]["mir"] = mir_metrics(reference_mir[mode])
            if not args.no_verify:
                object_file = case_dir / mode / "fixture.o"
                binary = case_dir / mode / "fixture"
                command([*compile_command, "--cache", "-c", "-o", str(object_file), str(source)],
                        args.timeout)
                # The fixtures have no runtime dependencies. Use the host C
                # startup instead of rebuilding and linking bundled libc for
                # every case; neither linking nor execution is timed here.
                command([args.host_cc, "-no-pie", str(object_file), "-o", str(binary)], args.timeout)
                command([str(binary)], args.timeout)
                entries[mode]["object_bytes"] = object_file.stat().st_size

        baseline = entries["baseline"]
        candidate = entries.get("qml")
        if candidate:
            changed = reference_mir["baseline"] != reference_mir["qml"]
            if args.assert_rewrites and changed != case.rewrite_expected:
                raise RuntimeError(f"unexpected canonicalization result for {case.name}: "
                                   f"MIR changed={changed}, expected={case.rewrite_expected}")
            candidate["changed_mir"] = changed
            print(f"{case.name:24} {baseline['wall_ns_median'] / 1e6:9.3f} "
                  f"{candidate['wall_ns_median'] / 1e6:9.3f} "
                  f"{candidate['phases_ns_median']['qml_rewrite'] / 1e6:9.3f} "
                  f"{baseline['mir']['alu']:7}→{candidate['mir']['alu']:<7}", flush=True)
        else:
            print(f"{case.name:24} {baseline['wall_ns_median'] / 1e6:9.3f} "
                  f"{'—':>9} {'—':>9} {baseline['mir']['alu']:>15}", flush=True)
        report["workloads"][case.name] = entries
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path, required=True, help="Prolific-enabled Cephyr binary")
    parser.add_argument("--output", type=Path, required=True, help="new directory for artifacts")
    parser.add_argument("--target", default="auto", help="override the native target triple")
    parser.add_argument("--host-cc", default="cc", help="driver used for native verification links")
    parser.add_argument("--config", type=Path, help="config for both runs; must not enable a default profile")
    parser.add_argument("--candidate-arg", action="append", metavar="ARG",
                        help="repeatable candidate-only argument, e.g. --candidate-arg=--profile=fast")
    parser.add_argument("--fixture", default=".", metavar="REGEX", help="select workload names")
    parser.add_argument("--repeat", type=int, default=5, help="measured samples per mode")
    parser.add_argument("--warmup", type=int, default=1, help="discarded samples per mode")
    parser.add_argument("--timeout", type=float, default=60, help="seconds per subprocess")
    parser.add_argument("--no-verify", action="store_true", help="do not link and run generated binaries")
    parser.add_argument("--baseline-only", action="store_true", help="profile without selecting QML")
    parser.add_argument("--assert-rewrites", action="store_true",
                        help="assert each workload's expected MIR changes for built-in canonicalization")
    args = parser.parse_args()
    if args.repeat < 1 or args.warmup < 0 or not (0 < args.timeout < float("inf")):
        parser.error("repeat and timeout must be positive, and warmup nonnegative")
    if args.baseline_only and (args.candidate_arg or args.assert_rewrites):
        parser.error("--baseline-only excludes candidate arguments and rewrite assertions")
    try:
        benchmark(args)
    except (OSError, KeyError, ValueError, RuntimeError, re.error, subprocess.TimeoutExpired) as exc:
        print(f"benchmark: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
