#!/usr/bin/env python3
"""Exercise external CDecl v2 JSON, explicit unsafe binding and required exports."""
import json, subprocess, sys, tempfile
from pathlib import Path
exe, header, resources, cc = sys.argv[1:]
with tempfile.TemporaryDirectory() as d:
    doc=Path(d)/'cdecl.json'
    doc.write_text((Path(header).parent/'transport.json').read_text())
    base=[exe,'on1x','--name','Fixture','--header',header,
          '--cdecl-json',str(doc),'--require-symbol','et_create','--resource-dir',resources,'--compiler',cc]
    out=subprocess.run(base,capture_output=True,text=True,check=True)
    data=json.loads(out.stdout)
    assert data['unsafe_by_default'] is True
    assert data['requires_review'] is True
    assert len(data['imports'])==2
    missing=subprocess.run(base+['--require-symbol','missing_symbol'],capture_output=True,text=True)
    assert missing.returncode!=0
    illegal=subprocess.run([exe,'inspect','--name','Fixture','--header',header,
        '--define','MULIB_IMPL_MICROJS'],capture_output=True,text=True)
    assert illegal.returncode!=0
print('exotract imported CDecl CLI: passed')
