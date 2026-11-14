"""Independent vendor probes: neither is the ExoML type authority."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

kind, probe, exe, resources, cc = sys.argv[1:]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    header = root / 'macro.h'
    header.write_text('#ifdef FEATURE\nint enabled(int value);\n#else\nint disabled(void);\n#endif\n')
    if kind == 'ucpp':
        result = subprocess.run([probe, '-DFEATURE', str(header)], capture_output=True, text=True, check=True)
        assert 'enabled' in result.stdout and 'disabled' not in result.stdout
        result = subprocess.run([exe, 'inspect', '--header', str(header), '--define', 'FEATURE',
                                 '--resource-dir', resources, '--compiler', cc], capture_output=True, text=True, check=True)
        assert any(d['code'] == 'EXOTRACT-UCPP-PROBE' for d in json.loads(result.stdout)['diagnostics'])
        header.write_text('#if __STDC_VERSION__ < 201112L\n#error C11 required\n#endif\nint c11(void);\n')
        assert subprocess.run([probe, str(header)], capture_output=True).returncode != 0
        result = subprocess.run([exe, 'inspect', '--header', str(header), '--resource-dir', resources, '--compiler', cc],
                                capture_output=True, text=True, check=True)
        manifest = json.loads(result.stdout)
        assert manifest['syntax_checked']
        assert any(d['code'] == 'EXOTRACT-UCPP-PARTIAL' for d in manifest['diagnostics'])
    else:
        preprocessed = root / 'input.i'
        preprocessed.write_text(subprocess.check_output([cc, '-E', '-P', '-DFEATURE', str(header)], text=True))
        subprocess.run([probe, str(preprocessed), str(root / 'unused.c')], check=True)
        # This vendored parser rejects _Static_assert, although the compiler
        # accepts it. Its actionable parser error is written to stdout.
        header.write_text('_Static_assert(sizeof(int) >= 2, "C11");\nint valid(void);\n')
        result = subprocess.run([exe, 'inspect', '--header', str(header), '--resource-dir', resources, '--compiler', cc],
                                capture_output=True, text=True, check=True)
        manifest = json.loads(result.stdout)
        diagnostic = next(d for d in manifest['diagnostics'] if d['code'] == 'EXOTRACT-CPARSER-PARTIAL')
        assert 'error' in diagnostic['message'].lower(), diagnostic
        assert manifest['syntax_checked']
print(kind + ': real subprocess success and unsupported-C11 diagnostics passed')
