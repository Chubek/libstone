"""Link and execute real ELF inputs; inspect generated data without tool output parsing."""
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

ld, cc = sys.argv[1:]


def run(*args, expected=0):
    p = subprocess.run(args, capture_output=True, text=True)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


def section(path, name):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    shoff = struct.unpack_from('<Q', data, 40)[0]
    entsize, count, strings = struct.unpack_from('<HHH', data, 58)
    headers = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * entsize) for i in range(count)]
    names = data[headers[strings][4]:headers[strings][4] + headers[strings][5]]
    for h in headers:
        if names[h[0]:].split(b'\0')[0].decode() == name:
            return data[h[4]:h[4] + h[5]], h[3]
    raise AssertionError(name)


with tempfile.TemporaryDirectory(prefix='qld-scripts-') as tmp:
    root = pathlib.Path(tmp)
    src = root / 'input.s'
    src.write_text('''
.text
.global _start
_start:
    mov ptr(%rip), %rsi
    mov (%rsi), %rdi
    mov $60, %eax
    syscall
.data
.balign 16
ptr: .quad target
target: .quad 7
''')
    obj = root / 'input.o'
    run(cc, '-c', str(src), '-o', str(obj))
    script = root / 'layout.ld'
    script.write_text('''ENTRY(_start)
MEMORY { RAM : ORIGIN = 0x401000, LENGTH = 64K }
SECTIONS {
 .text : { *(.text) } >RAM
 .data : { BYTE(0xaa) . = ALIGN(16); begin = .; *(.data) end = .;
           ASSERT(end > begin, "missing input data") } >RAM
 .meta : { SHORT(0x1234) LONG(0x12345678) QUAD(0x1122334455667788)
           FILL(0xcc) . += 3; PROVIDE(meta_end = .); } >RAM
}
ASSERT(SIZEOF(.meta) == 17, "incorrect metadata size")
''')
    exe = root / 'native'
    run(ld, '-T', str(script), str(obj), '-o', str(exe))
    run(str(exe), expected=7)
    data, _ = section(exe, '.meta')
    assert data == bytes.fromhex('3412785634128877665544332211cccccc'), data.hex()
    data, addr = section(exe, '.data')
    assert data[0] == 0xaa and addr % 16 == 0, (data.hex(), addr)
    lua = root / 'layout.lua'
    lua.write_text('''
qld.entry('_start')
qld.macro('ordinary', function(name) qld.section(name, name .. '*') end)
for _, name in ipairs({'.text', '.data', '.bss'}) do qld.ordinary(name) end
qld.emit('SECTIONS { .meta : { LONG(42) } }')
''')
    run(ld, '-T', str(lua), str(obj), '-o', str(exe))
    run(str(exe), expected=7)
    assert section(exe, '.meta')[0] == b'\x2a\0\0\0'
    run(ld, '--check-script', '-T', str(lua))
    include = root / 'include.ld'
    include.write_text('INCLUDE "layout.ld"\n')
    run(ld, '-T', str(include), str(obj), '-o', str(exe))
    run(str(exe), expected=7)
    run(ld, '--script-text', 'ENTRY(_start)', str(obj), '-o', str(exe))
    run(str(exe), expected=7)
    # Explicit script INPUT is enough without command-line objects, and symbols
    # declared by the script must survive discovery and resolve relocations.
    original_obj = obj
    obj = root / 'symbols.o'
    src.write_text('''
.text
.global _start, alternate
_start: mov $answer, %edi
    mov $60, %eax
    syscall
alternate: mov $9, %edi
    mov $60, %eax
    syscall
''')
    run(cc, '-c', str(src), '-o', str(obj))
    script.write_text('INPUT("symbols.o")\nENTRY(alternate)\nanswer = 11;\nSECTIONS { .text : { *(.text) } }')
    run(ld, '-T', str(script), '-e', '_start', '-o', str(exe))
    run(str(exe), expected=11)
    run(ld, '-T', str(script), '-o', str(exe))
    run(str(exe), expected=9)
    ar = shutil.which('ar')
    assert ar, 'ar is required for SEARCH_DIR/-l coverage'
    run(ar, 'rcs', str(root / 'libentry.a'), str(obj))
    script.write_text(f'SEARCH_DIR("{root}")\nINPUT(-lentry)\nENTRY(_start)\nanswer = 13; PROVIDE(answer = missing);')
    run(ld, '-T', str(script), '-o', str(exe))
    run(str(exe), expected=13)
    obj = original_obj
    for text, diagnostic in [
        ('SECTIONS { .text : { *(.text) } x = MYSTERY(3); }', 'unknown linker expression'),
        ('SECTIONS { .text : { *(.text) } x = 1 / 0; }', 'division by zero'),
        ('SECTIONS { .text : { *(.text) } x = missing; }', 'undefined'),
        ('SECTIONS { .text : { *(.text) } x = SIZEOF(.missing); }', 'unknown name'),
        ('SECTIONS { .text : { *(.text) } x = ORIGIN(missing); }', 'unknown name'),
        ('SECTIONS { .text 0x401000 : { *(.text) } .data 0x401000 : { *(.data) } }', 'overlapping'),
        ('SECTIONS { .text : { *(.text) . = 0; } }', 'backwards'),
        ('SECTIONS { .text : { *(.text) } .data : { BYTE(256) } }', 'out of range'),
        ('MEMORY { tiny : ORIGIN = 0x401000, LENGTH = 1 } SECTIONS { .text : { *(.text) } >tiny }', 'MEMORY region overflow'),
        ('SECTIONS { .text 0x10 : { *(.text) } }', 'overlaps ELF headers'),
        ('--!lua\nerror("lua-error-marker")', 'lua-error-marker'),
    ]:
        p = subprocess.run([ld, '--script-text', text, str(obj), '-o', str(exe)], capture_output=True, text=True)
        assert p.returncode and diagnostic in p.stderr, (text, p.returncode, p.stderr)
print('linker Lua/layout/relocation executable regressions passed')
