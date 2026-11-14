"""Pinned canonical MicroJS discovery, Lua contract and generated native TU."""
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

exe, bind, resources, mulib, revision, cc, cflags, lua = sys.argv[1:]
mulib = Path(mulib).resolve()
project = Path(__file__).resolve().parents[1]
assert subprocess.check_output(['git', '-C', str(mulib), 'rev-parse', 'HEAD'], text=True).strip() == revision
for name in ('include/mulib-microjs.h', 'include/mulib-minivm.h', 'MANIFEST.yaml'):
    data = (mulib/name).read_bytes()
    assert data == subprocess.check_output(['git', '-C', str(mulib), 'show', f'{revision}:{name}'])
    print(name, hashlib.sha256(data).hexdigest())

with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    common = ['--header', str(mulib/'include/mulib-microjs.h'), '--include', str(mulib/'include'),
              '--resource-dir', resources, '--exoml-bind', bind, '--revision', revision,
              '--provenance', str(mulib/'MANIFEST.yaml'), '--compiler', cc]
    metadata = json.loads(subprocess.check_output([exe, 'on1x', *common], text=True))
    public = (mulib/'include/mulib-microjs.h').read_text().split('#if defined(MULIB_IMPL_MICROJS)')[0]
    # Independent inventory from this header's public declaration section.
    names = set(re.findall(r'\b(mulib_microjs_\w+)\s*\((?!\s*\*)', re.sub(r'/\*.*?\*/', '', public, flags=re.S)))
    imported = {s['c_name'] for s in metadata['imports']}
    assert names == imported, (names - imported, imported - names)
    assert metadata['unsafe_by_default'] and all(not s['safe'] for s in metadata['imports'])
    assert all(s['effects'] == 'unknown' for s in metadata['imports'])
    assert any(d['name'] == 'mulib_microjs_native_fn' for d in metadata['cdecl']['declarations'])
    assert metadata['cdecl']['revision'] == revision
    print(f'MicroJS: {len(names)} discovered raw functions; zero reviewed safe exports')

    impl = root/'microjs.c'
    subprocess.run([exe, 'emit-source', *common, '--impl-define', 'MULIB_IMPL_MICROJS',
                    '--impl-define', 'MULIB_IMPL_MINIVM', '--dependency', 'mulib-minivm.h',
                    '--out', str(impl)], check=True)
    for macro in ('MULIB_IMPL_MICROJS', 'MULIB_IMPL_MINIVM'):
        assert impl.read_text().count('#define ' + macro + '\n') == 1
    flags = [cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-UNDEBUG', *shlex.split(cflags),
             '-I', str(mulib/'include')]
    obj = root/'microjs.o'
    subprocess.run([*flags, '-c', str(impl), '-o', str(obj)], check=True)
    consumer = root/'consumer'
    subprocess.run([*flags, str(project/'tests/microjs.c'), str(obj), '-lm', '-o', str(consumer)], check=True)
    subprocess.run([str(consumer)], check=True)
    duplicate = subprocess.run([*flags, str(project/'tests/microjs.c'), str(obj), str(obj),
                                '-lm', '-o', str(root/'duplicate')], capture_output=True, text=True)
    assert duplicate.returncode != 0 and 'mulib_microjs_create' in duplicate.stderr

    if lua == '1':
        variables = {'MULIB_KERNEL_DIR': str(mulib/'include'), 'MULIB_DOC_DIR': str(mulib/'manual'),
                     'EXOTRACT_ROOT': str(project), 'EXOML_BIND': bind, 'CLANG_RESOURCE_DIR': resources,
                     'MULIB_REVISION': revision, 'MULIB_MANIFEST': str(mulib/'MANIFEST.yaml'), 'C_COMPILER': cc}
        command = [exe, 'lua', str(project/'contracts/mulib/microjs.lua'), '--cache-dir', str(root/'db')]
        for key, value in variables.items():
            command += ['--global', f'{key}={value}']
        for _ in range(2):
            manifest = json.loads(subprocess.check_output(command, text=True))
            assert manifest['complete'] and not manifest['safe_export']
            assert manifest['revision'] == revision
    else:
        print('Lua MicroJS contract not built (requires Lua and LMDB)')
print('MicroJS single-TU native ownership, roots, callback and reentrancy checks passed')
