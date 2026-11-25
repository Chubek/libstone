#!/usr/bin/env python3
"""C11 Cephyr optimizer benchmark: baseline versus configurable candidate.

Measures compiler invocation time for MIR output, emitted MIR metrics and
relocatable object sizes; optionally runs standalone self-checking programs.
Does NOT claim to measure generated-code execution speed.
"""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time

MIR_OPS = frozenset(('const','copy','add','sub','mul','and','or','xor','shl',
                     'lshr','ashr','div_u','div_s','rem_u','rem_s','eq',
                     'slt','ult','zext','sext','trunc','load','store','call'))


def check_command(command: list[str], timeout: int, environment: dict) -> tuple[bytes, float]:
    started = time.perf_counter_ns()
    try:
        proc = subprocess.run(command, capture_output=True, timeout=timeout, env=environment)
    except subprocess.TimeoutExpired as exc:
        raise RuntimeError(f'timed out after {timeout}s: {command!r}') from exc
    elapsed_ms = (time.perf_counter_ns()-started)/1e6
    if proc.returncode:
        raise RuntimeError(f'exit {proc.returncode}: {command!r}\n'
                           f'{proc.stderr.decode(errors="replace")[:10000]}\n'
                           f'{proc.stdout.decode(errors="replace")[:3000]}')
    return proc.stdout, elapsed_ms


def count_mir(mir: bytes) -> dict:
    if not mir.startswith(b'archon-mir 1\n'):
        raise RuntimeError('expected Archon native MIR v1')
    histogram = Counter()
    for line in mir.splitlines():
        words = line.split()
        if words and words[0].decode('ascii') in MIR_OPS:
            histogram[words[0].decode('ascii')] += 1
    return dict(bytes=len(mir), opcodes=dict(sorted(histogram.items())),
                counted_ops=sum(histogram.values()))


def measure_mir(compilers, path, base_dir, args, modes, env):
    times={mode: [] for mode in modes}
    last={}
    order=list(modes)
    for mode in modes:
        (base_dir/mode).mkdir()
    for i in range(args.warmup + args.repeat):
        stem = 'warmup' if i < args.warmup else 'sample'
        for mode in (order if i % 2 == 0 else reversed(order)):
            output=base_dir/mode/f'{stem}-{i}.mir'
            command=[compilers[mode], f'--target={args.target}', f'--config={args.config_path}',
                     '--no-cache', *([] if args.with_factir else ['--no-factir']),
                     *modes[mode], '--emit-mir', str(path), '-o', str(output)]
            _, ms = check_command(command, args.timeout, env)
            payload=output.read_bytes()
            if mode in last and payload != last[mode]:
                raise RuntimeError(f'non-deterministic MIR for {path.name} in {mode}')
            last[mode]=payload
            if i >= args.warmup:
                times[mode].append(ms)
            if not args.keep_samples and i != args.warmup + args.repeat - 1:
                output.unlink()
    report={}
    for mode, samples in times.items():
        (base_dir/mode/'final.mir').write_bytes(last[mode])
        report[mode]=dict(mir_ms_median=statistics.median(samples),
                          mir_ms_min=min(samples), mir_ms_max=max(samples),
                          mir=count_mir(last[mode]))
    return report, last


def text_section_size(objdump: str|None, obj: Path, timeout: int, env: dict):
    if not objdump:
        return None
    output,_=check_command([objdump, '--section-headers', str(obj)], timeout, env)
    # llvm-objdump normally prints: 0 .text 00000014 ...
    match=re.search(rb'^\s*\d+\s+\.text\s+([0-9A-Fa-f]+)\b', output, re.M)
    return int(match.group(1),16) if match else None


def native_target_matches(target: str) -> bool:
    machine=platform.machine().lower()
    return (target=='x86_64-linux-gnu' and machine in ('x86_64','amd64') or
            target=='aarch64-linux-gnu' and machine in ('aarch64','arm64') or
            target=='riscv64-linux-gnu' and machine in ('riscv64',))


def main():
    parser=argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--compiler', required=True, type=Path, help='Cephyr CLI executable')
    parser.add_argument('--baseline-compiler', type=Path,
                        help='use a separate Cephyr executable for the baseline')
    parser.add_argument('--output', required=True, type=Path, help='new results directory')
    parser.add_argument('--target', default='x86_64-linux-gnu',
                        choices=('x86_64-linux-gnu','aarch64-linux-gnu','riscv64-linux-gnu'))
    parser.add_argument('--fixture', default='.', help='regex matching fixture stems')
    parser.add_argument('--repeat', type=int, default=5)
    parser.add_argument('--warmup', type=int, default=1)
    parser.add_argument('--timeout', type=int, default=120)
    parser.add_argument('--candidate-arg', action='append', default=None,
                        help='CLI option for candidate; repeat, e.g. --candidate-arg=--qml-pass=canonicalize')
    parser.add_argument('--baseline-arg', action='append', default=[],
                        help='extra CLI option for baseline; repeat as needed')
    parser.add_argument('--with-factir', action='store_true',
                        help='enable default FactIR (off for both modes otherwise)')
    parser.add_argument('--no-blackstone', action='store_true',
                        help='disable Blackstone in both modes to isolate other passes')
    parser.add_argument('--runner', default=None,
                        help='emulator executable, e.g. qemu-riscv64-static')
    parser.add_argument('--runner-arg', action='append', default=[])
    parser.add_argument('--skip-run', action='store_true',
                        help='skip linking and self-check execution (cross compile only)')
    parser.add_argument('--objdump', default=shutil.which('llvm-objdump'),
                        help='optional llvm-objdump path for .text size')
    parser.add_argument('--keep-samples', action='store_true',
                        help='retain each timed MIR artifact')
    args=parser.parse_args()
    if args.repeat < 1 or args.warmup < 0 or args.timeout < 1:
        parser.error('repeat and timeout must be positive; warmup must be nonnegative')
    if not args.skip_run and not args.runner and not native_target_matches(args.target):
        parser.error('cross-target execution requires --runner or --skip-run')
    args.compiler=args.compiler.resolve()
    if not args.compiler.is_file():
        parser.error(f'Cephyr executable does not exist: {args.compiler}')
    baseline=(args.baseline_compiler or args.compiler).resolve()
    if not baseline.is_file():
        parser.error(f'baseline Cephyr executable does not exist: {baseline}')
    compilers={'baseline':str(baseline), 'candidate':str(args.compiler)}
    # The fixtures directory is shared with run.py, which also has kernels
    # requiring a separate driver. Only the manifest programs are standalone.
    root=Path(__file__).resolve().parent
    manifest=json.loads((root/'manifest.json').read_text(encoding='utf-8'))
    fixtures=sorted(root/'fixtures'/(entry['name']+'.c') for entry in manifest['fixtures'])
    fixtures=[f for f in fixtures if re.search(args.fixture,f.stem)]
    if not fixtures:
        parser.error('no fixtures match the supplied --fixture regex')
    args.output=args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=False)
    args.config_path=args.output/'Cephyr.toml'
    config='[paths]\ncache_path = ".cache"\n'
    if args.no_blackstone:
        config+='[blackstone]\nenabled = false\n'
    args.config_path.write_text(config,encoding='ascii')
    modes={'baseline':args.baseline_arg,
           'candidate':args.candidate_arg if args.candidate_arg is not None else ['--qml-pass=canonicalize']}
    env=os.environ.copy()
    env.pop('CEPHYR_CONFIG',None)
    result={'schema':'cephyr.blackstone.c11.benchmark.v1',
            'date_utc':datetime.now(timezone.utc).isoformat(),
             'compiler':str(args.compiler),'baseline_compiler':str(baseline),'target':args.target,
             'modes':modes,'factir_enabled':args.with_factir,'blackstone_enabled':not args.no_blackstone,
            'repeat':args.repeat,'warmup':args.warmup,
            'runner':args.runner,'host':platform.platform(),
            'fixtures':{}}
    print(f"{'fixture':24} {'MIR base ms':>12} {'MIR cand ms':>12} {'MIR delta B':>12} {'obj delta B':>12} {'check':>8}")
    for source in fixtures:
        base_dir=args.output/source.stem
        base_dir.mkdir()
        item,mirs=measure_mir(compilers, source, base_dir, args, modes, env)
        for mode,flags in modes.items():
            mode_dir=base_dir/mode
            obj=mode_dir/(source.stem+'.o')
            check_command([compilers[mode], f'--target={args.target}',
                           f'--config={args.config_path}',
                           *([] if args.with_factir else ['--no-factir']),
                           *flags, '-c', str(source), '-o', str(obj)],args.timeout,env)
            item[mode]['object_bytes']=obj.stat().st_size
            item[mode]['text_bytes']=text_section_size(args.objdump,obj,args.timeout,env)
            if not args.skip_run:
                exe=mode_dir/(source.stem+'.exe')
                check_command([compilers[mode], f'--target={args.target}',
                               f'--config={args.config_path}',
                               *([] if args.with_factir else ['--no-factir']),
                               *flags, str(source), '-o', str(exe)], args.timeout,env)
                runner=[args.runner,*args.runner_arg] if args.runner else []
                check_command([*runner,str(exe)],args.timeout,env)
                item[mode]['self_check']='pass'
            else:
                item[mode]['self_check']='not_run'
        item['mir_changed']=mirs['baseline']!=mirs['candidate']
        result['fixtures'][source.stem]=item
        print(f'{source.stem:24} {item["baseline"]["mir_ms_median"]:12.2f} '
              f'{item["candidate"]["mir_ms_median"]:12.2f} '
              f'{len(mirs["candidate"])-len(mirs["baseline"]):+12d} '
              f'{item["candidate"]["object_bytes"]-item["baseline"]["object_bytes"]:+12d} '
              f'{"skip" if args.skip_run else "pass":>8}')
        (args.output/'summary.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf8')
    print('\nReport:',args.output/'summary.json')
    print('MIR timings include process start and driver overhead; they do not measure generated-code speed.')

if __name__=='__main__':
    try:
        main()
    except (RuntimeError,OSError) as exc:
        print(f'ERROR: {exc}',file=sys.stderr)
        raise SystemExit(1)
