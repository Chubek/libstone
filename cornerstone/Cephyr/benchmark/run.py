#!/usr/bin/env python3
"""Compare Cephyr MIR optimizers using Prolific and checked native objects."""

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


ALU = frozenset(("add", "sub", "mul", "and", "or", "xor", "shl", "lshr", "ashr",
                 "div_s", "div_u", "rem_s", "rem_u"))
INTEGER_PIPELINES = {
    "canonicalize": ("canonicalize",),
    "strength_reduce": ("strength_reduce",),
    "canonicalize+strength_reduce": ("canonicalize", "strength_reduce"),
    "integer_reduce": ("integer_reduce",),
    "all": ("canonicalize", "strength_reduce", "integer_reduce"),
}
NATIVE_HOSTS = {
    "elf64-x86-64": {"x86_64", "amd64"},
    "elf64-littleaarch64": {"aarch64", "arm64"},
    "elf64-littleriscv": {"riscv64"},
}


def capture(argv: list[str], timeout: float) -> tuple[str, int]:
    start = time.perf_counter_ns()
    completed = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    elapsed = time.perf_counter_ns() - start
    if completed.returncode:
        raise RuntimeError(f"exit {completed.returncode}: {argv!r}\n"
                           f"{completed.stdout}{completed.stderr}")
    return completed.stdout, elapsed


def object_metrics(objdump: str, object_file: Path, timeout: float) -> dict:
    output, _ = capture([objdump, "--section-headers", "--disassemble", "--section=.text",
                         "--no-show-raw-insn", "--disassemble-zeroes", str(object_file)], timeout)
    object_file.with_suffix(".objdump").write_text(output, encoding="utf-8")
    formats = re.findall(r"file format (\S+)$", output, re.M)
    sizes = re.findall(r"^\s*\d+\s+\.text\s+([\da-fA-F]+)\s", output, re.M)
    if len(formats) != 1 or len(sizes) != 1:
        raise RuntimeError(f"expected one .text section in {object_file}")
    opcodes = Counter(op for op in re.findall(r"^[ \t]*[\da-fA-F]+:[ \t]+(\S+)", output, re.M)
                      if not op.startswith("."))
    if not opcodes or any(op.startswith("<") for op in opcodes):
        raise RuntimeError(f"missing or undecodable instructions in {object_file}")
    return {"format": formats[0], "file_bytes": object_file.stat().st_size,
            "text_bytes": int(sizes[0], 16), "instructions": sum(opcodes.values()),
            "opcodes": dict(sorted(opcodes.items()))}


def profile(path: Path, *, mode: str, suite: str) -> dict:
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
    if names["compile"] != 1 or names["qml_rewrite"] != int(mode not in ("baseline", "blackstone")):
        raise RuntimeError(f"missing or duplicate expected compiler phases: {path}")
    if suite == "blackstone" and names["blackstone"] != int(mode == "blackstone"):
        raise RuntimeError(f"Blackstone did not follow the selected config: {path}")
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
        elif op in ALU or op == "trunc":
            opcodes[op] += 1
    alu = {op: n for op, n in sorted(opcodes.items()) if op in ALU}
    return {"bytes": len(mir), "instructions": instruction_count,
            "alu": sum(alu.values()), "alu_opcodes": alu, "trunc": opcodes["trunc"]}


def measure(base: list[str], source: Path, directory: Path,
            iteration: str, mode: str, suite: str, timeout: float) -> tuple[dict, bytes]:
    trace = directory / (iteration + ".prolific.json")
    mir_path = directory / (iteration + ".mir")
    _, wall = capture([*base, "--no-cache", f"--moonstone-profile={trace}",
                       "--emit-mir", "-o", str(mir_path), str(source)], timeout)
    return {"wall_ns": wall, "phases_ns": profile(trace, mode=mode, suite=suite)}, mir_path.read_bytes()


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


def verification(args: argparse.Namespace, object_format: str) -> tuple[str, list[str], str]:
    if args.no_verify:
        return "compile-only", [], "--no-verify requested"
    if args.runner:
        return "runner", [args.runner, *args.runner_arg], ""
    if platform.machine().lower() in NATIVE_HOSTS.get(object_format, set()):
        return "native", [], ""
    return "compile-only", [], "target is not native and no --runner supplied"


def round_order(modes: list[str], iteration: int) -> list[str]:
    cycle, rotation = divmod(iteration, len(modes))
    order = modes if cycle % 2 == 0 else modes[::-1]
    return order[rotation:] + order[:rotation]


def benchmark(args: argparse.Namespace) -> None:
    active = [case for case in workloads(args.suite) if re.search(args.fixture, case.name)]
    if not active:
        raise RuntimeError(f"no fixtures matched {args.fixture!r}")
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    config = args.config.resolve() if args.config else out / "Cephyr.toml"
    if not args.config:
        # Explicit config avoids ambient Cephyr.toml / CEPHYR_CONFIG settings.
        baseline = '[paths]\ncache_path = ".cache"\n[blackstone]\nenabled = false\n'
        if args.suite != "blackstone":
            # Otherwise FactIR also removes add-zero, masking the QML rewrite
            # whose presence/absence the legacy benchmark asserts.
            baseline += "[factir]\nenabled = false\n"
        config.write_text(baseline, encoding="utf-8")
    common = [str(args.cephyr.resolve()), f"--target={args.target}"]
    modes = {"baseline": []}
    if not args.baseline_only:
        if args.suite == "blackstone":
            # Exercise the actual default. Only the baseline sets enabled=false.
            default = out / "default.toml"
            default.write_text('[paths]\ncache_path = ".cache"\n', encoding="utf-8")
            modes["blackstone"] = [f"--config={default}"]
        elif args.candidate_arg:
            modes["custom"] = args.candidate_arg
        else:
            pipelines = INTEGER_PIPELINES if args.suite == "integer-reduce" else {
                "canonicalize": ("canonicalize",),
            }
            modes.update({name: [f"--qml-pass={phase}" for phase in phases]
                          for name, phases in pipelines.items()})
    for mode, flags in modes.items():
        if mode != "blackstone":
            flags.insert(0, f"--config={config}")
    report = {
        "schema": "cephyr.benchmark.v2",
        "compiler": str(args.cephyr.resolve()),
        "compiler_version": capture([common[0], "--version"], args.timeout)[0].strip(),
        "host": {
            "system": platform.system(),
            "machine": platform.machine(),
            "release": platform.release(),
            "cpus": sorted(os.sched_getaffinity(0)),
        },
        "target_argument": args.target,
        "config": str(config),
        "suite": args.suite,
        "mode_arguments": modes,
        "objdump": args.objdump if not args.mir_only else None,
        "objdump_version": (
            capture([args.objdump, "--version"], args.timeout)[0].strip().split("\n\n", 1)[0]
            if not args.mir_only else None
        ),
        "repeat": args.repeat,
        "warmup": args.warmup,
        "execution_repeat": args.execution_repeat,
        "workloads": {},
    }

    phase = "blackstone" if args.suite == "blackstone" else "qml_rewrite"
    print(f"{'workload':25} {'mode':29} {'MIR ms':>8} {'pass ms':>8} "
          f"{'insns (Δ)':>15} {'.text B (Δ)':>16}  verification")
    object_format = None
    start_object = out / "start.o"
    mode_names = list(modes)
    for case in active:
        case_dir = out / case.name
        case_dir.mkdir()
        source = case_dir / (case.name + ".c")
        source.write_text(case.source, encoding="ascii")
        for mode in modes:
            (case_dir / mode).mkdir()
        # Rotate which mode starts each serial round; reverse direction after
        # each full cycle. Warmup rounds do not enter reported summaries.
        entries = {mode: {"samples": []} for mode in modes}
        reference_mir = {}
        for iteration in range(-args.warmup, args.repeat):
            for mode in round_order(mode_names, iteration):
                label = f"warmup{-iteration:02}" if iteration < 0 else f"run{iteration + 1:02}"
                sample, mir = measure([*common, *modes[mode]], source, case_dir / mode, label,
                                      mode, args.suite, args.timeout)
                if mode in reference_mir and reference_mir[mode] != mir:
                    raise RuntimeError(f"nondeterministic {mode} MIR for {case.name}")
                reference_mir[mode] = mir
                if iteration >= 0:
                    entries[mode]["samples"].append(sample)
        driver_object = case_dir / "driver.o"
        if case.driver is not None:
            driver = case_dir / "driver.c"
            driver.write_text(case.driver, encoding="ascii")
        for mode, flags in modes.items():
            entry = entries[mode]
            entry.update(statistics_for(entry["samples"]))
            entry["mir"] = mir_metrics(reference_mir[mode])
            entry["changed_mir"] = reference_mir["baseline"] != reference_mir[mode]
            expected = case.expected_mir_changes.get(mode)
            if args.assert_rewrites and expected is not None and entry["changed_mir"] != expected:
                raise RuntimeError(f"unexpected {mode} rewrite result for {case.name}: "
                                   f"MIR changed={entry['changed_mir']}, expected={expected}")
            if args.assert_rewrites and case.removed_integer_ops:
                op, count = case.removed_integer_ops
                before = entries["baseline"]["mir"]["alu_opcodes"].get(op, 0)
                after = entry["mir"]["alu_opcodes"].get(op, 0)
                expected_removed = count if mode in ("integer_reduce", "all") else 0
                if before - after != expected_removed:
                    raise RuntimeError(f"{case.name}/{mode}: expected removal of {expected_removed} "
                                       f"{op} operations, found {before - after}")
            entry["verification"] = "mir-only"
            entry["verification_reason"] = "--mir-only requested"
            metrics = ["—", "—"]
            if not args.mir_only:
                object_file = case_dir / mode / "fixture.o"
                capture([*common, *flags, "--cache", "-c", "-o", str(object_file), str(source)],
                         args.timeout)
                obj = entry["object"] = object_metrics(args.objdump, object_file, args.timeout)
                if object_format is not None and object_format != obj["format"]:
                    raise RuntimeError(f"unexpected object format {obj['format']} in {object_file}")
                object_format = obj["format"]
                entry["object_delta_vs_baseline"] = {
                    key: obj[key] - entries["baseline"]["object"][key]
                    for key in ("file_bytes", "text_bytes", "instructions")
                }
                metrics = [f"{obj[key]} ({entry['object_delta_vs_baseline'][key]:+})"
                           for key in ("instructions", "text_bytes")]
                status, runner, reason = verification(args, object_format)
                if status != "compile-only":
                    if not start_object.exists():
                        start = Path(__file__).resolve().parent / "fixtures" / "start.S"
                        capture([*common, *modes["baseline"], "-c", str(start), "-o", str(start_object)], args.timeout)
                    link_inputs = [str(start_object), str(object_file)]
                    if case.driver is not None:
                        if not driver_object.exists():
                            capture([*common, *modes["baseline"], "--cache", "-c", str(driver),
                                     "-o", str(driver_object)], args.timeout)
                        link_inputs.append(str(driver_object))
                    binary = case_dir / mode / "fixture"
                    capture([*common, *flags, "-nostdlib", "-static", *link_inputs,
                             "-o", str(binary)], args.timeout)
                    capture([*runner, str(binary)], args.timeout)
                entry["verification"] = status
                entry["verification_reason"] = reason or None
                entry["runner"] = runner
            elapsed_ns = entry["phases_ns_median"].get(phase)
            elapsed_ms = f"{elapsed_ns / 1e6:.3f}" if elapsed_ns is not None else "—"
            status = entry["verification"]
            if entry["verification_reason"]:
                status += f" ({entry['verification_reason']})"
            print(f"{case.name:25} {mode:29} {entry['wall_ns_median'] / 1e6:8.3f} "
                  f"{elapsed_ms:>8} {metrics[0]:>15} {metrics[1]:>16}  {status}", flush=True)
        if args.execution_repeat:
            if entries["baseline"]["verification"] != "native":
                raise RuntimeError("--execution-repeat requires native verification")
            for iteration in range(-args.warmup, args.execution_repeat):
                for mode in round_order(mode_names, iteration):
                    _, ns = capture([str(case_dir / mode / "fixture")], args.timeout)
                    if iteration >= 0:
                        entries[mode].setdefault("execution_ns", []).append(ns)
            for mode, entry in entries.items():
                elapsed = entry["execution_ns_median"] = statistics.median(entry["execution_ns"])
                print(f"  {mode} native process median: {elapsed / 1e6:.3f} ms")
        report["workloads"][case.name] = entries
    report["object_format"] = object_format
    (out / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cephyr", type=Path, required=True, help="Prolific-enabled Cephyr binary")
    parser.add_argument("--output", type=Path, required=True, help="new directory for artifacts")
    parser.add_argument("--target", default="auto", help="override the native target triple")
    parser.add_argument("--config", type=Path, help="config for all modes; must not enable a default profile")
    parser.add_argument("--suite", choices=("canonicalize", "integer-reduce", "blackstone"), default="canonicalize")
    parser.add_argument("--candidate-arg", action="append", metavar="ARG",
                        help="repeatable candidate-only argument, e.g. --candidate-arg=--profile=fast")
    parser.add_argument("--fixture", default=".", metavar="REGEX", help="select workload names")
    parser.add_argument("--repeat", type=int, default=5, help="measured samples per mode")
    parser.add_argument("--warmup", type=int, default=1, help="discarded samples per mode")
    parser.add_argument("--execution-repeat", type=int, default=0,
                        help="measure native binary wall time including process startup (default: off)")
    parser.add_argument("--timeout", type=float, default=60, help="seconds per subprocess")
    parser.add_argument("--objdump", default="llvm-objdump", help="LLVM disassembler with target support")
    parser.add_argument("--runner", help="explicit cross-target runner, e.g. qemu-aarch64")
    parser.add_argument("--runner-arg", action="append", default=[], help="repeatable runner argument")
    parser.add_argument("--no-verify", action="store_true", help="compile objects but do not link or run")
    parser.add_argument("--mir-only", action="store_true", help="skip object generation and execution")
    parser.add_argument("--baseline-only", action="store_true", help="profile only the comparison baseline")
    parser.add_argument("--assert-rewrites", action="store_true",
                        help="assert each workload's expected MIR changes for built-in pipelines")
    args = parser.parse_args()
    if args.repeat < 1 or min(args.warmup, args.execution_repeat) < 0 or not (0 < args.timeout < float("inf")):
        parser.error("repeat and timeout must be positive, and warmup nonnegative")
    if args.baseline_only and (args.candidate_arg or args.assert_rewrites):
        parser.error("--baseline-only excludes candidate arguments and rewrite assertions")
    if args.candidate_arg and args.assert_rewrites:
        parser.error("--assert-rewrites requires the suite's built-in pipelines")
    if args.suite == "blackstone" and (args.candidate_arg or args.config):
        parser.error("--suite blackstone supplies its own paired TOML configurations")
    if args.runner_arg and not args.runner:
        parser.error("--runner-arg requires --runner")
    if args.runner and (args.no_verify or args.mir_only):
        parser.error("--runner requires linking and verification")
    if args.execution_repeat and (args.runner or args.no_verify or args.mir_only):
        parser.error("--execution-repeat requires native verification")
    try:
        benchmark(args)
    except (OSError, KeyError, ValueError, RuntimeError, re.error, subprocess.TimeoutExpired) as exc:
        print(f"benchmark: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
