"""Run freestanding Cephyr C11 and QML regressions as ArchVirt guests."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from test_qml_cfg import CFG, PIPELINES, RECORD, initializer, inputs, reference
from test_qml_driver import (
    DIFFERENTIAL, differential_expected, differential_initializer, differential_inputs,
)


FIXTURES = Path(__file__).resolve().parent / "fixtures"
ARCHVIRT = FIXTURES.parents[2] / "ArchVirt"
GUEST = FIXTURES / "archvirt"
MAILBOX = 0x500000
MAGIC = 0x4345504859525631
HEADER = struct.Struct("<QQQ")
MACHINES = ("x86_64", "aarch64", "riscv64")
C11 = ("records/compound_literals.c", "records/anonymous.c",
       "c11_character_constants.c", "c11_record_static_assertions.c", "c11_string_literals.c")
DIFFERENTIAL_PIPELINES = (*PIPELINES, ("canonicalize", "strength_reduce"))


def run(args, TestVM, work):
    env = dict(os.environ, LC_ALL="C", XDG_CACHE_HOME=str(work / "cache"))
    env.pop("CEPHYR_CONFIG", None)

    def command(*argv):
        subprocess.run(argv, cwd=work, env=env, check=True, timeout=120)

    cases = list(inputs())
    expected = [reference(case) for _, case in cases]
    (work / "cfg").mkdir()
    (work / "cfg" / "inputs.inc").write_text("".join(initializer(case) for _, case in cases),
                                            encoding="ascii")
    simple_cases = list(differential_inputs())
    (work / "simple").mkdir()
    (work / "simple" / "inputs.inc").write_text("".join(
        differential_initializer(case, result)
        for case, result in zip(simple_cases, differential_expected(simple_cases), strict=True)
    ), encoding="ascii")
    with TestVM(str(args.archvirt.resolve())) as vm:
        for machine in args.machine or MACHINES:
            cc = (args.cephyr.resolve(), f"--target={machine}-linux-gnu", "-std=c11",
                  "-ffreestanding", "-fno-pic")
            command(*cc, "-c", GUEST / "start.S", "-o", "start.o")
            command(*cc, "-c", f"-DARCHVIRT_MAILBOX={MAILBOX}", GUEST / "guest.c",
                    "-o", "guest.o")

            def boot(label, objects, payload_size=0):
                image = work / "guest.elf"
                command(args.lld, "-static", "-T", GUEST / "guest.ld", "start.o", "guest.o",
                        *objects, "-o", image)
                vm.boot(machine=machine, backend="software", kernel=str(image),
                        max_instructions=5_000_000)
                status = vm.wait(timeout=30)
                if status["state"] != "halted" or status["result"]["code"] != 0:
                    raise AssertionError(f"{machine} {label}: {status}")
                magic, result, size = HEADER.unpack(vm.read_memory(MAILBOX, HEADER.size))
                if (magic, result, size) != (MAGIC, 0, payload_size):
                    raise AssertionError(
                        f"{machine} {label}: mailbox {(hex(magic), result, size)}, "
                        f"expected {(hex(MAGIC), 0, payload_size)}"
                    )
                return vm.read_memory(MAILBOX + HEADER.size, size) if size else b""

            # The larger record fixture also emits CQO on x86.
            fixtures = C11 if machine == "x86_64" else (*C11, "records/native.c")
            for fixture in fixtures:
                command(*cc, "-c", FIXTURES / fixture, "-o", "test.o")
                boot(f"C11 {fixture}", ("test.o",))
            command(*cc, "-c", DIFFERENTIAL / "driver.c", "-I", work / "simple", "-o", "driver.o")
            for phases in DIFFERENTIAL_PIPELINES:
                label = "+".join(phases) or "baseline"
                flags = tuple(f"--qml-pass={phase}" for phase in phases)
                command(*cc, *flags, "-c", DIFFERENTIAL / "kernel.c", "-o", "test.o")
                boot(f"QML {label}", ("test.o", "driver.o"))
            print(f"{machine}: {len(fixtures)} C11 guests and {len(DIFFERENTIAL_PIPELINES)} "
                  f"QML variants passed ({len(simple_cases)} oracle cases each)", flush=True)

            # The x86 software adapter does not implement the CQO/IDIV
            # sequence emitted for this fixture's signed remainder.
            if machine == "x86_64":
                print(f"{machine}: skipped QML CFG guest (requires CQO/IDIV)", flush=True)
                continue
            command(*cc, "-c", CFG / "driver.c", "-I", work / "cfg", "-o", "driver.o")
            for phases in PIPELINES:
                label = "+".join(phases) or "baseline"
                flags = tuple(f"--qml-pass={phase}" for phase in phases)
                command(*cc, *flags, "-c", CFG / "kernel.c", "-o", "test.o")
                observations = boot(f"QML {label}", ("test.o", "driver.o"),
                                    len(cases) * RECORD.size)
                for (case_name, _), actual, wanted in zip(
                        cases, RECORD.iter_unpack(observations), expected, strict=True):
                    if actual != wanted:
                        raise AssertionError(f"{machine} QML {label} {case_name}: "
                                             f"got {actual}, expected {wanted}")
            print(f"{machine}: {len(PIPELINES)} QML CFG variants passed "
                  f"({len(cases)} oracle cases each)", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cephyr", type=Path)
    parser.add_argument("--archvirt", required=True, type=Path)
    parser.add_argument("--lld", default="ld.lld")
    parser.add_argument("--machine", action="append", choices=MACHINES)
    args = parser.parse_args()
    sys.path.insert(0, str(ARCHVIRT / "tools"))
    from archvirt_ipc import TestVM

    # This prefix also leaves enough space under the Unix socket path limit.
    with tempfile.TemporaryDirectory(prefix="cephyr-av-") as temporary:
        run(args, TestVM, Path(temporary).resolve())


if __name__ == "__main__":
    main()
