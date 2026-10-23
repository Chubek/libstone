"""Coverage and independent upstream-table checks for generated ISA inventories."""

from collections import Counter
import json
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import generate_binutils_isa as generator


class ArchitectureInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = generator.ROOT / 'binutils'
        cls.infobank = generator.ROOT / 'yellowstone/infobank'
        cls.inventory = generator.Inventory(cls.root)
        cls.manifest = json.loads((cls.infobank / 'manifest.json').read_text())

    def test_every_dispatched_architecture_has_a_description(self):
        source = (self.root / 'opcodes/disassemble.c').read_text()
        architectures = set(re.findall(r'case bfd_arch_(\w+):', source))
        coverage = self.manifest['binutils_coverage']
        self.assertEqual(set(coverage['architectures']), architectures)
        self.assertEqual(coverage['architecture_count'], len(architectures))
        entries = {entry['file']: entry for entry in self.manifest['architectures']}
        self.assertEqual(len(entries), len(self.manifest['architectures']))
        for arch, files in coverage['architectures'].items():
            self.assertTrue(files, arch)
            for file in files:
                self.assertIn(file, entries)
                text = (self.infobank / file).read_text()
                self.assertRegex(text, r'(?m)^arch ' + entries[file]['arch'] + r'\s*\{')
                self.assertGreater(entries[file]['instruction_count'], 0)
                names = re.findall(r'^op ([\w.-]+)\s*\{', text, re.M)
                self.assertEqual(len(names), entries[file]['instruction_count'], file)
                self.assertEqual(len(names), len(set(names)), file)

    def test_generated_files_are_current(self):
        outputs, entries = generator.outputs(self.root, self.infobank)
        self.assertEqual(len(entries), 63)
        for path, expected in outputs.items():
            self.assertEqual(path.read_text(), expected, str(path))

    def test_macro_expanded_names_against_c_compiler(self):
        # Include the original headers directly: C, rather than our parser,
        # determines macro expansion, conditional compilation, and array size.
        cases = [
            ('h8300', 'opcode/h8300.h', 'h8_opcodes'),
            ('h8500', '../opcodes/h8500-opc.h', 'h8500_table'),
            ('msp430', 'opcode/msp430.h', 'msp430_opcodes'),
            ('tic4x', 'opcode/tic4x.h', 'tic4x_insts'),
            ('vax', 'opcode/vax.h', 'votstrs'),
        ]
        with tempfile.TemporaryDirectory(prefix='isa-table-check-',
                dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None) as directory:
            path = Path(directory) / 'check.c'
            executable = Path(directory) / 'check'
            for arch, header, table in cases:
                with self.subTest(arch=arch):
                    source = f'''
#include <stdio.h>
#define DEFINE_TABLE 1
#define ASSEMBLER_TABLE 1
#define DASM_SECTION 1
#include "{header}"
int main(void) {{
  unsigned i;
  for (i = 0; i < sizeof({table}) / sizeof({table}[0]); ++i)
    if ({table}[i].name && {table}[i].name[0]) puts({table}[i].name);
  return 0;
}}
'''
                    path.write_text(source)
                    compiled = subprocess.run(['cc', '-std=gnu99', '-I' + str(self.root / 'include'),
                                               str(path), '-o', str(executable)],
                                              capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stderr)
                    output = subprocess.run([str(executable)], capture_output=True, text=True, check=True).stdout
                    expected = Counter(name for name in output.splitlines()
                                       if name.lower() not in ('bad', 'invalid', 'undefined', '???'))
                    actual = Counter(row['mnemonic'] for row in self.inventory.instructions(arch))
                    self.assertEqual(expected, actual)

    def test_source_specific_architecture_constraints(self):
        # BFD's addressable bytes differ from host octets on these DSPs.
        self.assertEqual(self.inventory.dimensions('tic4x')['bits_per_byte'], 32)
        self.assertEqual(self.inventory.dimensions('tic54x')['bits_per_byte'], 16)
        self.assertEqual(self.inventory.dimensions('h8500')['address_size'], 24)
        # RS/6000 uses the POWER dialect, not all modern PowerPC extensions.
        names = {row['mnemonic'] for row in self.inventory.instructions('rs6000')}
        self.assertIn('cal', names)
        self.assertNotIn('vaddubm', names)
        self.assertNotIn('ld', names)
        z80 = {row['mnemonic'] for row in self.inventory.instructions('z80')}
        self.assertTrue({'ldi', 'ldir', 'lddr', 'cpir', 'otir', 'otdr'} <= z80)


if __name__ == '__main__':
    unittest.main()
