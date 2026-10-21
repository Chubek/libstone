#!/usr/bin/env python3
"""Reproduce the native-hardening source overlay for the ignored dependency."""
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import tarfile


FILES = (
    "CMakeLists.txt",
    "LICENSE",
    "README.md",
    "IR/Peephole.cpp",
    "Passlib/PassSupport.hpp",
    "docs/manual/16-cli.md",
    "docs/native-hardening.md",
    "include/Jiterati.hpp",
    "include/Jiterati-Asm.hpp",
    "include/Jiterati-Bytecode.hpp",
    "include/Jiterati-Native.hpp",
    "src/Bytecode.cpp",
    "src/Elf.hpp",
    "src/Jiterati-CLI.cpp",
    "src/Native.cpp",
    "src/TemporaryDirectory.hpp",
    "tests/test_binary_hardening.cpp",
    "tests/test_cli_native.py",
    "tests/test_tac_folding.cpp",
)


def main():
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=here.parents[2] / "third_party/jiterati")
    parser.add_argument("--check", action="store_true", help="compare without changing files")
    args = parser.parse_args()
    tar_bytes = io.BytesIO()
    hashes = {}
    with tarfile.open(fileobj=tar_bytes, mode="w", format=tarfile.USTAR_FORMAT) as archive:
        for name in sorted(FILES):
            data = (args.source / name).read_bytes()
            hashes[name] = hashlib.sha256(data).hexdigest()
            member = tarfile.TarInfo(name)
            member.size = len(data)
            member.mode = 0o644
            archive.addfile(member, io.BytesIO(data))
    compressed = io.BytesIO()
    with gzip.GzipFile(filename="", fileobj=compressed, mode="wb", mtime=0) as output:
        output.write(tar_bytes.getvalue())
    payload = compressed.getvalue()
    manifest = {
        "format": "source-overlay-v1",
        "destination": "third_party/jiterati",
        "reference": "lapislazuli/.info/binutils (20111121)",
        "archive_sha256": hashlib.sha256(payload).hexdigest(),
        "files": hashes,
    }
    outputs = {
        "overlay.tar.gz": payload,
        "manifest.json": (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode(),
    }
    for name, data in outputs.items():
        path = here / name
        if args.check:
            if not path.exists() or path.read_bytes() != data:
                raise SystemExit(f"overlay differs: {path}")
        else:
            path.write_bytes(data)
    print(f"{'Verified' if args.check else 'Generated'} {len(FILES)} overlay files")


if __name__ == "__main__":
    main()
