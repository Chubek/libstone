"""Independent JSON/provenance/target/CLI checks; optional real MuLIB inventory."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

tool, fixture, mulib, resources, compiler = sys.argv[1:]
PIN = 'f6f2eea234f1baa3fe73b8eb49194e7362b76278'


def extract(header, *args, expected=0):
    p = subprocess.run([tool, 'extract', '--header', str(header), '--resource-dir', resources, *args],
                       capture_output=True, text=True, timeout=30)
    assert p.returncode == expected, (p.returncode, p.stderr)
    data = json.loads(p.stdout)
    assert data['complete'] == (expected == 0)
    assert data['state'] == 'discovered' and not data['safe_export']
    assert data['unknown_effects'] and data['schema_version'] == 2
    for entry in data['inputs']:
        assert hashlib.sha256(Path(entry['path']).read_bytes()).hexdigest() == entry['sha256']
    return data


u = extract(fixture)
assert u == extract(fixture)
assert extract(fixture, '--define', 'MULIB_IMPL_VEC', expected=3)['diagnostics']
assert extract(fixture, '--define', 'EXOML_INVALID', expected=3)['diagnostics']
assert subprocess.run([tool, 'generate'], capture_output=True).returncode == 2
assert subprocess.run([tool, 'extract', '--header', fixture, '--plugin', 'bad'],
                      capture_output=True).returncode == 2
if Path('/dev/full').exists():
    failed_write = subprocess.run([tool, 'extract', '--header', fixture, '--resource-dir', resources,
                                   '--out', '/dev/full'], capture_output=True)
    assert failed_write.returncode == 7
with tempfile.TemporaryDirectory(prefix='exoml-discovery-') as temp:
    root = Path(temp)
    output = root / 'declarations.json'
    written = subprocess.run([tool, 'extract', '--header', fixture, '--resource-dir', resources,
                              '--out', str(output)], capture_output=True, text=True, check=True)
    assert not written.stdout and json.loads(output.read_text()) == u
    provenance = root / 'provenance.json'
    provenance.write_text('{"evidence":"first"}\n')
    earlier = extract(fixture, '--provenance', str(provenance))
    provenance.write_text('{"evidence":"second"}\n')
    later = extract(fixture, '--provenance', str(provenance))
    assert earlier['fingerprint'] != later['fingerprint']
    undefined = extract(fixture, '--define', 'EXOML_ALTERNATE', '--undefine', 'EXOML_ALTERNATE')
    assert not any(d['name'] == 'exoml_conditional' for d in undefined['declarations'])
    probe = root / 'probe'
    subprocess.run([compiler, '-std=c11', '-Wall', '-Wextra', '-Wpedantic',
                    str(Path(fixture).with_name('discovery_probe.c')), '-o', str(probe)], check=True)
    measured = json.loads(subprocess.check_output([str(probe)], text=True))
    types = {t['id']: t for t in u['types']}
    declarations = {d['name']: d for d in u['declarations']}

    def underlying(name):
        t = types[declarations[name]['type']]
        while t['kind'] in ('typedef', 'elaborated'):
            t = types[t['element']]
        return t

    for name in ('size_t', 'intptr_t'):
        matches = [t for t in u['types'] if t['spelling'] == name]
        assert matches and all(t['size'] == measured[name] for t in matches)
    for spelling, key in (('long', 'long'), ('_Bool', 'bool')):
        assert any(t['spelling'] == spelling and t['size'] == measured[key] for t in u['types'])
    node = underlying('exoml_node')
    assert node['size'] == measured['node_size'] and node['alignment'] == measured['node_align']
    assert node['fields'][2]['offset_bits'] == measured['values_offset_bits']
    assert underlying('exoml_packed')['size'] == measured['packed_size']
    aligned = underlying('exoml_aligned')
    assert aligned['size'] == measured['aligned_size'] and aligned['alignment'] == measured['aligned_align']
    dep, header = root / 'dep.h', root / 'input.h'
    dep.write_text('typedef long dependency;\n')
    header.write_text('#include "dep.h"\ndependency function(dependency value);\n')
    first = extract(header)
    dep.write_text('typedef unsigned long dependency;\n')
    second = extract(header)
    assert first['fingerprint'] != second['fingerprint']
    target32 = extract(header, '--target', 'i386-unknown-linux-gnu')
    target64 = extract(header, '--target', 'x86_64-unknown-linux-gnu')
    assert target32['target']['pointer_bits'] == 32
    assert target64['target']['pointer_bits'] == 64
    assert target32['fingerprint'] != target64['fingerprint']
    for unit, width in ((target32, 4), (target64, 8)):
        integer = [t for t in unit['types'] if t['spelling'] == 'unsigned long']
        assert len(integer) == 1 and integer[0]['size'] == width
    # Paths are never interpolated into C source or a shell command.
    unusual = root / 'quote"header.h'
    unusual.write_text('int ordinary(void);\n')
    assert extract(unusual)['declarations'][0]['name'] == 'ordinary'
    dep.unlink()
    assert extract(header, expected=3)['diagnostics']
    # Include search order changes must not reuse stale discovery artifacts.
    dep.write_text('typedef int dependency;\n')
    other = root / 'other'
    other.mkdir()
    (other / 'dep.h').write_text('typedef double dependency;\n')
    header.write_text('#include <dep.h>\ndependency function(void);\n')
    ordered1 = extract(header, '--include', str(root), '--include', str(other))
    ordered2 = extract(header, '--include', str(other), '--include', str(root))
    assert ordered1['fingerprint'] != ordered2['fingerprint']

if mulib:
    root = Path(mulib).resolve()
    revision = subprocess.check_output(['git', '-C', str(root), 'rev-parse', 'HEAD'], text=True).strip()
    assert revision == PIN, f'MuLIB revision drift: expected {PIN}, got {revision}'
    # Do not silently label locally changed headers/manifest with the pinned SHA.
    subprocess.run(['git', '-C', str(root), 'diff', '--exit-code', 'HEAD', '--',
                    'include', 'MANIFEST.yaml', 'mudsl/scripts/generate_mulib.py',
                    'mudsl/mull/bridge/Mulib/Inventory.json'], check=True)
    inventory = json.loads((root / 'mudsl/mull/bridge/Mulib/Inventory.json').read_text())
    source_inventory = json.loads(Path(__file__).with_name('mulib-discovery-inventory.json').read_text())
    assert source_inventory['revision'] == revision
    for name in ('vec', 'json', 'parsec'):
        header = root / 'include' / f'mulib-{name}.h'
        unit = extract(header, '--revision', revision, '--provenance', str(root / 'MANIFEST.yaml'))
        functions = {d['name'] for d in unit['declarations'] if d['kind'] == 'function'}
        # Independently inspect declarations before the implementation guard.
        source = header.read_text().split('#if defined(MULIB_IMPL_', 1)[0]
        source = re.sub(r'/\*.*?\*/', '', source, flags=re.S)
        source_functions = set(re.findall(r'\b(mulib_\w+)\s*\([^;{}]*\)\s*;', source, re.S))
        assert functions == source_functions, (name, functions ^ source_functions)
        inventory_functions = set(inventory['modules'][name]['functions'])
        assert functions == inventory_functions, (name, functions ^ inventory_functions)
        expected = source_inventory['headers'][name]
        assert hashlib.sha256(header.read_bytes()).hexdigest() == expected['sha256']
        for kind in ('typedef', 'record', 'enum'):
            actual = sorted(d['name'] for d in unit['declarations'] if d['kind'] == kind)
            assert actual == sorted(expected[kind]), (name, kind, actual)
        assert not any(d['kind'] == 'unsupported' for d in unit['declarations'])
        pp = unit['preprocessing']
        assert pp['final_snapshot_complete']
        assert any(d['source']['file'] == str(header) for d in pp['definitions'])
        for d in unit['declarations']:
            if d['kind'] not in ('function', 'typedef', 'record', 'enum', 'macro', 'variable'):
                assert d['unsupported_reason']
        print(f'{name}: {len(functions)} discovered functions; 0 safe exports; header SHA256 '
              f'{hashlib.sha256(header.read_bytes()).hexdigest()}')
    print('MuLIB manifest SHA256', hashlib.sha256((root / 'MANIFEST.yaml').read_bytes()).hexdigest())
