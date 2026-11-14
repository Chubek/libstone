import os, subprocess, sys, tempfile
from pathlib import Path
exe = sys.argv[1]
with tempfile.TemporaryDirectory() as tmp:
    path=Path(tmp)/'db'
    subprocess.run([exe,'store',str(path),'cold','value'],check=True)
    assert subprocess.check_output([exe,'read',str(path),'cold'])==b'value'
    writers=[subprocess.Popen([exe,'store',str(path),f'key-{i}',f'value-{i}']) for i in range(16)]
    assert all(p.wait()==0 for p in writers)
    for i in range(16):
        assert subprocess.check_output([exe,'read',str(path),f'key-{i}'])==f'value-{i}'.encode()
    subprocess.run([exe,'duplicate',str(path)],check=True)
    invalid=Path(tmp)/'file';invalid.write_text('not a directory')
    assert subprocess.run([exe,'store',str(invalid),'a','b'],capture_output=True).returncode != 0
    corrupt=Path(tmp)/'corrupt';corrupt.mkdir();(corrupt/'data.mdb').write_bytes(b'x'*8192)
    assert subprocess.run([exe,'read',str(corrupt),'a'],capture_output=True).returncode != 0
    # Directory write permission controls creating/removing entries, not writing
    # existing data.mdb/lock.mdb files. Deny the actual open of those files.
    for name in ('data.mdb','lock.mdb'):
        (path/name).chmod(0)
    credentials = {}
    if os.geteuid() == 0:
        # Root bypasses DAC. Use an unprivileged child, with a traversable parent.
        Path(tmp).chmod(0o755)
        path.chmod(0o755)
        credentials = {'user': 65534, 'group': 65534, 'extra_groups': []}
    try:
        result=subprocess.run([exe,'store',str(path),'denied','x'],capture_output=True,**credentials)
        assert result.returncode != 0 and b'Permission denied' in result.stderr, result.stderr
    finally:
        for name in ('data.mdb','lock.mdb'):
            (path/name).chmod(0o600)
print('independent LMDB writers, persistence, corruption and permissions passed')
