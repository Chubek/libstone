import pathlib
import struct
import subprocess
import sys
import tempfile

binary, isa_only, build = sys.argv[1:]
def run(*args):
    return subprocess.run([binary, "--headless", *args], input="exit\n", text=True,
                          capture_output=True, timeout=20)

assert run("--list-machines").returncode == 0
for name in ("x86_64", "aarch64", "riscv64"):
    profile = pathlib.Path(build) / "src/generated/machines" / f"{name}.aml"
    result = run("--machine", name, "--archml", str(profile), "-V", "--max-instructions", "3000000")
    assert result.returncode == 0, result.stdout + result.stderr
for machine in ("nvptx", "wasm", "spirv"):
    result = run("--machine", machine)
    assert result.returncode != 0 and "Unsupported machine" in result.stderr
for memory in ("0", "39", "1025", "-1", "bogus"):
    assert run("--memory", memory).returncode != 0
assert run("--machine", "aarch64", "--backend", "kvm").returncode != 0
assert run("--machine", "riscv64", "--backend", "kvm").returncode != 0
assert run("--machine", "riscv64", "--os", str(pathlib.Path(build) / "domix/x86_64/domix.img")).returncode != 0
assert pathlib.Path(isa_only).is_file(), isa_only
assert run("--archml", isa_only).returncode != 0
assert run("--machine", "aarch64", "--os", str(pathlib.Path(build) / "domix/x86_64/domix.img")).returncode != 0
with tempfile.TemporaryDirectory() as directory:
    path = pathlib.Path(directory) / "bad.img"
    rv = bytearray((pathlib.Path(build) / "domix/riscv64/kernel").read_bytes())
    for flags in [1, 2, 4, 6, 8, 16, 0x80000000]:
        mutated = bytearray(rv); struct.pack_into("<I", mutated, 48, flags); path.write_bytes(mutated)
        result = run("--machine", "riscv64", "-V", "-k", str(path))
        assert result.returncode != 0 and "ELF flags" in result.stderr, result.stderr
    truncated = bytearray(rv)
    phoff = struct.unpack_from("<Q", truncated, 32)[0]
    phnum = struct.unpack_from("<H", truncated, 56)[0]
    entry = struct.unpack_from("<Q", truncated, 24)[0]
    for i in range(phnum):
        p = phoff + i * 56
        kind, flags, offset, va, pa, size, mem, align = struct.unpack_from("<II6Q", truncated, p)
        if kind == 1 and flags & 1 and pa <= entry < pa + size:
            struct.pack_into("<Q", truncated, p + 32, entry - pa + 3)
    path.write_bytes(truncated)
    assert run("--machine", "riscv64", "-V", "-k", str(path)).returncode != 0
    struct.pack_into("<Q", rv, 24, 0x200002); path.write_bytes(rv)
    assert run("--machine", "riscv64", "-V", "-k", str(path)).returncode != 0
    image = bytearray((pathlib.Path(build) / "domios/domios.img").read_bytes())
    image[-1] ^= 1
    path.write_bytes(image)
    result = run("--bios", str(path))
    assert result.returncode != 0 and "checksum" in result.stderr
    image = bytearray((pathlib.Path(build) / "domix/x86_64/domix.img").read_bytes())
    struct.pack_into("<Q", image, 16, 2**64 - 1)
    path.write_bytes(image)
    assert run("--os", str(path)).returncode != 0
    # Unsupported guest instructions must produce an error, not successful halt.
    image = bytearray((pathlib.Path(build) / "domix/x86_64/kernel").read_bytes())
    phoff = struct.unpack_from("<Q", image, 32)[0]
    textoff = struct.unpack_from("<Q", image, phoff + 8)[0]
    image[textoff:textoff + 2] = b"\x0f\x0b"  # UD2
    path.write_bytes(image)
    result = run("-V", "-k", str(path))
    assert result.returncode != 0 and "Unsupported guest instruction" in result.stderr, result.stderr
print("Machine selection, image validation and unsupported-instruction diagnostics passed")
