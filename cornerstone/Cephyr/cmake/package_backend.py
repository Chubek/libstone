"""Generate one reviewed Archon native backend at package build time."""
import hashlib
import sys
from pathlib import Path

root, spec, architecture, abi, dest, *sources = sys.argv[1:]
sys.path.insert(0, root)
from archon.compiler import compile_file, parse
from archon.database import dumps
from archon.irgen import generate_ir

root = Path(root).resolve()
entry = root / 'stdspec' / spec
db = compile_file(entry)
sources_sha = {}

def record(path):
    path = path.resolve()
    name = path.relative_to(root).as_posix()
    if name in sources_sha:
        return
    data = path.read_bytes()
    sources_sha[name] = hashlib.sha256(data).hexdigest()
    for decl in parse(data.decode('utf-8'), str(path))['declarations']:
        if decl['kind'] == 'import':
            record(path.parent / Path(*decl['path'].split('.')).with_suffix('.arch'))

record(entry)
files = generate_ir('gated', [db], options=dict(profile='native', target=architecture,
                                                abi=abi, oracle=False, translate_tool=False))
out = Path(dest)
out.mkdir(parents=True, exist_ok=True)
for name in sources:
    (out / name).write_text(files[name], encoding='utf-8')
for name, text in files.items():
    if name.endswith('.h'):
        (out / name).write_text(text, encoding='utf-8')
(out / 'provider.archdb').write_text(dumps(db), encoding='utf-8')

(out / 'sources.sha256').write_text(''.join(f'{digest} {name}\n' for name, digest in sorted(sources_sha.items())))
