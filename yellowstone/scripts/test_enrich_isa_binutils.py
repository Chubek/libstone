"""Cross-check imported constants against independently compiled upstream tables."""

from pathlib import Path
import re
import subprocess
import tempfile
import unittest

import enrich_isa_binutils as importer


class BinutilsImportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = importer.ROOT / 'binutils'
        cls.tables = importer.load_tables(cls.root, 'cc')
        cls.scratch = tempfile.TemporaryDirectory(
            prefix='isa-binutils-', dir='/tmp/opencode' if Path('/tmp/opencode').is_dir() else None)
        cls.directory = Path(cls.scratch.name)
        (cls.directory / 'bfd_stdint.h').write_text('#include <stdint.h>\n')

    @classmethod
    def tearDownClass(cls):
        cls.scratch.cleanup()

    def compile_and_run(self, name, source, stdin=None):
        path = self.directory / (name + '.c')
        executable = self.directory / name
        path.write_text(source)
        result = subprocess.run(['cc', '-std=gnu99', '-I' + str(self.directory),
                                 '-I' + str(self.root / 'include'), str(path),
                                 '-o', str(executable)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return subprocess.run([str(executable)], input=stdin, text=True,
                              capture_output=True, check=True).stdout

    def test_compiled_opcode_constants(self):
        # Compile the original upstream C, independently of the importer's
        # expression evaluator, table splitter, and macro-expansion handling.
        for family, table, match, mask, skip in [
            ('mips', 'mips_builtin_opcodes', 'match', 'mask', 'if (p->pinfo == INSN_MACRO) continue;'),
            ('ppc', 'powerpc_opcodes', 'opcode', 'mask', ''),
            ('m68k', 'm68k_opcodes', 'opcode', 'match', ''),
            ('sparc', 'sparc_opcodes', 'match', 'match | p->lose', ''),
        ]:
            with self.subTest(family=family):
                source = (self.root / f'opcodes/{family}-opc.c').read_text()
                source = re.sub(r'^#include "(?:sysdep|opintl)\.h"', '', source, flags=re.M)
                prelude = ('#include <stdio.h>\n#include <stdint.h>\n#include <string.h>\n'
                           '#include <stdlib.h>\n#define _(s) (s)\n#define ATTRIBUTE_UNUSED\n')
                main = f'''
int main(void) {{
  unsigned i;
  for (i = 0; i < sizeof({table}) / sizeof({table}[0]); ++i) {{
    const __typeof__({table}[0]) *p = &{table}[i];
    {skip}
    printf("%s\\t%lx\\t%lx\\n", p->name,
           (unsigned long)(p->{match}) & 0xffffffffUL,
           (unsigned long)(p->{mask}) & 0xffffffffUL);
  }}
  return 0;
}}
'''
                output = self.compile_and_run(family, prelude + source + main)
                expected = [(name, int(value, 16), int(bits, 16))
                            for name, value, bits in (line.split('\t') for line in output.splitlines())]
                actual = [(row['mnemonic'], row['match'], row['mask'])
                          for row in self.tables[family]
                          if 'match' in row and 'alias' not in row]
                self.assertEqual(expected, actual)

    def test_s390_upstream_mnemonic_expansion(self):
        source = (self.root / 'opcodes/s390-mkopc.c').read_text()
        output = self.compile_and_run('s390-mkopc', source,
                                     (self.root / 'opcodes/s390-opc.txt').read_text())
        expected = []
        for name, width, opcode, fmt in re.findall(
                r'\{ "([^"]+)", OP(\d+)\(0x([0-9a-fA-F]+)LL\), MASK_(\w+)', output):
            # mkopc stores a left-aligned opcode of width OP<n>.
            expected.append((name, fmt, int(opcode, 16) << (48 - int(width))))
        actual = [(row['mnemonic'], row['format'], row['match'] << (48 - row['encoding_bits']))
                  for row in self.tables['s390']]
        self.assertGreater(len(expected), 1000)
        self.assertCountEqual(expected, actual)

    def test_provenance_lines_and_macro_safety(self):
        sources = {}
        for family, rows in self.tables.items():
            for row in rows:
                path = importer.ROOT / row['source']
                if path not in sources:
                    sources[path] = path.read_text().split('\n')
                lines = sources[path]
                self.assertTrue(0 < row['line'] <= len(lines), row)
                self.assertTrue(lines[row['line'] - 1].strip(), row)
                if row.get('kind') == 'assembler_macro':
                    self.assertNotIn('mask', row)
                    self.assertNotIn('match', row)
                    self.assertTrue(row['macro_id'].startswith('M_'))
                if family == 'avr':
                    self.assertEqual(len(row['bit_pattern']), 16)
                    self.assertIn(row['encoding_bits'], (16, 32))
                if family == 's390':
                    self.assertLess(row['match'], 1 << row['encoding_bits'])
                    self.assertLess(row['mask'], 1 << row['encoding_bits'])

    def test_regeneration_preserves_existing_descriptions(self):
        directory = importer.ROOT / 'yellowstone/infobank/isa'
        for name, family in importer.TARGETS.items():
            with self.subTest(isa=name):
                original = (directory / (name + '.isa')).read_text()
                bare = importer.GENERATED.sub('', original)
                updated, stats = importer.enrich(bare, family, self.tables[family], [], 'test')
                repeated, repeated_stats = importer.enrich(updated, family, self.tables[family], [], 'test')
                self.assertEqual(importer.GENERATED.sub('', updated), bare)
                self.assertEqual(repeated, updated)
                self.assertEqual(repeated_stats, stats)
                self.assertGreater(stats[0], 0)


if __name__ == '__main__':
    unittest.main()
