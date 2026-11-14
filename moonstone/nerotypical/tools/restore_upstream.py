#!/usr/bin/env python3
"""Fill missing upstream Nerotypical files from an existing LibStone checkout,
else from a PINNED GitHub snapshot. Never replaces new/modified files.

Usage:
    python tools/restore_upstream.py --checkout /path/to/libstone
    python tools/restore_upstream.py --fetch

Git SHA-1 of each source blob is checked before writing; no blind master pulls.
"""
from __future__ import annotations
import argparse
import hashlib
from pathlib import Path
from urllib.request import urlopen

UPSTREAM_COMMIT = '321bcc989ece9e08cc2ba318c21aa14d02d98a3c'
UPSTREAM = 'https://raw.githubusercontent.com/Chubek/libstone/' + UPSTREAM_COMMIT + '/moonstone/nerotypical/'
BLOBS = {
    'include/nerotypical/Analysis.hpp': '47e2793565632812d5de10f61a895dbc6a466561',
    'include/nerotypical/Language.hpp': '45766c8913e8b6ac22175aa840aa25c41e35a021',
    'include/nerotypical/SetTyping.hpp': '136fd78e2fd74b9b85dcb0c01b1201003c6178cb',
    'include/nerotypical/StructuralTyping.hpp': '7a0b34b799674b5426f63daa078e48a2d128c7e0',
    'tests/set_typing.cpp': '1c6425e548558a533e12905b3651be61d8fc8316',
}

def git_blob_sha(content: bytes) -> str:
    return hashlib.sha1(b'blob ' + str(len(content)).encode() + b'\0' + content).hexdigest()

def main() -> None:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--checkout', type=Path, help='local LibStone checkout; nothing fetched')
    p.add_argument('--fetch', action='store_true', help='download missing pinned files from GitHub')
    p.add_argument('--destination', type=Path, default=Path(__file__).resolve().parents[1])
    args = p.parse_args()
    if not args.checkout and not args.fetch:
        p.error('select --checkout PATH or --fetch')
    source = args.checkout / 'moonstone' / 'nerotypical' if args.checkout else None
    if source and not source.is_dir():
        p.error(f'missing source directory {source}')
    for relative, expected in BLOBS.items():
        dest=args.destination / relative
        if dest.exists():
            if git_blob_sha(dest.read_bytes()) != expected:
                raise SystemExit(f'Existing {dest} has unexpected content; will not overwrite')
            print(f'verified {relative}')
            continue
        if source:
            content = (source / relative).read_bytes()
        else:
            url=UPSTREAM+relative
            with urlopen(url, timeout=40) as handle:
                content=handle.read()
        got=git_blob_sha(content)
        if got != expected:
            raise SystemExit(f'Hash mismatch for {relative}: {got} != {expected}')
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(content)
        print(f'restored {relative}: {len(content)} bytes')
    print('Original Nerotypical files restored; new Sequent IR files remain untouched.')

if __name__=='__main__':
    main()
