#!/usr/bin/env python3
"""Import source-table metadata from the local binutils 2.22 opcode tables.

The generated fields are reference variants, not replacement ISA encodings.
Only marked fields are regenerated. Requires Python 3.10+ and a C preprocessor.
"""

import argparse
import ast
import hashlib
import json
import operator
from pathlib import Path
import re
import shlex
import subprocess


ROOT = Path(__file__).resolve().parents[2]
STRING = r'"(?:\\.|[^"\\])*"'
COMMENT = re.compile(rf'{STRING}|/\*.*?\*/|//[^\n]*', re.S)
GENERATED = re.compile(
    r'^  # BEGIN BINUTILS METADATA\n.*?^  # END BINUTILS METADATA\n', re.M | re.S
)
TARGETS = {
    'arm32': 'arm', 'avr': 'avr', 'm68k': 'm68k',
    'mips32': 'mips', 'mips64': 'mips', 'ppc32': 'ppc', 'ppc64': 'ppc',
    's390x': 's390', 'sparc64': 'sparc', 'x86': 'x86',
}
SOURCES = {
    'arm': ['opcodes/arm-dis.c', 'include/opcode/arm.h'],
    'avr': ['opcodes/avr-dis.c', 'include/opcode/avr.h'],
    'm68k': ['opcodes/m68k-opc.c', 'include/opcode/m68k.h'],
    'mips': ['opcodes/mips-opc.c', 'include/opcode/mips.h'],
    'ppc': ['opcodes/ppc-opc.c', 'include/opcode/ppc.h'],
    's390': ['opcodes/s390-opc.txt', 'opcodes/s390-opc.c',
             'opcodes/s390-mkopc.c', 'include/opcode/s390.h'],
    'sparc': ['opcodes/sparc-opc.c', 'include/opcode/sparc.h'],
    'x86': ['opcodes/i386-opc.tbl', 'opcodes/i386-opc.h'],
}


class Hex(int):
    """An integer rendered as an ISA hexadecimal literal."""


def uncomment(text):
    # Keep offsets/newlines, and never treat comment characters in strings as comments.
    return COMMENT.sub(lambda m: m[0] if m[0].startswith('"') else
                       re.sub(r'[^\n]', ' ', m[0]), text)


def quoted(text):
    parts = re.findall(STRING, text)
    if not parts or re.sub(STRING, '', text).strip():
        raise ValueError(f'Expected C string literal: {text}')
    return ''.join(ast.literal_eval(part) for part in parts)


def delimiters(text):
    """Yield punctuation outside strings (including escaped quotes)."""
    for match in re.finditer(rf'{STRING}|[{{}}()[\],;]', text):
        if not match[0].startswith('"'):
            yield match.start(), match[0]


def split_fields(text):
    fields, depth, start = [], 0, 0
    for pos, char in delimiters(text):
        if char in '({[':
            depth += 1
        elif char in ')}]':
            depth -= 1
        elif char == ',' and depth == 0:
            fields.append(text[start:pos].strip())
            start = pos + 1
    if depth:
        raise ValueError('Unbalanced table entry')
    if text[start:].strip():
        fields.append(text[start:].strip())
    return fields


def closing_brace(text, start):
    depth = 0
    for pos, char in delimiters(text[start:]):
        if char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if not depth:
                return start + pos
    raise ValueError('Unterminated block')


def table_rows(text, table):
    clean = uncomment(text)
    match = re.search(r'\b' + re.escape(table) + r'\s*\[\s*\]\s*=\s*\{', clean)
    if not match:
        raise ValueError(f'Table not found: {table}')
    start = match.end() - 1
    end = closing_brace(clean, start)
    body = clean[start + 1:end]
    depth, row_start = 0, 0
    for pos, char in delimiters(body):
        if char == '{':
            if depth == 0:
                row_start = pos
            depth += 1
        elif char == '}':
            depth -= 1
            if depth == 0:
                offset = start + 1 + row_start
                yield clean.count('\n', 0, offset) + 1, split_fields(body[row_start + 1:pos])


def defines(text):
    return '\n'.join(re.findall(r'^[ \t]*#\s*define[^\n]*',
                                uncomment(text).replace('\\\n', ' '), re.M))


def cpp(text, command, line_markers=False):
    args = shlex.split(command) + ['-E', '-x', 'c', '-nostdinc', '-undef']
    if not line_markers:
        args.append('-P')
    result = subprocess.run(args + ['-'], input=text, text=True, capture_output=True)
    if result.returncode:
        raise ValueError(f'C preprocessor failed:\n{result.stderr}')
    return result.stdout


def integer(expression):
    """Evaluate only integer/bitwise C constant expressions, never Python eval."""
    expression = re.sub(r'\(\s*(?:(?:unsigned|signed|long|short|int)\s*)+\)', '', expression)
    expression = re.sub(r'\b(0[xX][0-9a-fA-F]+|[0-9]+)[uUlL]+\b', r'\1', expression)
    expression = re.sub(r'\b0([0-7]+)\b', r'0o\1', expression)
    expression = re.sub(r'!\s*([01])\b', lambda m: str(1 - int(m[1])), expression)
    binary = {ast.BitOr: operator.or_, ast.BitAnd: operator.and_, ast.BitXor: operator.xor,
              ast.LShift: operator.lshift, ast.RShift: operator.rshift,
              ast.Add: operator.add, ast.Sub: operator.sub, ast.Mult: operator.mul}
    unary = {ast.Invert: operator.invert, ast.USub: operator.neg, ast.UAdd: operator.pos}

    def visit(node):
        if isinstance(node, ast.Constant) and type(node.value) is int:
            return node.value
        if isinstance(node, ast.BinOp) and type(node.op) in binary:
            return binary[type(node.op)](visit(node.left), visit(node.right))
        if isinstance(node, ast.UnaryOp) and type(node.op) in unary:
            return unary[type(node.op)](visit(node.operand))
        raise ValueError(f'Unsupported integer expression: {expression}')

    return visit(ast.parse(expression.strip(), mode='eval').body)


def resolve_constants(records, definitions, command):
    slots = [(row, key) for row in records for key in ('match', 'mask') if key in row]
    text = definitions + '\n' + '\n'.join(f'ISA_VALUE {row[key]}' for row, key in slots)
    values = re.findall(r'^ISA_VALUE (.+)$', cpp(text, command), re.M)
    if len(values) != len(slots):
        raise ValueError('C preprocessor lost opcode expressions')
    for (row, key), value in zip(slots, values):
        row[key] = Hex(integer(value) & 0xffffffff)


def record(source, line, table, **fields):
    return {'source': 'binutils/' + source, 'line': line, 'table': table, **fields}


def preprocess_source(text, source, command, prefix=''):
    clean = re.sub(r'^[ \t]*#include[^\n]*', lambda m: ' ' * len(m[0]), uncomment(text), flags=re.M)
    output = cpp(prefix + f'\n#line 1 "{source}"\n' + clean, command, True)
    lines, origins, current = [], [], 0
    for line in output.splitlines():
        marker = re.match(r'#\s+(\d+)\s+"', line)
        if marker:
            current = int(marker[1])
        else:
            lines.append(line)
            origins.append(current)
            current += 1
    return '\n'.join(lines), origins


def load_tables(root, command):
    tables = {}

    for family, table in [('mips', 'mips_builtin_opcodes'), ('ppc', 'powerpc_opcodes'),
                          ('m68k', 'm68k_opcodes')]:
        source = SOURCES[family][0]
        text = (root / source).read_text()
        definitions = defines(text[:text.index(table + '[]')])
        origins = None
        if family == 'm68k':
            text, origins = preprocess_source(text, source, command)
            definitions = ''
        rows = []
        for line, fields in table_rows(text, table):
            if origins:
                line = origins[line - 1]
            if family == 'mips':
                name, operands, match, mask, pinfo, pinfo2, membership = fields
                row = record(source, line, table, mnemonic=quoted(name),
                             operand_codes=quoted(operands), pinfo=pinfo, pinfo2=pinfo2,
                             membership=membership)
                if pinfo == 'INSN_MACRO':
                    row.update(kind='assembler_macro', macro_id=mask.removeprefix('(int)').strip())
                else:
                    row.update(kind='instruction', match=match, mask=mask, encoding_bits=32)
            elif family == 'ppc':
                name, match, mask, flags, deprecated, operands = fields
                row = record(source, line, table, mnemonic=quoted(name), match=match,
                             mask=mask, encoding_bits=32, flags=flags, deprecated=deprecated,
                             operand_codes=split_fields(operands.strip('{} ')))
            else:
                name, size, opcode, mask, operands, arch = fields
                row = record(source, line, table, mnemonic=quoted(name), match=opcode,
                             mask=mask, match_layout='first_word_high16',
                             pseudo_size_bytes=int(size), operand_codes=quoted(operands),
                             architecture=arch)
            rows.append(row)
        resolve_constants(rows, definitions, command)
        if family == 'm68k':
            by_name = {}
            for row in rows:
                by_name.setdefault(row['mnemonic'], []).append(row)
            for line, fields in table_rows(text, 'm68k_opcode_aliases'):
                alias, target = map(quoted, fields)
                for row in by_name.get(target, []):
                    rows.append({**row, 'alias': alias, 'alias_line': origins[line - 1]})
        tables[family] = rows

    # SPARC encodes many complete rows in nested macros. Expand them with the
    # actual preprocessor, retaining architecture and F_* flag names as symbols.
    source = SOURCES['sparc'][0]
    text = (root / source).read_text()
    header = (root / 'include/opcode/sparc.h').read_text()
    header_defs = '\n'.join(d for d in defines(header).splitlines()
                            if not re.match(r'\s*#define\s+F_', d))
    clean = uncomment(text)
    clean = re.sub(r'^#define\s+(?:v\d\w*|sparclet|sparclite)[ \t]+(?:[^\n]*\\\n)*[^\n]*',
                   lambda m: '\n' * m[0].count('\n'), clean, flags=re.M)
    output, origins = preprocess_source(clean, source, command, header_defs)
    rows = []
    for line, fields in table_rows(output, 'sparc_opcodes'):
        name, match, lose, operands, flags, arch = fields
        match, lose = integer(match) & 0xffffffff, integer(lose) & 0xffffffff
        rows.append(record(source, origins[line - 1], 'sparc_opcodes', mnemonic=quoted(name),
                           match=Hex(match), lose=Hex(lose), mask=Hex(match | lose),
                           encoding_bits=32, operand_codes=quoted(operands), flags=flags,
                           architecture=arch))
    tables['sparc'] = rows

    source = SOURCES['arm'][0]
    text = (root / source).read_text()
    rows = []
    for table in ('arm_opcodes', 'coprocessor_opcodes', 'neon_opcodes'):
        for line, fields in table_rows(text, table):
            arch, match, mask, assembler = fields
            if not assembler.startswith('"') or not int(mask, 0):
                continue
            # A trailing diagnostic macro is not part of the assembler template.
            template = ''.join(ast.literal_eval(s) for s in re.findall(STRING, assembler))
            rows.append(record(source, line, table, assembler=template, architecture=arch,
                               match=Hex(int(match, 0)), mask=Hex(int(mask, 0)), encoding_bits=32))
    tables['arm'] = rows

    source = 'include/opcode/avr.h'
    rows = []
    for line, text in enumerate(uncomment((root / source).read_text()).splitlines(), 1):
        match = re.match(r'AVR_INSN\s*\((.*)\)', text)
        if not match:
            continue
        name, operands, pattern, size, arch, opcode = split_fields(match[1])
        pattern = quoted(pattern)
        mask = int(''.join('1' if c in '01' else '0' for c in pattern), 2)
        rows.append(record(source, line, 'avr_opcodes', mnemonic=name,
                           operand_codes=quoted(operands), bit_pattern=pattern,
                           encoding_bits=16 * int(size), match_bits=16,
                           match=Hex(int(opcode, 0)), mask=Hex(mask), architecture=arch))
    tables['avr'] = rows

    source = SOURCES['x86'][0]
    rows = []
    for line, text in enumerate(uncomment((root / source).read_text()).splitlines(), 1):
        if not text.strip():
            continue
        name, count, opcode, extension, length, cpu, modifiers, operands = split_fields(text)
        rows.append(record(source, line, 'i386_optab', mnemonic=name, operand_count=int(count),
                           base_opcode=Hex(int(opcode, 0)), extension_opcode=extension,
                           opcode_length=int(length), cpu_flags=cpu, modifiers=modifiers,
                           operand_types=split_fields(operands.strip('{} '))))
    tables['x86'] = rows

    source = SOURCES['s390'][0]
    formats = uncomment((root / 'opcodes/s390-opc.c').read_text())
    sizes = dict(re.findall(r'#define INSTR_(\w+)\s+(\d+),', formats))
    operands = dict(re.findall(r'#define INSTR_(\w+)\s+\d+,\s*\{([^}]+)\}', formats))
    masks = dict(re.findall(r'#define MASK_(\w+)\s*\{([^}]+)\}', formats))
    # Conditional mnemonic expansions use the same nibble/extension tables as mkopc.
    generator = (root / 'opcodes/s390-mkopc.c').read_text()
    extensions = {}
    for tag, table in [('*', 's390_cond_extensions'), ('$', 's390_crb_extensions')]:
        body = generator.split(table + '[', 1)[1].split('};', 1)[0]
        extensions[tag] = re.findall(r"\{\s*'([0-9a-f])',\s*\"(\w+)\"", body)
    rows = []
    for line, text in enumerate((root / source).read_text().splitlines(), 1):
        if not text.strip() or text.lstrip().startswith('#'):
            continue
        opcode, name, fmt, description, cpu, modes = shlex.split(text)
        names = [(name, opcode)]
        tag = re.search(r'([*$])(\d+)', name)
        if tag:
            index = int(tag[2]) // 4
            names = [(name[:tag.start()] + suffix + name[tag.end():],
                      opcode[:index] + nibble + opcode[index + 1:])
                     for nibble, suffix in extensions[tag[1]]]
        for mnemonic, code in names:
            size = int(sizes[fmt])
            byte_mask = [int(v.strip(), 0) for v in masks[fmt].split(',')][:size]
            rows.append(record(source, line, 's390_opcodes', mnemonic=mnemonic,
                               opcode_template=code, format=fmt, description=description,
                               minimum_cpu=cpu, modes=modes.split(','), encoding_bits=size * 8,
                               match=Hex(int(code.replace('?', '0').ljust(12, '0')[:size * 2], 16)),
                               mask=Hex(int.from_bytes(bytes(byte_mask), 'big')),
                               operand_codes=[v.strip() for v in operands[fmt].split(',')
                                              if v.strip() != '0']))
    tables['s390'] = rows
    return tables


def render(value):
    if isinstance(value, Hex):
        return hex(value)
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=True)
    if isinstance(value, dict):
        return '{ ' + ' '.join(f'{key} = {render(val)};' for key, val in value.items()) + ' }'
    if isinstance(value, list):
        return '[' + ', '.join(render(item) for item in value) + ']'
    if type(value) is int:
        return str(value)
    raise TypeError(type(value))


def metadata(fields):
    lines = ['  # BEGIN BINUTILS METADATA', '  binutils = {']
    for key, value in fields.items():
        if key == 'variants':
            lines.append('    variants = [')
            lines.extend('      ' + render(row) + (',' if i + 1 < len(value) else '')
                         for i, row in enumerate(value))
            lines.append('    ];')
        else:
            lines.append(f'    {key} = {render(value)};')
    return '\n'.join(lines + ['  };', '  # END BINUTILS METADATA', ''])


def arm_mnemonic(template, word):
    """Resolve the mnemonic controls used by the imported A32 descriptions."""
    mnemonic = template.split('\t')[0]
    condition = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc',
                 'hi', 'ls', 'ge', 'lt', 'gt', 'le', '', ''][(word >> 28) & 15]
    mnemonic = mnemonic.replace('%c', condition).replace('%u', '')
    mnemonic = re.sub(r"%(\d+)'(.)", lambda m: m[2] if word & (1 << int(m[1])) else '', mnemonic)
    mnemonic = re.sub(r'%(\d+)`(.)', lambda m: '' if word & (1 << int(m[1])) else m[2], mnemonic)
    mnemonic = re.sub(r'%(\d+)\?(.)(.)', lambda m: m[2] if word & (1 << int(m[1])) else m[3], mnemonic)
    # %p is the obsolete test/compare P suffix; %t is the post-indexed T suffix.
    mnemonic = mnemonic.replace('%p', 'p' if (word >> 12) & 15 == 15 else '')
    mnemonic = mnemonic.replace('%t', 't' if word & 0x01200000 == 0x00200000 else '')
    return mnemonic


def enrich(text, family, rows, provenance, version):
    text = GENERATED.sub('', text)
    encodings = {}
    for match in re.finditer(r'^encoding ([\w.]+)\s*\{', text, re.M):
        end = closing_brace(text, match.end() - 1)
        base = re.search(r'\bbase\s*=\s*(0x[0-9a-fA-F]+|\d+)\s*;', text[match.end():end])
        if base:
            encodings[match[1]] = int(base[1], 0)
    indexed = {}
    for row in rows:
        indexed.setdefault(row.get('alias', row.get('mnemonic')), []).append(row)
    edits, matched, total, variants, missing = [], 0, 0, 0, []
    for match in re.finditer(r'^op ([\w.]+)\s*\{', text, re.M):
        total += 1
        end = closing_brace(text, match.end() - 1)
        body = text[match.end():end]
        syntax = re.search(r'\bsyntax\s*=\s*(' + STRING + r')\s*;', body)
        mnemonic = quoted(syntax[1]).split()[0] if syntax else match[1]
        if family == 'm68k':
            mnemonic = mnemonic.replace('.', '')
        if family == 'sparc':
            mnemonic = mnemonic.split(',')[0]
        method = 'syntax_mnemonic'
        if family == 'x86' and mnemonic in ('pushfq', 'popfq'):
            mnemonic = mnemonic[:-1]
            method = 'gas_size_suffix'
        selected = indexed.get(mnemonic, [])
        if family == 'arm':
            method = 'a32_base_bits_and_mnemonic'
            encoding = re.search(r'\bencoding\s*=\s*([\w.]+)', body)
            base = encodings.get(encoding[1]) if encoding else None
            selected = [] if base is None else [r for r in rows
                if base & r['mask'] == r['match'] and arm_mnemonic(r['assembler'], base) == mnemonic]
        elif family == 'x86':
            selected = [r for r in selected if 'CpuNo64' not in r['cpu_flags'].split('|')]
        elif family == 'sparc':
            selected = [r for r in selected if r['architecture'] not in
                        ('v6notv9', 'sparclet', 'sparclite')]
        elif family == 's390':
            selected = [r for r in selected if 'zarch' in r['modes']]
        if selected:
            matched += 1
            variants += len(selected)
            edits.append((end, metadata({'lookup': method, 'mnemonic': mnemonic, 'variants': selected})))
        else:
            missing.append(match[1])
    profile = re.search(r'^profile\s*\{', text, re.M)
    if not profile:
        raise ValueError('Missing ISA profile')
    fields = {'version': version, 'scope': 'source_table_reference_variants', 'sources': provenance,
              'matched_operations': matched, 'variant_count': variants, 'unmatched_operations': missing}
    edits.append((closing_brace(text, profile.end() - 1), metadata(fields)))
    for offset, addition in sorted(edits, reverse=True):
        text = text[:offset] + addition + text[offset:]
    return text, (matched, total, variants, missing)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binutils', type=Path, default=ROOT / 'binutils')
    parser.add_argument('--isa-dir', type=Path, default=ROOT / 'yellowstone/infobank/isa')
    parser.add_argument('--cpp', default='cc', help='C compiler/preprocessor command')
    parser.add_argument('--check', action='store_true', help='report stale generated fields without writing')
    args = parser.parse_args()
    version_text = (args.binutils / 'bfd/configure.in').read_text()
    version = re.search(r'AM_INIT_AUTOMAKE\(bfd,\s*([^\)]+)\)', version_text)[1]
    tables = load_tables(args.binutils, args.cpp)
    pending = []
    for name, family in TARGETS.items():
        path = args.isa_dir / (name + '.isa')
        original = path.read_text()
        provenance = [{'path': 'binutils/' + source,
                       'sha256': hashlib.sha256((args.binutils / source).read_bytes()).hexdigest()}
                      for source in SOURCES[family]]
        updated, (matched, total, variants, missing) = enrich(original, family, tables[family],
                                                             provenance, version)
        print(f'{path.name}: {matched}/{total} operations, {variants} variants'
              + (f'; unmatched: {", ".join(missing)}' if missing else ''))
        if original != updated:
            pending.append((path, updated))
    # Parse all inputs successfully before changing any file.
    for path, updated in pending:
        if args.check:
            print(f'STALE: {path}')
        else:
            path.write_text(updated)
    unsupported = sorted(p.name for p in args.isa_dir.glob('*.isa')
                         if not p.name.startswith('.') and p.stem not in TARGETS
                         and not p.read_text().startswith('# Generated by generate_binutils_isa.py;'))
    print('No corresponding source table: ' + ', '.join(unsupported))
    print(f'{len(pending)} file(s) ' + ('stale' if args.check else 'updated'))
    return int(args.check and bool(pending))


if __name__ == '__main__':
    raise SystemExit(main())
