"""Execute and inspect native ELF links with an explicit optimization hook."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ld, cc, ar = sys.argv[1:]
native_env = dict(os.environ, PATH='/nonexistent', QLD_TOOLCHAIN_BACKEND_ACTIVE='1')


def run(*args, expected=0, **kwargs):
    p = subprocess.run(args, capture_output=True, text=True, timeout=10, **kwargs)
    assert p.returncode == expected, (args, p.returncode, p.stdout, p.stderr)
    return p


def link(*args, **kwargs):
    return run(ld, *args, env=native_env, **kwargs)


def elf(path):
    """Read section payloads and global definitions without relying on nm."""
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x02\x01'
    assert struct.unpack_from('<HH', data, 16) == (2, 62)
    shoff = struct.unpack_from('<Q', data, 40)[0]
    entsize, count, strings = struct.unpack_from('<HHH', data, 58)
    heads = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * entsize)
             for i in range(count)]
    names = data[heads[strings][4]:heads[strings][4] + heads[strings][5]]
    names = [names[h[0]:].split(b'\0')[0].decode() for h in heads]
    sections = {name: data[h[4]:h[4] + h[5]] for name, h in zip(names, heads) if h[1] != 8}
    symbols = {}
    for name, _, _, index, value, size in struct.iter_unpack('<IBBHQQ', sections['.symtab']):
        if name:
            name = sections['.strtab'][name:].split(b'\0')[0].decode()
            symbols[name] = (index, value)
            if 0 < index < 0xff00:
                address = heads[index][3]
                assert address <= value <= value + size <= address + heads[index][5], name
    return sections, symbols


assert '--lto-script' in link('--help').stdout
with tempfile.TemporaryDirectory(prefix='qld-lto-') as tmp:
    root = pathlib.Path(tmp)

    def assemble(name, source):
        src, obj = root / (name + '.s'), root / (name + '.o')
        src.write_text(source)
        run(cc, '-c', src, '-o', obj)
        return obj

    start = assemble('start', '''
.section .text.start,"ax",@progbits
.globl _start
.type _start,@function
_start:
    call exit_code
    mov %eax,%edi
    mov $60,%eax
    syscall
.size _start,.-_start
.section .note.GNU-stack,"",@progbits
''')
    helper = assemble('helper', '''
.section .text.exit,"ax",@progbits
.globl exit_code
.type exit_code,@function
exit_code:
    mov .Lpointer(%rip),%rcx
    mov (%rcx),%eax
    ret
.size exit_code,.-exit_code
.section .rodata.ptr,"a",@progbits
.Lpointer: .quad .Lanswer
.section .rodata.answer,"a",@progbits
.globl answer_bytes
answer_bytes:
.Lanswer: .long 42
.section .text.unused,"ax",@progbits
.globl unused
unused: .ascii "DEAD_SECTION_MARKER"
.section .text.extra,"ax",@progbits
.globl extra_root
extra_root: mov extra_data(%rip),%eax; ret
.section .rodata.extra,"a",@progbits
.globl extra_data
extra_data: .long 27
.section .rodata.layout_keep,"a",@progbits
.globl layout_keep
layout_keep: .byte 53
.section .rodata.retain,"aR",@progbits
.globl elf_retain
elf_retain: .byte 113
.section .text.alternate,"ax",@progbits
.globl alternate
alternate: call exit_code; mov %eax,%edi; mov $60,%eax; syscall
.section .note.GNU-stack,"",@progbits
''')

    hook = root / 'optimize.lua'
    hook_text = '''
assert(io == nil and os == nil and package == nil and require == nil and
       load == nil and pcall == nil and rawset == nil and qld == nil)
function optimize(link)
    assert(type(link) == "table" and link.format == "elf" and link.mode == "static-exec")
    assert(link.elf_class == 64 and link.architecture == "x86-64")
    assert(link.entry == "_start" and link.object_count == 2 and link.symbol_count >= 5)
    return { gc_sections = true, keep = {} }
end
'''
    hook.write_text(hook_text)
    baseline, image, mapfile = [root / name for name in ('baseline', 'image', 'link.map')]
    link(start, helper, '-o', baseline)
    link(start, helper, '--gc-sections', '-o', image)
    assert b'DEAD_SECTION_MARKER' in elf(image)[0]['.text']  # legacy switch unchanged

    link('--lto-script', hook, start, helper, '-o', image, '--Map', mapfile)
    run(image, expected=42)
    normal_sections, normal_symbols = elf(baseline)
    sections, symbols = elf(image)
    assert len(sections['.text']) < len(normal_sections['.text'])
    assert b'DEAD_SECTION_MARKER' in normal_sections['.text']
    assert b'DEAD_SECTION_MARKER' not in sections['.text']
    assert set(symbols) == {'_start', 'answer_bytes', 'elf_retain', 'exit_code'}, symbols
    assert {'unused', 'extra_root', 'extra_data', 'layout_keep', 'alternate'} <= normal_symbols.keys()
    assert 'unused' not in mapfile.read_text()
    optimized = image.read_bytes()
    link(f'--lto-script={hook}', start, helper, '-o', image)
    assert optimized == image.read_bytes()
    # No-op remains available and does not implicitly toggle legacy flags.
    hook.write_text('function optimize(link) return { gc_sections = false } end')
    link('--lto-script', hook, start, helper, '-o', image)
    assert image.read_bytes() == baseline.read_bytes()

    hook.write_text('function optimize(link) return {gc_sections=true, keep={"extra_root"}} end')
    link('--lto-script', hook, start, helper, '-o', image)
    run(image, expected=42)
    assert {'extra_root', 'extra_data'} <= elf(image)[1].keys()

    # The prevailing strong definition roots its section, not a weak body
    # in a separate object that names the same symbol.
    weak_start = assemble('weak_start', start.with_suffix('.s').read_text() + '''
.section .text.weak,"ax",@progbits
.weak exit_code
exit_code: mov $91,%eax; ret
.globl sign_of_weak_body
sign_of_weak_body: .byte 1
''')
    hook.write_text(hook_text)
    link('--lto-script', hook, weak_start, helper, '-o', image)
    run(image, expected=42)
    assert 'sign_of_weak_body' not in elf(image)[1]

    archive = root / 'libhelper.a'
    run(ar, 'rcs', archive, helper)
    response = root / 'inputs.rsp'
    response.write_text(f'--static --lto-script "{hook}" "{start}" -L "{root}" -lhelper\n')
    link(f'@{response}', '-o', image)
    run(image, expected=42)
    assert image.read_bytes() == optimized

    layout = root / 'layout.ld'
    layout.write_text('''
ENTRY(alternate)
INPUT("helper.o")
SECTIONS {
  /DISCARD/ : { *(.text.unused) }
  .text : { *(.text .text.*) }
  .rodata : { KEEP(*(.rodata.layout_keep)) *(.rodata .rodata.*) }
  .metadata : { LONG(42) }
}
''')
    hook.write_text('''
function optimize(link)
  assert(link.entry == "alternate")
  return {gc_sections=true}
end
''')
    link('--lto-script', hook, '-T', layout, start, '-o', image, '--Map', mapfile)
    run(image, expected=42)
    sections, symbols = elf(image)
    assert 'alternate' in symbols and '_start' not in symbols
    assert 'layout_keep' in symbols and 'unused' not in symbols
    assert sections['.metadata'] == bytes.fromhex('2a000000')
    assert '.rodata.layout_keep' in mapfile.read_text()
    assert '.text.unused' not in mapfile.read_text()
    hook.write_text(hook_text)
    link('--lto-script', hook, '-T', layout, '-e', '_start', start, '-o', image)
    run(image, expected=42)
    assert '_start' in elf(image)[1] and 'alternate' not in elf(image)[1]

    lua_layout = root / 'layout.lua'
    lua_layout.write_text('''
qld.entry('_start')
qld.section('.text', '.text*')
qld.section('.rodata', {patterns={'.rodata.layout_keep'}, keep=true})
qld.emit('SECTIONS { .metadata : { LONG(42) } }')
''')
    link('-T', lua_layout, start, helper, '--lto-script', hook, '-o', image)
    run(image, expected=42)
    assert 'layout_keep' in elf(image)[1] and 'unused' not in elf(image)[1]

    # A failed hook, layout, relocation, or unsupported link must not
    # overwrite an existing artifact.
    def fails(diagnostic, *args):
        for artifact in (image, mapfile):
            artifact.write_bytes(b'previous output')
        p = subprocess.run((ld, *args, '-o', image, '--Map', mapfile),
                           env=native_env, capture_output=True, text=True, timeout=10)
        assert p.returncode != 0 and diagnostic in p.stderr, (args, p.returncode, p.stderr)
        for artifact in (image, mapfile):
            assert artifact.read_bytes() == b'previous output', artifact

    for bad, diagnostic in [
        ('function optimize(', 'LTO script'),
        ('optimize = false', 'expected function optimize'),
        ('function optimize() return true end', 'return exactly one table'),
        ('function optimize() return {gc_sections=true}, nil end', 'return exactly one table'),
        ('function optimize() return {} end', 'requires boolean gc_sections'),
        ('function optimize() return {gc_sections=1} end', 'must be a boolean'),
        ('function optimize() return {gc_sections=true, surprise=1} end', 'unknown'),
        ('function optimize() return {gc_sections=true, keep="x"} end', 'must be an array'),
        ('function optimize() return {gc_sections=true, keep={[2]="x"}} end', 'dense array'),
        ('function optimize() return {gc_sections=true, keep={42}} end', 'dense array'),
        (r'function optimize() return {gc_sections=true, keep={"x\0y"}} end', 'without NUL'),
        ('function optimize() return {gc_sections=true, keep={"x","x"}} end', 'duplicate'),
        ('function optimize() return {gc_sections=true, keep={"x"}} end', 'unknown keep symbol'),
        ('function optimize() return {gc_sections=false, keep={"x"}} end', 'requires gc_sections'),
        ('function optimize(link) link.entry = "x"; return {gc_sections=true} end', 'read-only'),
        ('function optimize() error("hook raised") end', 'hook raised'),
        ('function optimize() while true do end end', 'instruction limit'),
        ('local s = "a"; while true do s = s .. s end', 'allocation limit'),
        ('--' + 'a' * 65536, 'size limit'),
    ]:
        hook.write_text(bad)
        fails(diagnostic, '--lto-script', hook, start, helper)
    hook.write_bytes(b'\x1bLua bytecode')
    fails('binary chunk', '--lto-script', hook, start, helper)
    hook.write_text('function optimize(link) return {gc_sections=true} end')
    fails('specify --lto-script only once', '--lto-script', hook, start, helper,
          '--lto-script', hook)
    fails('requires an entry symbol defined in an input code section', '--lto-script', hook,
          '-e', 'does_not_exist', start, helper)
    fails('entry is not inside an allocatable input code section', '--lto-script', hook,
          '-e', 'answer_bytes', start, helper)
    for unsupported in ('--shared', '--pie', '-r', '--format=pe', '--dynamic-linker=/ld.so'):
        fails('requires a native static ELF', '--lto-script', hook, start, helper, unsupported)
    fails('cannot be used with an external linker', '--lto-script', hook, start, helper, '--dynamic')
    fails('cannot be used with an external linker', '--elf-linker=/unavailable',
          '--lto-script', hook, start, helper)

    raw_bc = root / 'input.bc'
    raw_bc.write_bytes(b'BC\xc0\xde\x00bitcode')
    fails('LLVM bitcode', '--lto-script', hook, start, helper, raw_bc)
    mixed_archive = root / 'libmixed.a'
    run(ar, 'rcS', mixed_archive, helper, raw_bc)  # do not ask ar to index fake bitcode
    fails('LLVM bitcode', '--lto-script', hook, start, mixed_archive)
    for section in ('.llvm.lto', '.gnu.lto_.opts'):
        embedded = assemble('embedded', f'.section {section},"e",@progbits\n.byte 1\n')
        fails('embedded compiler bitcode', '--lto-script', hook, start, helper, embedded)
    dso = root / 'libdso.so'
    link('--shared', weak_start, '-o', dso)
    fails('static ELF ET_REL', '--lto-script', hook, start, dso)
    foreign = root / 'module.wasm'
    foreign.write_bytes(b'\0asm\x01\0\0\0')
    fails('requires ELF ET_REL', '--lto-script', hook, start, helper, foreign)

    implicit = root / 'implicit.lua'
    implicit.write_text('--!lua\nerror("INPUT lua must not be executed")')
    fails('not another format or an implicit script', '--lto-script', hook, start, helper, implicit)
    fails('explicitly discarded', '--lto-script', hook, start, helper, '--script-text',
          'SECTIONS { /DISCARD/ : { *(.rodata.ptr) } .text : { *(.text*) } }')
    fails('does not support layout symbol assignments', '--lto-script', hook, start, helper,
          '--script-text', 'SECTIONS { .text : { *(.text*) start_alias = .; } }')
    fails('supports only constant script data', '--lto-script', hook, start, helper,
          '--script-text', 'SECTIONS { .text : { *(.text*) } .ptr : { QUAD(exit_code) } }')
    fails('cannot place initialized input in NOLOAD', '--lto-script', hook, start, helper,
          '--script-text', 'SECTIONS { .text : { *(.text*) } .rodata (NOLOAD) : { *(.rodata*) } }')

    badrel = assemble('bad_relocation', '''
.section .text.still_a_native_error,"ax",@progbits
fixup: .long 0
.reloc fixup,R_X86_64_GOTPCREL,answer_bytes
''')
    fails('unsupported relocation', '--lto-script', hook, start, helper, badrel)
    overflow = assemble('overflow', '''
.section .rodata.bad_pointer,"aR",@progbits
.long answer_bytes
''')
    fails('relocation truncated to fit', '--lto-script', hook, start, helper, overflow,
          '--image-base=0x100000000')

    # Output staging failures do not touch the other destination.
    image.write_bytes(b'previous output')
    missing_map = root / 'missing-parent' / 'map'
    link('--lto-script', hook, start, helper, '-o', image, '--Map', missing_map, expected=1)
    assert image.read_bytes() == b'previous output' and not missing_map.exists()
    image.unlink()
    mapfile.unlink()
    hook.write_text('error("early failure")')
    link('--lto-script', hook, start, helper, '-o', image, '--Map', mapfile, expected=1)
    assert not image.exists() and not mapfile.exists()
print('native static ELF Lua reachability and publication regressions passed')
