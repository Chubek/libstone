"""Shell-free optional ELF bridge; not part of the generated C runtime."""

import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import tempfile

from ..diagnostics import ArchonError

_ELF64_HEADER = struct.Struct("<16sHHIQQQIHHHHHH")
_ELF64_PROGRAM_HEADER = struct.Struct("<IIQQQQQQ")
_ELF64_SECTION_HEADER = struct.Struct("<IIQQQQIIQQ")
_ET_REL, _ET_EXEC, _EM_RISCV = 1, 2, 243
_PT_LOAD, _PT_DYNAMIC, _PT_INTERP, _PT_GNU_STACK = 1, 2, 3, 0x6474e551
_PF_X, _PF_W = 1, 2
_SHT_STRTAB, _SHT_NOBITS = 3, 8
_SHF_ALLOC = 2
_MAX_LINK_OBJECTS = 256
_MAX_ELF_BYTES = 256 * 1024 * 1024


def _tool(name):
    path = shutil.which(name)
    if path is None:
        raise ArchonError(f"configured native tool is not executable: {name!r}")
    return os.path.abspath(path)


def _run(argv, stage, *, stdout=None, stdin=None):
    try:
        return subprocess.run(argv, check=True, stdout=stdout, stdin=stdin, timeout=60)
    except subprocess.CalledProcessError as exc:
        raise ArchonError(f"native {stage} failed (exit {exc.returncode})") from exc
    except subprocess.TimeoutExpired as exc:
        raise ArchonError(f"native {stage} timed out") from exc
    except OSError as exc:
        raise ArchonError(f"cannot invoke native {stage}: {exc}") from exc


def _check_elf(path, role, expected_kind, *, original=None):
    kind_name = "relocatable ELF (ET_REL)" if expected_kind == _ET_REL else "executable ELF (ET_EXEC)"

    def invalid(reason):
        raise ArchonError(f"{role} {str(original or path)!r}: expected little-endian RV64IM LP64 "
                          f"{kind_name}: {reason}")

    try:
        if path.is_symlink() or not path.is_file():
            invalid("not a regular file or does not exist")
        with path.open("rb") as file:
            length = os.fstat(file.fileno()).st_size
            if length > _MAX_ELF_BYTES:
                invalid("ELF file exceeds size limit")
            header = file.read(_ELF64_HEADER.size)
            if len(header) != _ELF64_HEADER.size:
                invalid("truncated ELF header")
            (ident, kind, machine, version, entry, phoff, shoff, flags, ehsize,
             phsize, phnum, shsize, shnum, shstrndx) = _ELF64_HEADER.unpack(header)
            if ident[:7] != b"\x7fELF\x02\x01\x01":
                invalid("not a version 1 little-endian ELF64 file")
            if ident[7] not in (0, 3) or ident[8] != 0:
                invalid("unsupported ELF OS ABI or ABI version")
            if kind != expected_kind:
                invalid(f"wrong ELF type ({kind})")
            if machine != _EM_RISCV:
                invalid(f"wrong ELF machine ({machine})")
            if version != 1 or ehsize != _ELF64_HEADER.size:
                invalid("invalid ELF header version or size")
            # Soft-float LP64, without RVC, RVE or other e_flags extensions.
            if flags:
                invalid(f"unsupported RISC-V ELF flags ({flags:#x}; expected zero)")

            if shnum or expected_kind == _ET_REL:
                # Extended section counts and indices are outside this bridge.
                if not (shsize == _ELF64_SECTION_HEADER.size and 0 < shstrndx < shnum < 0xff00
                        and shoff >= ehsize and shoff % 8 == 0
                        and shoff + shsize * shnum <= length):
                    invalid("invalid section header table")
                _check_section_ranges(file, shoff, shnum, shstrndx, length, invalid)
            elif shoff or shstrndx or shsize not in (0, _ELF64_SECTION_HEADER.size):
                invalid("invalid absent section header table")

            if expected_kind == _ET_REL:
                if entry or phoff or phsize or phnum:
                    invalid("relocatable objects must not have an entry point or program headers")
            else:
                if not (phsize == _ELF64_PROGRAM_HEADER.size and 0 < phnum < 0xffff
                        and phoff >= ehsize and phoff % 8 == 0
                        and phoff + phsize * phnum <= length):
                    invalid("invalid program header table")
                _check_load_segments(file, phoff, phnum, entry, length, invalid)
    except OSError as exc:
        invalid(f"could not read file: {exc}")


def _check_section_ranges(file, shoff, shnum, shstrndx, length, invalid):
    record = _ELF64_SECTION_HEADER
    file.seek(shoff)
    table = file.read(shnum * record.size)
    if len(table) != shnum * record.size or any(table[:record.size]):
        invalid("invalid null section header or truncated section table")
    strings = record.unpack_from(table, shstrndx * record.size)
    offset, size = strings[4:6]
    if strings[1] != _SHT_STRTAB or not size or offset + size > length:
        invalid("invalid section name string table")
    file.seek(offset)
    if file.read(1) != b"\0":
        invalid("unterminated section name string table")
    file.seek(offset + size - 1)
    if file.read(1) != b"\0":
        invalid("unterminated section name string table")

    for index, section in enumerate(record.iter_unpack(table)):
        name, kind, flags, addr, offset, count, link, _, alignment, entsize = section
        if name >= size or link >= shnum or (alignment and alignment & (alignment - 1)):
            invalid(f"invalid section metadata at index {index}")
        if kind != _SHT_NOBITS and offset + count > length:
            invalid(f"section {index} payload outside ELF file")
        if flags & _SHF_ALLOC and (addr + count > 1 << 64 or (alignment and addr % alignment)):
            invalid(f"section {index} has an invalid virtual address")
        if entsize and count % entsize:
            invalid(f"section {index} size is not a multiple of its entry size")


def _check_load_segments(file, phoff, phnum, entry, length, invalid):
    if not entry or entry % 4:
        invalid("entry point must be nonzero and 4-byte aligned")
    file.seek(phoff)
    load_end = 0
    mapped_bytes = 0
    entry_is_loaded = False
    for _ in range(phnum):
        header = file.read(_ELF64_PROGRAM_HEADER.size)
        if len(header) != _ELF64_PROGRAM_HEADER.size:
            invalid("truncated program header")
        kind, flags, offset, addr, _, filesz, memsz, align = _ELF64_PROGRAM_HEADER.unpack(header)
        if kind in (_PT_DYNAMIC, _PT_INTERP):
            invalid("dynamic ELF segments are not supported")
        if kind == _PT_GNU_STACK and flags & _PF_X:
            invalid("executable stacks are not supported")
        if kind != _PT_LOAD:
            continue
        if flags & _PF_X and flags & _PF_W:
            invalid("writable executable load segments are not supported")
        if (filesz > memsz or offset + filesz > length or addr + memsz > (1 << 64)
                or (addr - offset) % 4096
                or (align > 1 and (align & (align - 1) or (addr - offset) % align))):
            invalid("invalid load segment geometry")
        if memsz:
            if addr < load_end:
                invalid("overlapping or unsorted load segments")
            mapped_bytes += memsz
            if mapped_bytes > _MAX_ELF_BYTES:
                invalid("native load segments exceed memory size limit")
            load_end = addr + memsz
        if flags & _PF_X and addr <= entry and entry + 4 <= addr + filesz:
            entry_is_loaded = True
    if not entry_is_loaded:
        invalid("entry point is not in file-backed executable load bytes")


def _same_file(left, right):
    return (left.resolve() == right.resolve()
            or (left.exists() and right.exists() and left.samefile(right)))


def _snapshot_objects(inputs, root):
    """Link exactly the private copies that were validated, not reopened input paths."""
    remaining = _MAX_ELF_BYTES
    staged = []
    for i, original in enumerate(inputs):
        target = root / f"input-{i}.o"
        try:
            # Prevent an input replaced with a FIFO from blocking before fstat.
            flags = os.O_RDONLY | getattr(os, "O_NONBLOCK", 0) | getattr(os, "O_BINARY", 0)
            with os.fdopen(os.open(original, flags), "rb") as source:
                info = os.fstat(source.fileno())
                if not stat.S_ISREG(info.st_mode):
                    raise ArchonError(f"link input {str(original)!r}: not a regular file or does not exist")
                if info.st_size > remaining:
                    raise ArchonError("aggregate native link input exceeds size limit")
                with target.open("wb") as output:
                    while chunk := source.read(min(1024 * 1024, remaining + 1)):
                        remaining -= len(chunk)
                        if remaining < 0:
                            raise ArchonError("aggregate native link input exceeds size limit")
                        output.write(chunk)
        except FileNotFoundError:
            raise ArchonError(f"link input {str(original)!r}: not a regular file or does not exist") from None
        except OSError as exc:
            raise ArchonError(f"could not snapshot link input {str(original)!r}: {exc}") from exc
        _check_elf(target, "link input", _ET_REL, original=original)
        staged.append(target)
    return staged


def assemble_object(backend, assembler, flavor, source, output):
    """Build and atomically publish one object without modifying the source."""
    if flavor not in {"clang", "gnu"}:
        raise ArchonError("unknown native assembler flavor")
    if os.fspath(output) == "-":
        raise ArchonError("native ELF output requires a file destination")
    backend, assembler = _tool(backend), _tool(assembler)
    source = os.fspath(source)
    dest = Path(output).absolute()
    for protected in (backend, assembler, source):
        if protected != "-" and _same_file(dest, Path(protected)):
            raise ArchonError("native object destination must differ from source and tools")
    if dest.exists() and not dest.is_file():
        raise ArchonError("native object destination must be a regular file")

    with tempfile.TemporaryDirectory(prefix=".archon-native-", dir=dest.parent) as raw:
        staging = Path(raw)
        assembly, obj = staging / "module.s", staging / "module.o"
        with assembly.open("wb") as sink:
            _run([backend, "--input", source], "backend", stdout=sink)
        if flavor == "clang":
            argv = [assembler, "--target=riscv64-linux-gnu", "-march=rv64im",
                    "-mabi=lp64", "-x", "assembler", "-c", str(assembly), "-o", str(obj)]
        else:
            argv = [assembler, "-march=rv64im", "-mabi=lp64", "-o", str(obj), str(assembly)]
        _run(argv, "assembler", stdin=subprocess.DEVNULL)
        _check_elf(obj, "assembler output", _ET_REL)
        os.replace(obj, dest)


def link_executable(linker, objects, entry, output):
    """Link already-produced RV64IM LP64 ELF objects using explicit LLVM LLD."""
    if os.fspath(output) == "-":
        raise ArchonError("native ELF output requires a file destination")
    # In particular, exclude numeric addresses (LLD also accepts them as entries).
    if not isinstance(entry, str) or re.fullmatch(r"[A-Za-z_.$][A-Za-z0-9_.$]{0,254}", entry) is None:
        raise ArchonError("native link requires an entry symbol name, not an address or option")
    inputs = []
    for obj in objects:
        if len(inputs) >= _MAX_LINK_OBJECTS:
            raise ArchonError("too many inputs for native link")
        inputs.append(Path(obj).absolute())
    if not inputs:
        raise ArchonError("native link requires at least one relocatable ELF input")

    linker = _tool(linker)
    dest = Path(output).absolute()
    if any(_same_file(dest, path) for path in (Path(linker), *inputs)):
        raise ArchonError("native executable destination must differ from inputs and linker")
    if dest.exists() and not dest.is_file():
        raise ArchonError("native executable destination must be a regular file")
    with tempfile.TemporaryDirectory(prefix=".archon-native-", dir=dest.parent) as raw:
        root = Path(raw)
        staged_inputs = _snapshot_objects(inputs, root)
        version = _run([linker, "--version"], "linker version check",
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE).stdout
        if re.search(rb"\bLLD\s+\d", version) is None:
            raise ArchonError("unsupported native linker: use LLVM ld.lld (GNU ld is not supported)")
        exe = root / "program.elf"
        # Private absolute paths cannot be reopened with changed input bytes or
        # interpreted as options or @response files.
        _run([linker, "-m", "elf64lriscv", "-static", "--no-pie", "--no-relax",
              "--no-undefined", "--no-dependent-libraries", "--fatal-warnings",
              f"--entry={entry}", "-o", str(exe),
              *map(str, staged_inputs)], "linker", stdin=subprocess.DEVNULL)
        _check_elf(exe, "linker output", _ET_EXEC)
        os.replace(exe, dest)
