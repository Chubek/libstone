"""Real cross-process normalization-cache hits and adversarial invalidation."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

tool, fixture, resources, mulib = sys.argv[1:]


def run(header, cache, *args, expected=0):
    command = [tool, 'extract', '--header', str(header), '--resource-dir', resources,
               '--cache-dir', str(cache), '--cache-report', *args]
    p = subprocess.run(command, capture_output=True, text=True, timeout=30)
    assert p.returncode == expected, (p.returncode, p.stderr)
    unit = json.loads(p.stdout)
    report = json.loads(p.stderr.splitlines()[-1])['cache']
    assert unit['complete'] == (expected == 0)
    assert not unit['safe_export'] and unit['unknown_effects']
    return unit, report


with tempfile.TemporaryDirectory(prefix='exoml-cache-') as temp:
    root = Path(temp)
    cache = root / 'cache'
    cold, report = run(fixture, cache)
    assert report['status'] == 'miss' and report['stored']
    first_key = report['key']
    warm, report = run(fixture, cache)
    assert report['status'] == 'hit' and not report['stored'] and warm == cold
    assert report['key'] == first_key and len(first_key) == 64
    uncached = subprocess.run([tool, 'extract', '--header', fixture, '--resource-dir', resources,
                               '--cache-report'], capture_output=True, text=True, check=True)
    assert json.loads(uncached.stdout) == cold
    assert json.loads(uncached.stderr)['cache']['status'] == 'disabled'
    assert subprocess.run([tool, 'extract', '--header', fixture, '--max-types', '-1'],
                          capture_output=True).returncode == 2
    assert not list(cache.glob('.pending-*'))
    artifacts = list(cache.glob('*.cdecl-cache'))
    assert len(artifacts) == 1
    original = artifacts[0].read_bytes()
    for damage in (b'bad', original[:-1], original.replace(b'EXOML-CACHE-2', b'EXOML-CACHE-9', 1)):
        artifacts[0].write_bytes(damage)
        restored, report = run(fixture, cache)
        assert report['status'] == 'rebuilt' and report['stored'] and report['reason']
        assert restored == cold
    assert artifacts[0].read_bytes() == original
    bounded, bounded_report = run(fixture, cache, '--max-cache-bytes', '1048576')
    bounded_path = cache / (bounded_report['key'] + '.cdecl-cache')
    with bounded_path.open('r+b') as f:
        f.truncate(2097152)
    repaired, repaired_report = run(fixture, cache, '--max-cache-bytes', '1048576')
    assert repaired == bounded and repaired_report['status'] == 'rebuilt'
    assert bounded_path.stat().st_size < 1048576
    # Corrupt payload with a valid envelope checksum: the typed decoder must still reject it.
    prefix, key, checksum, payload = original.split(b'\n', 3)
    bad_payload = b'EXOML-UNIT-2\n' + b'\xff' * 40
    artifacts[0].write_bytes(prefix + b'\n' + key + b'\n' + hashlib.sha256(bad_payload).hexdigest().encode()
                             + b'\n' + bad_payload)
    restored, report = run(fixture, cache)
    assert report['status'] == 'rebuilt' and restored == cold
    sentinel = root / 'sentinel'
    sentinel.write_text('must not read or overwrite this')
    artifacts[0].unlink()
    artifacts[0].symlink_to(sentinel)
    restored, report = run(fixture, cache)
    assert report['status'] == 'rebuilt' and restored == cold
    assert sentinel.read_text() == 'must not read or overwrite this'
    assert not artifacts[0].is_symlink()
    insecure = root / 'insecure'
    insecure.mkdir(mode=0o755)
    insecure.chmod(0o755)
    failed, report = run(fixture, insecure, expected=3)
    assert report['status'] == 'error' and 'private' in report['reason']
    link = root / 'link'
    link.symlink_to(cache, target_is_directory=True)
    failed, report = run(fixture, link, expected=3)
    assert report['status'] == 'error'
    before = set(cache.glob('*.cdecl-cache'))
    failed, report = run(fixture, cache, '--define', 'EXOML_INVALID', expected=3)
    assert report['status'] == 'bypassed' and not report['stored']
    assert set(cache.glob('*.cdecl-cache')) == before
    _, limit_report = run(fixture, cache, '--max-types', '0', expected=3)
    assert limit_report['status'] == 'bypassed' and not limit_report['stored']
    _, limit_report = run(fixture, cache, '--max-cache-bytes', '1', expected=3)
    assert limit_report['status'] == 'error' and not limit_report['stored']
    _, changed_limits = run(fixture, cache, '--max-depth', '127')
    assert changed_limits['status'] == 'miss' and changed_limits['key'] != first_key
    for args in (('--define', 'EXOML_ALTERNATE'), ('--revision', 'another-revision'),
                 ('--define', 'EXOML_ALTERNATE', '--undefine', 'EXOML_ALTERNATE')):
        changed, report = run(fixture, cache, *args)
        assert report['status'] == 'miss' and report['key'] != first_key
        _, repeated = run(fixture, cache, *args)
        assert repeated['status'] == 'hit'
    header = root / 'input.h'
    header.write_text('typedef long word;\nword function(word value);\n')
    earlier, r1 = run(header, cache, '--target', 'i386-unknown-linux-gnu')
    later, r2 = run(header, cache, '--target', 'x86_64-unknown-linux-gnu')
    assert earlier['target']['pointer_bits'] == 32 and later['target']['pointer_bits'] == 64
    assert r1['key'] != r2['key'] and r1['status'] == r2['status'] == 'miss'
    provenance = root / 'contract.json'
    provenance.write_text('{"candidate":1}\n')
    _, r1 = run(header, cache, '--provenance', str(provenance))
    timestamp = provenance.stat()
    provenance.write_text('{"candidate":2}\n')
    os.utime(provenance, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
    _, r2 = run(header, cache, '--provenance', str(provenance))
    assert r1['key'] != r2['key'] and r2['status'] == 'miss'
    # A new earlier include is not in the previous dependency closure.
    first, second = root / 'first', root / 'second'
    first.mkdir(); second.mkdir()
    dep = second / 'dependency.h'
    dep.write_text('typedef int scalar;\n')
    header.write_text('#include <dependency.h>\nscalar function(void);\n')
    include_args = ('--include', str(first), '--include', str(second))
    _, r1 = run(header, cache, *include_args)
    (first / 'dependency.h').write_text('typedef double scalar;\n')
    changed, r2 = run(header, cache, *include_args)
    assert r1['key'] != r2['key'] and r2['status'] == 'miss'
    assert any(t['spelling'] == 'double' for t in changed['types'])
    (first / 'dependency.h').unlink()
    dep.unlink()
    _, report = run(header, cache, *include_args, expected=3)
    assert report['status'] == 'bypassed'
    # A negative __has_include probe need not ever become an actual include.
    header.write_text('#if __has_include("optional.h")\nint present(void);\n#else\nint absent(void);\n#endif\n')
    earlier, r1 = run(header, cache)
    (root / 'optional.h').write_text('')
    later, r2 = run(header, cache)
    assert r1['key'] != r2['key'] and r2['status'] == 'miss'
    assert any(d['name'] == 'absent' for d in earlier['declarations'])
    assert any(d['name'] == 'present' for d in later['declarations'])
    header.write_text('struct conditional {\n#if __has_include("layout.h")\nlong value;\n#else\nchar value;\n#endif\n};\n')
    earlier, r1 = run(header, cache)
    (root / 'layout.h').write_text('')
    later, r2 = run(header, cache)
    assert r1['key'] != r2['key'] and r2['status'] == 'miss'
    before_size = next(t['size'] for t in earlier['types'] if t['kind'] == 'record')
    after_size = next(t['size'] for t in later['types'] if t['kind'] == 'record')
    assert before_size != after_size
    header.write_text('int version_one(void);\n')
    _, r1 = run(header, cache)
    timestamp = header.stat()
    header.write_text('int version_two(void);\n')
    os.utime(header, ns=(timestamp.st_atime_ns, timestamp.st_mtime_ns))
    changed, r2 = run(header, cache)
    assert r1['key'] != r2['key'] and r2['status'] == 'miss'
    assert changed['declarations'][0]['name'] == 'version_two'
    # Multiple independent writers publish one complete deterministic artifact.
    concurrent_cache = root / 'concurrent'
    concurrent_cache.mkdir(mode=0o700)
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        results = list(pool.map(lambda _: run(fixture, concurrent_cache), range(12)))
    assert all(unit == cold and report['status'] in ('hit', 'miss') for unit, report in results)
    assert len(list(concurrent_cache.glob('*.cdecl-cache'))) == 1
    assert not list(concurrent_cache.glob('.pending-*'))
    if mulib:
        root_mulib = Path(mulib)
        revision = subprocess.check_output(['git', '-C', str(root_mulib), 'rev-parse', 'HEAD'], text=True).strip()
        assert revision == 'f6f2eea234f1baa3fe73b8eb49194e7362b76278'
        for name in ('vec', 'json', 'parsec'):
            path = root_mulib / 'include' / f'mulib-{name}.h'
            a, ra = run(path, cache, '--revision', revision)
            b, rb = run(path, cache, '--revision', revision)
            assert ra['status'] == 'miss' and rb['status'] == 'hit' and a == b
            print(f'{name}: real compiler-validated cache miss -> hit')
