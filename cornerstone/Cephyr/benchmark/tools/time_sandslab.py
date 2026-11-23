"""Time warmed -c invocations after benchmark.py (includes the late pass)."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("results", type=Path, help="benchmark.py output directory")
parser.add_argument("--pairs", type=int, default=21)
args = parser.parse_args()
if args.pairs < 1:
    parser.error("--pairs must be positive")
results = args.results.resolve()
summary = json.loads((results / "summary.json").read_text())
for fixture in summary["fixtures"]:
    source = Path(__file__).resolve().parents[1] / "fixtures" / (fixture + ".c")
    timings = {"baseline": [], "candidate": []}
    for i in range(args.pairs + 2):
        for mode in (timings if i % 2 == 0 else reversed(timings)):
            cmd = [summary["compiler"], "--target=" + summary["target"],
                   "--config=" + str(results / "Cephyr.toml"),
                   *([] if summary["factir_enabled"] else ["--no-factir"]),
                   *summary["modes"][mode], "-c", str(source), "-o", str(results / "timed.o")]
            start = time.perf_counter_ns()
            subprocess.run(cmd, check=True)
            if i >= 2:
                timings[mode].append((time.perf_counter_ns() - start) / 1e6)
    delta = [b - a for a, b in zip(timings["baseline"], timings["candidate"])]
    print(fixture, {k: round(statistics.median(v), 3) for k, v in timings.items()},
          "paired delta ms:", round(statistics.median(delta), 3))
    (results / (fixture + "-object-ms.json")).write_text(json.dumps(timings) + "\n")
