#!/usr/bin/env python3
"""Basic YAML and structure checks; needs PyYAML (not required at runtime)."""
from pathlib import Path
import yaml

root = Path(__file__).resolve().parents[1]
source = root / 'bat' / 'LLSynth.sublime-syntax'
doc = yaml.safe_load(source.read_text(encoding='utf-8'))
assert doc['name'] == 'LLSynth'
assert 'llsynth' in doc['file_extensions']
assert doc['scope'] == 'source.llsynth'
for name in ('main', 'declaration', 'rule-facets', 'regex-declaration', 'comments', 'strings', 'atoms'):
    assert name in doc['contexts'], name
print(f'OK: {source} is valid YAML with expected LLSynth contexts')
