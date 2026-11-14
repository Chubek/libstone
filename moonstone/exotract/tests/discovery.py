"""Compiler-derived discovery, backend parity and fresh-evidence LMDB cache."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

exe, bind, fixture, resources, linked, lmdb, cc = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    header, provenance = root/'api.h', root/'manifest.txt'
    source = '#ifdef FEATURE\nlong enabled(long value);\n#else\nint disabled(void);\n#endif\n'
    header.write_text(source)
    provenance.write_text('revision one')
    base = ['--header', str(header), '--resource-dir', resources, '--exoml-bind', bind,
            '--compiler', cc, '--revision', 'test-revision', '--provenance', str(provenance)]

    def run(op='inspect', flags=(), success=True):
        result = subprocess.run([exe, op, *base, *flags], capture_output=True, text=True)
        assert (result.returncode == 0) == success, (result.stdout, result.stderr)
        return json.loads(result.stdout) if success else result

    direct = run('on1x')
    external = run('on1x', ['--parser', 'compiler'])
    assert direct['cdecl'] == external['cdecl']
    assert direct['unsafe_by_default'] and all(not s['safe'] for s in direct['imports'])
    assert {s['c_name'] for s in direct['imports']} == {'disabled'}
    assert run()['parser'] == ('exoml/libclang' if linked == '1' else 'exoml-bind')
    changed = run('on1x', ['--define', 'FEATURE'])
    assert {s['c_name'] for s in changed['imports']} == {'enabled'}
    assert changed['cdecl']['fingerprint'] != direct['cdecl']['fingerprint']
    narrow = run('on1x', ['--compile-arg', '-m32'])
    assert narrow['cdecl']['target']['pointer_bits'] == 32
    assert narrow['cdecl']['fingerprint'] != direct['cdecl']['fingerprint']

    sidecar = root/'api.exocontract.json'
    contract = {'schema_version': 2, 'library': 'test', 'headers': [str(header)],
                'state': 'annotated', 'functions': [{'c_name': 'disabled',
                'effects': {'effects': ['foreign_call'], 'unknown': True}}]}
    sidecar.write_text(json.dumps(contract))
    normalized = run('validate', ['--contract', str(sidecar)])
    assert normalized['state'] == 'annotated'
    assert normalized['functions'][0]['effects']['unknown']
    if linked == '1':
        # Installed linked validation must not require an exoml-bind executable.
        assert run('validate', ['--contract', str(sidecar), '--exoml-bind', '']) == normalized
    sidecar.write_text(json.dumps({**contract, 'functions': [{'c_name': 'missing'}]}))
    invalid_contract = root/'invalid-contract-output'
    run('validate', ['--contract', str(sidecar), '--out', str(invalid_contract)], success=False)
    assert not invalid_contract.exists()

    if lmdb == '1':
        cache = ['--cache-dir', str(root/'db')]
        assert not run(flags=cache)['cache_hit']
        assert run(flags=cache)['cache_hit']
        assert not run(flags=cache + ['--define', 'FEATURE'])['cache_hit']
        assert run(flags=cache + ['--define', 'FEATURE'])['cache_hit']
        provenance.write_text('revision two')
        assert not run(flags=cache)['cache_hit']
        assert run(flags=cache)['cache_hit']
        header.write_text(source + 'int added(void);\n')
        assert not run(flags=cache)['cache_hit']
        assert run(flags=cache)['cache_hit']
        header.write_text('this is invalid C;\n')
        failure = run(flags=cache, success=False)
        assert not json.loads(failure.stdout)['cache_hit']
        header.write_text(source)

    output = root/'output.json'
    output.write_text('previous successful artifact')
    run('on1x', ['--require-symbol', 'absent', '--out', str(output)], success=False)
    run('on1x', ['--parser', 'compiler', '--exoml-bind', str(root/'missing'),
                 '--out', str(output)], success=False)
    assert output.read_text() == 'previous successful artifact'
    # A genuine libclang failure after the independent C compiler succeeds.
    missing = run(flags=['--header', fixture, '--resource-dir', str(root/'missing-resources')], success=False)
    diagnostics = json.loads(missing.stdout)['diagnostics']
    assert any(d['code'] == 'EXOML-C11-COMPILER' and 'stddef.h' in d['message'] for d in diagnostics)
print('linked/CLI discovery parity, macro/ABI profiles, cache freshness and publication passed')
