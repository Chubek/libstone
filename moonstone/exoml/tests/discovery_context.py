"""Compiler-backed macro state, compile database security, target/ABI and cache tests."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

tool, resources, compiler = sys.argv[1:]


def extract(header, *args, expected=0):
    p = subprocess.run([tool, 'extract', '--header', str(header), '--resource-dir', resources, *args],
                       text=True, capture_output=True, timeout=30)
    assert p.returncode == expected, (p.returncode, p.stderr)
    unit = json.loads(p.stdout)
    assert unit['complete'] == (expected == 0)
    assert unit['schema_version'] == 2 and not unit['safe_export'] and unit['unknown_effects']
    return unit


def named(unit, name):
    return next(d for d in unit['declarations'] if d['name'] == name)


def typ(unit, spelling):
    return next(t for t in unit['types'] if t['spelling'] == spelling)


with tempfile.TemporaryDirectory(prefix='exoml-context-') as temp:
    root = Path(temp)
    include = root / 'include directory'
    include.mkdir()
    config = include / 'configuration.h'
    config.write_text('#define INCLUDED_VALUE 7\n')
    header = root / 'public.h'
    header.write_text('''#include <configuration.h>
#define GONE 1
#undef GONE
#define RESTORED 1
#pragma push_macro("RESTORED")
#undef RESTORED
#define RESTORED 2
#pragma pop_macro("RESTORED")
#define ACTIVE 3
#undef ACTIVE
#define ACTIVE 4
#define DECLARE(name) long name(long value)
DECLARE(generated);
#if CHOICE == 23 && INCLUDED_VALUE == 7
long selected(long value);
#else
int alternative(int value);
#endif
''')
    main = root / 'source.c'
    main.write_text('#include "public.h"\n')
    database = root / 'compile_commands.json'
    sentinel = root / 'executed'
    driver = root / 'never-run'
    driver.write_text('#!/bin/sh\ntouch "' + str(sentinel) + '"\n')
    driver.chmod(0o700)
    args = [str(driver), '-std=c11', '-I', 'include directory', '-DCHOICE=23', '-UNEVER_DEFINED',
            '-O2', '-g', '-Wall', '-c', 'source.c', '-o', 'must-not-exist.o', '-MMD', '-MF', 'must-not-exist.d']

    def write(arguments=None, command=None, entries=None):
        entry = {'directory': str(root), 'file': 'source.c'}
        entry.update({'command': command} if command is not None else {'arguments': args if arguments is None else arguments})
        database.write_text(json.dumps(entries if entries is not None else [entry]))

    write()
    flags = ['--compile-commands', str(database), '--compile-file', str(main)]
    u = extract(header, *flags)
    assert named(u, 'selected')['kind'] == 'function' and named(u, 'generated')['kind'] == 'function'
    assert u['compilation']['driver'] == str(driver)
    assert u['compilation']['directory'] == str(root)
    assert u['compilation']['original_arguments'] == args
    assert '-o' in u['compilation']['ignored_arguments'] and '-MF' in u['compilation']['ignored_arguments']
    assert str(database) in {x['path'] for x in u['inputs']}
    assert not sentinel.exists() and not (root / 'must-not-exist.o').exists() and not (root / 'must-not-exist.d').exists()
    pp = u['preprocessing']
    assert pp['final_snapshot_complete'] and pp['skipped_ranges'] and pp['includes']
    states = {m['name']: m for m in pp['final_macros']}
    definitions = {m['id']: m for m in pp['definitions']}
    assert states['ACTIVE']['defined'] and definitions[states['ACTIVE']['definition_id']]['tokens'][-1] == '4'
    assert states['GONE']['defined'] is False and states['NEVER_DEFINED']['defined'] is False
    assert states['RESTORED']['defined'] is True
    assert states['__LINE__']['defined'] is True
    for name in ('RESTORED', '__LINE__'):
        if not states[name]['definition_id']:
            assert states[name]['unknown_reason']
    assert definitions[states['CHOICE']['definition_id']]['source']['file'] == '<command line>'
    assert definitions[states['CHOICE']['definition_id']]['source']['line'] == definitions[states['CHOICE']['definition_id']]['source']['end_line']
    assert any(d['name'] == 'INCLUDED_VALUE' and d['source']['file'] == str(config) for d in definitions.values())
    generated = next(e for e in pp['expansions'] if e['name'] == 'DECLARE')
    assert generated['definition_id'] in definitions and generated['source']['file'] == str(header)
    assert 'nested' in pp['coverage']
    assert 'long' in named(u, 'selected')['tokens']
    assert named(u, 'selected')['parameters'][0]['tokens'] == 'long value'
    assert u == extract(header, *flags)
    forced = extract(header, *flags, '--compile-arg', '-include', '--compile-arg', str(config))
    assert forced['preprocessing']['final_snapshot_complete']
    assert str(config) in {x['path'] for x in forced['inputs']}
    overridden = extract(header, *flags, '--undefine', 'CHOICE')
    assert named(overridden, 'alternative')['kind'] == 'function'
    assert u['fingerprint'] != overridden['fingerprint']
    # A command string is tokenized, never evaluated by a shell.
    write(command="'" + str(driver) + "' -std=c11 -I 'include directory' -DCHOICE=23 -c source.c -o ignored.o")
    assert named(extract(header, *flags), 'selected')['kind'] == 'function'
    assert not sentinel.exists()
    for command in (f'cc $(touch {sentinel}) -c source.c', f'cc -c source.c; touch {sentinel}',
                    'cc -c "source.c', 'cc -c source.c | cat'):
        write(command=command)
        assert extract(header, *flags, expected=3)['diagnostics']
    for unsafe in ('-Xclang', '-fplugin=bad.so', '@response', '-std=c++20', '-include-pch', '-objc-arc', '-unknown-abi-option'):
        write(arguments=[str(driver), unsafe, '-c', 'source.c'])
        assert extract(header, *flags, expected=3)['diagnostics']
    assert not sentinel.exists()
    # Malformed/duplicate metadata, ambiguity and budget exhaustion are errors.
    database.write_text('[{"directory":"' + str(root) + '","file":"source.c","file":"other.c","arguments":[]}]')
    assert extract(header, *flags, expected=3)['diagnostics']
    database.write_text('{"invalid":true}')
    assert extract(header, *flags, expected=3)['diagnostics']
    write(entries=[{'directory': str(root), 'file': 'source.c', 'arguments': args}] * 2)
    assert extract(header, *flags, expected=3)['diagnostics']
    write()
    assert extract(header, '--compile-file', str(main), expected=3)['diagnostics']
    assert extract(header, *flags, '--max-input-bytes', '16', expected=3)['diagnostics']
    # Minimal target/ABI fixture: no host-system includes are needed to cross-parse.
    abi = root / 'abi.h'
    abi.write_text('enum small { ZERO, MAXIMUM = 255 };\nstruct layout { char tag; long value; };\nchar scalar(char value);\n')
    base = extract(abi)
    short = extract(abi, '--compile-arg', '-fshort-enums')
    packed = extract(abi, '--compile-arg', '-fpack-struct=1')
    unsigned = extract(abi, '--compile-arg', '-funsigned-char')
    signed = extract(abi, '--compile-arg', '-fsigned-char')
    assert typ(short, 'enum small')['size'] == 1 < typ(base, 'enum small')['size']
    assert typ(short, 'enum small')['id'] != typ(base, 'enum small')['id']
    assert typ(packed, 'struct layout')['alignment'] == 1 < typ(base, 'struct layout')['alignment']
    assert typ(unsigned, 'char')['signed'] is False and typ(signed, 'char')['signed'] is True
    target32 = extract(abi, '--compile-arg', '-m32')
    target64 = extract(abi, '--compile-arg', '-m64')
    assert target32['target']['pointer_bits'] == 32 and target64['target']['pointer_bits'] == 64
    assert typ(target32, 'long')['size'] == 4 and typ(target64, 'long')['size'] == 8
    assert len({x['fingerprint'] for x in (base, short, packed, unsigned, signed, target32, target64)}) == 7
    probe_source = root / 'probe.c'
    probe_source.write_text('#include "abi.h"\n#include <stdio.h>\nint main(void) { printf("%zu %zu %d\\n", sizeof(enum small), _Alignof(struct layout), (char)-1 < 0); return 0; }\n')
    for flag, unit in ((None, base), ('-fshort-enums', short), ('-fpack-struct=1', packed),
                       ('-funsigned-char', unsigned), ('-fsigned-char', signed)):
        executable = root / 'probe'
        command = [compiler, '-std=c11', '-Wall', '-Wextra', '-Wpedantic']
        subprocess.run([*command, *([flag] if flag else []), str(probe_source), '-o', str(executable)], check=True)
        enum_size, alignment, signed_char = map(int, subprocess.check_output([str(executable)], text=True).split())
        assert enum_size == typ(unit, 'enum small')['size']
        assert alignment == typ(unit, 'struct layout')['alignment']
        assert bool(signed_char) == typ(unit, 'char')['signed']
    for bad in ('-fplugin=bad', '-Xclang', '@response', '--target=', '-std=gnu17'):
        assert extract(abi, '--compile-arg', bad, expected=3)['diagnostics']
    # Compile database content and ABI flags participate in persistent cache keys.
    cache = root / 'cache'
    write()
    a = subprocess.run([tool, 'extract', '--header', str(header), '--resource-dir', resources,
                        *flags, '--cache-dir', str(cache), '--cache-report'], text=True, capture_output=True, check=True)
    b = subprocess.run([tool, 'extract', '--header', str(header), '--resource-dir', resources,
                        *flags, '--cache-dir', str(cache), '--cache-report'], text=True, capture_output=True, check=True)
    assert a.stdout == b.stdout
    assert json.loads(a.stderr)['cache']['status'] == 'miss' and json.loads(b.stderr)['cache']['status'] == 'hit'
    write(arguments=[x.replace('-DCHOICE=23', '-DCHOICE=24') for x in args])
    c = subprocess.run([tool, 'extract', '--header', str(header), '--resource-dir', resources,
                        *flags, '--cache-dir', str(cache), '--cache-report'], text=True, capture_output=True, check=True)
    assert json.loads(c.stderr)['cache']['status'] == 'miss'
    assert json.loads(c.stderr)['cache']['key'] != json.loads(a.stderr)['cache']['key']
    assert named(json.loads(c.stdout), 'alternative')['kind'] == 'function'
