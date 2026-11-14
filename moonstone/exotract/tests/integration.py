#!/usr/bin/env python3
"""Linkage and inspection regression; invoked from CTest."""
import json, subprocess, sys, tempfile
from pathlib import Path
exe, header, resources, cc = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    d=Path(tmp)
    src=d/'fixture.c'
    out=d/'fixture.o'
    cmd=[exe,'emit-source','--header',header,'--name','Fixture','--impl-define','EXOTRACT_IMPL_FIXTURE','--out',str(src)]
    subprocess.run(cmd,check=True)
    subprocess.run([cc,'-std=c11','-Wall','-Wextra','-Werror','-c',str(src),'-o',str(out)],check=True)
    assert out.exists()
    p=subprocess.run([exe,'inspect','--header',header,'--name','Fixture','--resource-dir',resources,'--compiler',cc],capture_output=True,text=True,check=True)
    manifest=json.loads(p.stdout)
    assert manifest['syntax_checked'] is True
    assert manifest['safe_export'] is False
    assert manifest['header_only']['implementation_translation_units']==1
    # No fabricated ABI: standalone configurations may have no CDecl extractor.
    if not manifest['complete']:
        assert manifest['symbols']==[]
    # A second implementation TU intentionally causes a link error.
    other=d/'second.o'
    subprocess.run([cc,'-std=c11','-c',str(src),'-o',str(other)],check=True)
    main=d/'main.c'
    main.write_text('int main(void){return 0;}\n')
    dupl=subprocess.run([cc,str(main),str(out),str(other),'-o',str(d/'bad')],capture_output=True,text=True)
    assert dupl.returncode!=0
print('exotract integration: passed')
