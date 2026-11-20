"""In-process bindings for the Cephyr compiler's C ABI."""

from __future__ import annotations

import ctypes as _ct
import json
import os
import sys
import threading
from collections.abc import Iterable
from dataclasses import dataclass
from enum import IntEnum
from pathlib import Path

__all__ = ["ABI_VERSION", "Compiler", "Result", "Status", "compile"]

ABI_VERSION = 1
_LIBRARY_NAMES = ("libcephyr.so.1", "libcephyr.so")
_PathArg = str | bytes | os.PathLike[str] | os.PathLike[bytes]
_compile_lock = threading.RLock()


class Status(IntEnum):
    """Status values returned by the Cephyr C ABI."""

    OK = 0
    COMPILE_ERROR = 1
    INVALID_ARGUMENT = 2
    INTERNAL_ERROR = 3


@dataclass(frozen=True)
class Result:
    """A compilation status and copies of the native output and diagnostics."""

    status: Status
    output: bytes
    diagnostics: bytes

    @property
    def ok(self) -> bool:
        return self.status == Status.OK

    def sarif(self) -> dict:
        """Parse diagnostics requested with ``--diagnostics-format=sarif``."""
        return json.loads(self.diagnostics)


class _Request(_ct.Structure):
    _fields_ = [
        ("struct_size", _ct.c_size_t),
        ("argc", _ct.c_size_t),
        ("argv", _ct.POINTER(_ct.c_char_p)),
        ("stdin_data", _ct.c_char_p),
        ("stdin_size", _ct.c_size_t),
        ("resource_root", _ct.c_char_p),
        ("archon_root", _ct.c_char_p),
        ("sdk", _ct.c_void_p),
    ]


def _require_main_interpreter() -> None:
    if (
        sys.implementation.name != "cpython"
        or sys.version_info < (3, 10)
        or "t" in sys.abiflags
    ):
        raise RuntimeError("Cephyr requires CPython 3.10 or later built with the GIL")

    current = _ct.pythonapi.PyInterpreterState_Get
    main = _ct.pythonapi.PyInterpreterState_Main
    for function in (current, main):
        function.argtypes = []
        function.restype = _ct.c_void_p
    if current() != main():
        raise RuntimeError("Cephyr requires the CPython main interpreter")


def _c_string(value: _PathArg, label: str) -> bytes:
    encoded = os.fsencode(value)
    if b"\0" in encoded:
        raise ValueError(f"{label} must not contain a NUL byte")
    return encoded


def _open_library(library: _PathArg | None) -> _ct.PyDLL:
    if library is None:
        library = os.environ.get("CEPHYR_LIBRARY")
    if library is not None:
        path = _c_string(library, "library")
        if not path:
            raise ValueError("library must not be empty")
        # PyDLL expects str on some CPython versions; fsdecode round-trips bytes.
        return _ct.PyDLL(os.fsdecode(os.path.abspath(path)))

    package = Path(__file__).resolve().parent
    directories = [package]
    python_dir = package.parent
    if python_dir.name == "python" and python_dir.parent.name == "cephyr":
        directories.append(python_dir.parent.parent)
    for directory in directories:
        for name in _LIBRARY_NAMES:
            path = directory / name
            if path.is_file():
                return _ct.PyDLL(str(path))

    errors = []
    for name in _LIBRARY_NAMES:
        try:
            return _ct.PyDLL(name)
        except OSError as error:
            errors.append(str(error))
    raise OSError(
        "Could not load the Cephyr shared library; specify library= or set "
        f"CEPHYR_LIBRARY ({'; '.join(errors)})"
    )


def _function(library: _ct.PyDLL, name: str, restype, *argtypes):
    try:
        function = getattr(library, name)
    except AttributeError as error:
        raise RuntimeError(
            f"Cephyr library {library._name!r} is missing {name}"
        ) from error
    function.restype = restype
    function.argtypes = argtypes
    return function


def _copy_buffer(getter, result: _ct.c_void_p) -> bytes:
    size = _ct.c_size_t()
    address = getter(result, _ct.byref(size))
    if not size.value:
        return b""
    if not address:
        raise RuntimeError("Cephyr returned a null buffer with a nonzero size")
    return _ct.string_at(address, size.value)


class Compiler:
    """A handle to a Cephyr shared library, loaded and ABI-checked on creation."""

    def __init__(self, library: _PathArg | None = None) -> None:
        _require_main_interpreter()
        # PyDLL keeps the caller's GIL held on entry into native code.
        self._library = native = _open_library(library)
        abi = _function(native, "cephyr_abi_version", _ct.c_uint)()
        if abi != ABI_VERSION:
            raise RuntimeError(
                f"Cephyr library {native._name!r} has ABI version {abi}; "
                f"expected {ABI_VERSION}"
            )

        major, minor = sys.version_info[:2]
        python_version = _function(native, "cephyr_python_version", _ct.c_uint)()
        if python_version != ((major << 16) | minor):
            raise RuntimeError(
                f"Cephyr library {native._name!r} was built for CPython "
                f"{python_version >> 16}.{python_version & 0xffff}, not {major}.{minor}"
            )

        version = _function(native, "cephyr_version", _ct.c_char_p)()
        if version is None:
            raise RuntimeError("Cephyr returned a null version string")
        self._version = version.decode("utf-8")
        self._compile = _function(
            native, "cephyr_compile", _ct.c_int,
            _ct.POINTER(_Request), _ct.POINTER(_ct.c_void_p),
        )
        # c_char_p as a return type would truncate these buffers at the first NUL.
        self._output = _function(
            native, "cephyr_result_output", _ct.c_void_p,
            _ct.c_void_p, _ct.POINTER(_ct.c_size_t),
        )
        self._diagnostics = _function(
            native, "cephyr_result_diagnostics", _ct.c_void_p,
            _ct.c_void_p, _ct.POINTER(_ct.c_size_t),
        )
        self._destroy = _function(native, "cephyr_result_destroy", None, _ct.c_void_p)

    @property
    def version(self) -> str:
        """Return the native Cephyr version string."""
        return self._version

    def compile(
        self,
        argv: Iterable[_PathArg],
        source: str | bytes | None = None,
        *,
        resource_root: _PathArg | None = None,
        archon_root: _PathArg | None = None,
    ) -> Result:
        """Compile with driver arguments (no program name).

        For an input named ``-``, pass ``source`` as UTF-8 text or verbatim
        bytes. Paths and arguments use the filesystem encoding.
        """
        if isinstance(argv, (str, bytes, os.PathLike)):
            raise TypeError("argv must be an iterable of arguments, not a string or path")
        args = [_c_string(arg, "argv entry") for arg in argv]
        native_argv = (_ct.c_char_p * len(args))(*args) if args else None
        if isinstance(source, str):
            source = source.encode("utf-8")
        elif source is not None and not isinstance(source, bytes):
            raise TypeError("source must be str, bytes, or None")
        request = _Request(
            _ct.sizeof(_Request),
            len(args),
            native_argv,
            source,
            len(source) if source is not None else 0,
            None if resource_root is None else _c_string(resource_root, "resource_root"),
            None if archon_root is None else _c_string(archon_root, "archon_root"),
            None,
        )
        # Archon runs Python code, which may yield the GIL during compilation.
        # Serialize Python callers; the native driver rejects recursive calls.
        with _compile_lock:
            result = _ct.c_void_p()
            try:
                status = Status(self._compile(_ct.byref(request), _ct.byref(result)))
                if not result:
                    return Result(status, b"", b"")
                return Result(
                    status, _copy_buffer(self._output, result),
                    _copy_buffer(self._diagnostics, result),
                )
            finally:
                if result:
                    self._destroy(result)


def compile(
    argv: Iterable[_PathArg],
    source: str | bytes | None = None,
    *,
    library: _PathArg | None = None,
    resource_root: _PathArg | None = None,
    archon_root: _PathArg | None = None,
) -> Result:
    """Compile with Cephyr, resolving and ABI-checking the library on each call."""
    return Compiler(library).compile(
        argv, source, resource_root=resource_root, archon_root=archon_root
    )
