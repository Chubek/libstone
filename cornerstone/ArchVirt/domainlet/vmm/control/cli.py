"""Command-line entrypoint for the VMM."""

from __future__ import annotations

import argparse

from domainlet.vmm.control.builder import build_vmm
from domainlet.vmm.control.config import VmConfig


def main(argv: list[str] | None = None) -> int:
    """Build a VM from CLI arguments."""

    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", required=True)
    parser.add_argument("--initrd")
    parser.add_argument("--cmdline", default="console=ttyS0")
    parser.add_argument("--memory", type=int, default=128 * 1024 * 1024)
    parser.add_argument("--vcpus", type=int, default=1)
    args = parser.parse_args(argv)
    assembly = build_vmm(VmConfig(args.kernel, args.initrd,
                         args.cmdline, args.memory, args.vcpus))
    if assembly.lifecycle is not None:
        assembly.lifecycle.shutdown()
    return 0
