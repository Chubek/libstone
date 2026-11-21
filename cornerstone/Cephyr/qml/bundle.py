#!/usr/bin/env python3
"""Assemble pure QML 1.1 pass fragments into one parser input (stdlib only).

Pass files deliberately are NOT executed as Python or Perl code.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parent
ORDER = (
    'Desugar.qpass', 'Sema.qpass', 'Canonicalize.qpass',
    'Optimize.qpass', 'Lower.qpass', 'AbsInterp.qpass',
    'SymExec.qpass', 'Format.qpass', 'PrettyPrint.qpass',
)
DECL = re.compile(r'^\s*(rewrite|reject)\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*:', re.M)
PASS = re.compile(r'^\s*pass\s+([A-Za-z_]\w*)\s*;', re.M)


def assemble(pass_dir=ROOT, orchestrator=None, term_file=None):
    root = Path(pass_dir)
    main = Path(orchestrator) if orchestrator else root / 'Orchestrator.qml'
    program = main.read_text(encoding='utf-8')
    if len(re.findall(r'^\s*run\s+', program, re.M)) != 1:
        raise ValueError(f'{main} must have exactly one run statement')
    chosen = PASS.findall(program)
    if not chosen:
        raise ValueError('Orchestrator must declare pass order explicitly')
    if term_file:
        extra = Path(term_file).read_text(encoding='utf-8')
        extra = re.sub(r'(?m)^\s*#[^\n]*\n?', '', extra).strip()
        if not extra.startswith('run ') or not extra.endswith(';'):
            raise ValueError('--program input must contain one run TERM; statement')
        # Separate statement scanning is safe because the surface DSL has no quoted strings.
        program = re.sub(r'(?s)\brun\s+.*?;\s*$', extra+'\n', program)
    all_decls = []
    names = set()
    for name in ORDER:
        file = root / name
        content = file.read_text(encoding='utf-8')
        if re.search(r'^\s*(?:run|pass)\s+', content, re.M):
            raise ValueError(f'{file}: .qpass fragments may only define rewrite/reject rules')
        declarations = DECL.findall(content)
        if not declarations:
            raise ValueError(f'{file}: no rewrite/reject declarations found')
        for kind, phase, label in declarations:
            ident = (kind, phase, label)
            if ident in names:
                raise ValueError(f'duplicate pass declaration: {ident}')
            names.add(ident)
            if phase not in chosen:
                raise ValueError(f'{file}: phase {phase!r} is absent from Orchestrator')
        all_decls.append(f'# === {name} ===\n{content.rstrip()}\n')
    return ('# Generated from Cephyr QML fragments; edit .qpass and Orchestrator.qml instead.\n'
            + '\n'.join(all_decls) + '\n' + program.rstrip() + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('-o', '--output', type=Path, help='output file (stdout if omitted)')
    ap.add_argument('--program', type=Path, help='alternate run TERM; expression file')
    ap.add_argument('--orchestrator', type=Path, help='alternate orchestration file')
    args = ap.parse_args()
    result = assemble(orchestrator=args.orchestrator, term_file=args.program)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(result, encoding='utf-8')
    else:
        print(result, end='')


if __name__ == '__main__':
    main()
