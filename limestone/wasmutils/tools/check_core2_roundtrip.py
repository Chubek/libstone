#!/usr/bin/env python3
"""Round-trip upstream WebAssembly Core 2.0 text modules through the CLI tools.

Example (from the repository root; needs Python 3.8+, Git, and Node.js):

    git clone --depth 1 --branch wg-2.0 https://github.com/WebAssembly/spec.git spec-core2
    limestone/wasmutils/tools/check_core2_roundtrip.py spec-core2 \
        build/limestone-wasmutils-v1/wasmutils build/core2-roundtrip

Tag wg-2.0, commit fffc6e12fa454e475455a7b58d3b5dc343980c10, contains 1532
top-level text modules under test/core. WAST assertions, scripts, and quoted
or binary modules are not round-trip inputs. report.json records provenance,
stage totals, and the source file and line for each extracted fixture.

Add --check-invalid to probe top-level assert_invalid text modules separately.
It builds a small helper against the neighboring libwasmutils.a with CXX (or
c++) and sends the inputs it can encode, without enforcing validation, to Node.
Parser/encoder rejections are not counted as validator rejections. Node results
are observations, not an assertion that its enabled features match Core 2.0.
The negative phase fails only if the Core validator accepts a fixture or a
probe fails to run; parser skips and Node disagreements remain in the report.
"""

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


TOKEN = re.compile(rb'\(;|;\)|;;[^\r\n]*|"(?:\\[\s\S]|[^"\\])*"|[()]')
COMMENT = re.compile(rb'\(;|;\)')
MODULE = re.compile(rb'\(module(?:\s|[()])')
NON_TEXT = re.compile(rb'\(module\s+(?:\$\S+\s+)?(?:quote|binary)\b')
ASSERT_INVALID = re.compile(rb'\(assert_invalid(?=\s|\()')
NODE_FEATURE_OR_LIMIT = re.compile(
    r"unsupported|not (?:supported|enabled)|disable[d]?|enable with|--(?:experimental|wasm)"
    r"|(?:implementation|internal|engine) limit|too many (?:memories|tables|locals|functions)",
    re.IGNORECASE,
)

NODE_CHECK = r"""
const fs = require('fs');
const files = JSON.parse(fs.readFileSync(0, 'utf8'));
const failures = [];
for (const [id, stage, path] of files) {
  try { new WebAssembly.Module(fs.readFileSync(path)); }
  catch (error) { failures.push([id, stage, error.name, error.message]); }
}
process.stdout.write(JSON.stringify(failures));
"""

INVALID_PROBE = r"""
#include "WASM-Validate.hpp"
#include "WAT-Module.hpp"

#include <fstream>
#include <iostream>
#include <iterator>

using limestone::wasmutils::Error;
namespace core = limestone::wasmutils::core;

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    try {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) throw std::runtime_error("could not open fixture");
        const std::string text((std::istreambuf_iterator<char>(in)), {});
        core::Module module;
        try {
            module = core::assemble_module(text);
        } catch (const Error& error) {
            std::cerr << "parse byte " << error.offset << ": " << error.what() << '\n';
            return 2;
        }
        auto valid = true;
        try {
            core::validate_module(module);
        } catch (const Error& error) {
            std::cerr << "validate byte " << error.offset << ": " << error.what() << '\n';
            valid = false;
        }
        try {
            const auto bytes = core::encode_module(module);
            std::cout.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            if (!std::cout) throw std::runtime_error("could not write binary");
        } catch (const Error& error) {
            std::cerr << "encode byte " << error.offset << ": " << error.what() << '\n';
            return valid ? 5 : 4;
        }
        return valid ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "probe: " << error.what() << '\n';
        return 1;
    }
}
"""


def forms(data, pos=0):
    """Yield byte ranges of top-level S-expressions, ignoring strings and comments."""
    depth = start = 0
    while (match := TOKEN.search(data, pos)):
        pos = match.end()
        token = match.group()
        if token == b"(;":
            comments = 1
            while comments and (match := COMMENT.search(data, pos)):
                pos = match.end()
                comments += 1 if match.group() == b"(;" else -1
        elif token == b"(":
            if not depth:
                start = match.start()
            depth += 1
        elif token == b")":
            if not depth:
                return
            depth -= 1
            if not depth:
                yield start, pos


def extract(core, invalid=False):
    for path in sorted(core.rglob("*.wast")):
        data = path.read_bytes()
        for start, end in forms(data):
            form = data[start:end]
            if invalid:
                assertion = ASSERT_INVALID.match(form)
                if not assertion:
                    continue
                child = next(forms(form, assertion.end()), None)
                if child is None:
                    continue
                begin, finish = child
                form = form[begin:finish]
                start += begin
            if MODULE.match(form) and not NON_TEXT.match(form):
                yield str(path.relative_to(core)), data.count(b"\n", 0, start) + 1, form


def run_roundtrip(job, tools, output):
    ident, (source, line, wat) = job
    paths = {suffix: output / f"{ident:04}.{suffix}" for suffix in
             ("wat", "wasm", "printed.wat", "reassembled.wasm")}
    paths["wat"].write_bytes(wat)
    checked = []
    artifacts = []
    issues = []
    commands = (
        ("assemble", ["wasm-assemble", paths["wat"]], "wasm"),
        ("disassemble", ["wasm-disassemble", "--wat", paths["wasm"]], "printed.wat"),
        ("reassemble", ["wasm-assemble", paths["printed.wat"]], "reassembled.wasm"),
    )
    for stage, command, target in commands:
        completed = subprocess.run([tools / command[0], *command[1:]], capture_output=True)
        if completed.returncode:
            issues.append([stage, completed.stderr.decode("utf-8", "replace").strip()])
            break
        paths[target].write_bytes(completed.stdout)
        checked.append(stage)
        if stage == "assemble":
            artifacts.append([ident, "node-original", str(paths[target])])
        if stage == "reassemble":
            artifacts.append([ident, "node-roundtrip", str(paths[target])])
            original = paths["wasm"].read_bytes()
            if original == completed.stdout:
                checked.append("identical-bytes")
            else:
                offset = next((i for i, (a, b) in enumerate(zip(original, completed.stdout))
                               if a != b), min(len(original), len(completed.stdout)))
                issues.append(["identical-bytes",
                               f"first difference at {offset}; sizes {len(original)} and {len(completed.stdout)}"])
    return {"id": ident, "source": source, "line": line, "passed": checked,
            "failures": issues, "artifacts": artifacts}


def check_with_node(files):
    node = subprocess.run(["node", "-e", NODE_CHECK], input=json.dumps(files), capture_output=True,
                          text=True, check=True)
    return {(ident, stage): (kind, message) for ident, stage, kind, message in json.loads(node.stdout)}


def run_invalid(job, probe, output):
    ident, (source, line, wat) = job
    text = output / f"{ident:04}.wat"
    text.write_bytes(wat)
    completed = subprocess.run([probe, text], capture_output=True)
    status = completed.returncode
    outcomes = {
        0: "validator-accepted", 2: "parser-rejected", 3: "validator-rejected",
        4: "validator-and-encoder-rejected", 5: "validator-accepted-encoder-rejected",
    }
    result = {"id": ident, "source": source, "line": line, "fixture": str(text),
              "outcome": outcomes.get(status, "probe-error"),
              "diagnostic": completed.stderr.decode("utf-8", "replace").strip()}
    if status in (0, 3):
        binary = text.with_suffix(".wasm")
        binary.write_bytes(completed.stdout)
        result["binary"] = str(binary)
    return result


def check_invalid(core, tools, output, jobs, baseline_node_supported):
    output = output / "invalid"
    output.mkdir(exist_ok=True)
    header_path = Path(__file__).resolve().parents[1]
    cpp = output / "probe.cpp"
    cpp.write_text(INVALID_PROBE.lstrip(), encoding="utf-8")
    probe = output / "probe"
    subprocess.run([*shlex.split(os.environ.get("CXX", "c++")),
                    *shlex.split(os.environ.get("CXXFLAGS", "")),
                    "-std=c++20", "-I", header_path, cpp, tools / "libwasmutils.a", "-o", probe],
                   check=True)
    sources = list(extract(core, invalid=True))
    with ThreadPoolExecutor(max_workers=jobs) as pool:
        results = list(pool.map(lambda job: run_invalid(job, probe, output), enumerate(sources)))
    files = [[result["id"], "node-invalid", result["binary"]]
             for result in results if "binary" in result]
    node_errors = check_with_node(files)
    outcomes, node_outcomes = Counter(), Counter()
    hard_failures = 0
    for result in results:
        ident = result["id"]
        outcome = result["outcome"]
        outcomes[outcome] += 1
        if "binary" not in result:
            node_stage = "not-run"
        else:
            error = node_errors.get((ident, "node-invalid"))
            if error is None:
                node_stage = "accepted"
            else:
                kind, message = error
                if kind != "CompileError":
                    node_stage = "inconclusive-engine-error"
                elif not baseline_node_supported or NODE_FEATURE_OR_LIMIT.search(message):
                    node_stage = "inconclusive-feature-or-limit"
                else:
                    node_stage = "rejected"
                result["node_diagnostic"] = message
        result["node_outcome"] = node_stage
        node_outcomes[node_stage] += 1
        if outcome.startswith("validator-accepted") or outcome == "probe-error":
            hard_failures += 1
            print(f"{result['source']}:{result['line']} (invalid/{ident:04}.wat) "
                  f"[{outcome}] {result['diagnostic']}")
        elif node_stage == "accepted":
            print(f"{result['source']}:{result['line']} (invalid/{ident:04}.wat) "
                  f"[Node accepted; {outcome}] {result['diagnostic']}")
    print(f"Negative text fixtures: {len(sources)}")
    for name, total in sorted(outcomes.items()):
        print(f"  {name}: {total}")
    for name, total in sorted(node_outcomes.items()):
        print(f"  Node {name}: {total}")
    return {"count": len(sources), "outcomes": dict(outcomes),
            "node_outcomes": dict(node_outcomes), "hard_failures": hard_failures, "results": results}


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("spec", type=Path, help="checkout of WebAssembly/spec (tag wg-2.0)")
    parser.add_argument("tools", type=Path, help="directory containing wasm-assemble and wasm-disassemble")
    parser.add_argument("output", type=Path, help="directory for fixtures, binary/text results and report.json")
    parser.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    parser.add_argument("--check-invalid", action="store_true",
                        help="also check assert_invalid text modules (requires libwasmutils.a and a C++20 compiler)")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    root = args.spec / "test" / "core"
    tools = args.tools.resolve()
    if args.check_invalid and not (tools / "libwasmutils.a").is_file():
        parser.error("--check-invalid requires libwasmutils.a in the tools directory")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    sources = list(extract(root))
    if not sources:
        parser.error(f"no text modules found in {root}")
    revision = subprocess.check_output(["git", "-C", str(args.spec), "rev-parse", "HEAD"], text=True).strip()
    node_version = subprocess.check_output(["node", "--version"], text=True).strip()
    stages = ("assemble", "disassemble", "reassemble", "identical-bytes", "node-original", "node-roundtrip")

    with ThreadPoolExecutor(max_workers=args.jobs) as pool:
        results = list(pool.map(lambda job: run_roundtrip(job, tools, output), enumerate(sources)))

    files = [artifact for result in results for artifact in result["artifacts"]]
    node_errors = check_with_node(files)
    for ident, stage, _ in files:
        error = node_errors.get((ident, stage))
        if error is None:
            results[ident]["passed"].append(stage)
        else:
            results[ident]["failures"].append([stage, error[1]])

    passed = Counter(stage for result in results for stage in result["passed"])
    failed = Counter(stage for result in results for stage, _ in result["failures"])
    report = {"revision": revision, "node": node_version, "tools": str(tools),
              "count": len(sources), "passed": dict(passed), "failed": dict(failed),
              "results": results}
    print(f"WebAssembly/spec {revision}, {node_version}, {len(sources)} modules")
    for stage in stages:
        print(f"{stage}: {passed[stage]} passed, {failed[stage]} failed")
    for result in results:
        for stage, error in result["failures"]:
            print(f"{result['source']}:{result['line']} ({result['id']:04}.wat) [{stage}] {error}")
    if args.check_invalid:
        report["invalid"] = check_invalid(root, tools, output, args.jobs, passed["node-original"] == len(sources))
    report_path = output / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {report_path}")
    return bool(failed or report.get("invalid", {}).get("hard_failures"))


if __name__ == "__main__":
    raise SystemExit(main())
