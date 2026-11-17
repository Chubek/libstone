"""Execute only our freestanding x86-64 fixtures, NOT a Windows/Darwin loader.

An independent byte reader maps the generated static image's segments at their
preferred addresses. This checks relocation results on Linux, not OS integration.
Run in a disposable child process so invalid code cannot corrupt the test runner.
"""
import ctypes
import pathlib
import struct
import sys

data = pathlib.Path(sys.argv[1]).read_bytes()
u16 = lambda offset: struct.unpack_from("<H", data, offset)[0]
u32 = lambda offset: struct.unpack_from("<I", data, offset)[0]
u64 = lambda offset: struct.unpack_from("<Q", data, offset)[0]
regions = []
if data[:2] == b"MZ":
    pe = u32(60)
    opt = pe + 24
    assert u16(opt) == 0x20b
    base, entry = u64(opt + 24), u32(opt + 16)
    entry += base
    table = opt + u16(pe + 20)
    for i in range(u16(pe + 6)):
        sh = table + i * 40
        address, size = base + u32(sh + 12), u32(sh + 8)
        file, count = u32(sh + 20), u32(sh + 16)
        assert file + count <= len(data)
        regions.append((address, size, data[file:file + min(count, size)]))
else:
    assert u32(0) == 0xfeedfacf and u32(4) == 0x1000007
    command, entry = 32, None
    for _ in range(u32(16)):
        kind, length = u32(command), u32(command + 4)
        if kind == 0x19:
            address, size = u64(command + 24), u64(command + 32)
            offset, count = u64(command + 40), u64(command + 48)
            assert offset + count <= len(data) and count <= size
            if data[command + 8:command + 18] != b"__PAGEZERO":
                regions.append((address, size, data[offset:offset + count]))
        elif kind == 5:
            assert u32(command + 8) == 4 and u32(command + 12) == 42
            entry = u64(command + 16 + 16 * 8)
        command += length
    assert entry is not None
    base = min(address for address, size, _ in regions)

end = max(address + size for address, size, _ in regions)
assert end - base < 256 * 1024 * 1024
libc = ctypes.CDLL(None)
libc.mmap.restype = ctypes.c_void_p
libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int,
                      ctypes.c_int, ctypes.c_int, ctypes.c_long]
length = (end - base + 4095) & ~4095
mapped = libc.mmap(base, length, 7, 0x100022, -1, 0)  # FIXED_NOREPLACE, PRIVATE, ANONYMOUS
assert mapped == base, "could not reserve freestanding fixture address"
for address, size, contents in regions:
    assert base <= address <= end and address + size <= end
    if contents:
        ctypes.memmove(address, contents, len(contents))
result = ctypes.CFUNCTYPE(ctypes.c_int)(entry)()
assert result == 42, result
