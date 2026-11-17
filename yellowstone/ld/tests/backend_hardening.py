"""ELF staging, output-kind validation, and native static/partial link checks."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

ld, cc = map(os.path.abspath, sys.argv[1:])


def run(*args):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True)
    assert p.returncode == 0, (args, p.stdout, p.stderr)
    return p


with tempfile.TemporaryDirectory(prefix="qld-backend-") as tmp:
    root = pathlib.Path(tmp)
    src, obj = root / "start.s", root / "start.o"
    src.write_text(".global _start\n.text\n_start: mov $60, %eax; mov $23, %edi; syscall\n")
    run(cc, "-c", src, "-o", obj)
    image = root / "image"
    run(ld, "--format=elf", "--static", obj, "-o", image)
    assert subprocess.run([image]).returncode == 23
    partial = root / "partial.o"
    run(ld, "--format=elf", "-r", obj, "-o", partial)
    assert struct.unpack_from("<H", partial.read_bytes(), 16)[0] == 1
    run(ld, partial, "-o", image)
    assert subprocess.run([image]).returncode == 23

    # A successful child returning the wrong ELF kind must not replace output.
    fake = root / "fake linker"
    fake.write_text(f"""#!{sys.executable}
import pathlib, shlex, shutil, sys
args = shlex.split(pathlib.Path(sys.argv[1][1:]).read_text())
shutil.copyfile({str(obj)!r}, args[args.index('-o') + 1])
""")
    fake.chmod(0o755)

    def fail(*args, contains):
        image.write_bytes(b"preserved")
        p = subprocess.run([ld, *map(str, args), "-o", str(image)],
                           capture_output=True, text=True)
        assert p.returncode and contains in p.stderr, (args, p.stdout, p.stderr)
        assert image.read_bytes() == b"preserved"

    fail("--shared", "--elf-linker=" + str(fake), obj, contains="output kind")
    fail("--format=elf", "--static", "--elf-linker=" + str(fake), obj, contains="output kind")
    fail("--shared", obj, "-Map", image, contains="collide")
    fake.write_text(f"#!{sys.executable}\nprint('warning: ignoring unknown argument: --imaginary')\n")
    fail("--shared", "--elf-linker=" + str(fake), obj, contains="ignored")
    print("ELF backend hardening regressions passed")
