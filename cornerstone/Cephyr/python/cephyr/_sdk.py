"""Python value objects and the private bridge to Cephyr's SDK ABI v1."""

from __future__ import annotations

import ctypes as _ct
import operator
import threading
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from enum import IntEnum

_ABI_VERSION = 1
_MAX_HOOKS = 64
_OK, _REJECT = 0, 1
_POSITION_INDEPENDENT = 1
_TextValue = str | bytes


class Stage(IntEnum):
    """Compiler stages at which SDK hooks may run."""

    DESUGAR = 1
    ANALYZE = 2
    REWRITE = 3
    ASSEMBLY = 4


class Severity(IntEnum):
    """SDK diagnostic severity."""

    NOTE = 1
    WARNING = 2
    ERROR = 3


@dataclass(frozen=True)
class HookInput:
    """An immutable snapshot of the hook input; all byte strings are owned."""

    stage: Stage
    text: bytes
    input_name: bytes
    target_triple: bytes
    target_architecture: bytes
    target_abi: bytes
    mir_schema_version: int
    flags: int

    @property
    def position_independent(self) -> bool:
        return bool(self.flags & _POSITION_INDEPENDENT)


@dataclass(frozen=True)
class Diagnostic:
    """A diagnostic to report from a hook."""

    severity: Severity
    code: _TextValue
    message: _TextValue
    file: _TextValue = b""
    line: int = 0
    column: int = 0


@dataclass(frozen=True)
class HookResult:
    """Report diagnostics and optionally replace the input or reject it."""

    replacement: _TextValue | None = None
    diagnostics: tuple[Diagnostic, ...] = ()
    reject: bool = False


@dataclass(frozen=True)
class Hook:
    """An explicitly registered trusted callback; an empty triple is a wildcard."""

    stage: Stage
    name: _TextValue
    callback: Callable[[HookInput], HookResult | _TextValue | None]
    target_triple: _TextValue = b""


class _Span(_ct.Structure):
    _fields_ = [("data", _ct.POINTER(_ct.c_char)), ("size", _ct.c_size_t)]


class _Input(_ct.Structure):
    _fields_ = [
        ("struct_size", _ct.c_size_t),
        ("abi_version", _ct.c_uint32),
        ("stage", _ct.c_uint32),
        ("text", _Span),
        ("input_name", _Span),
        ("target_triple", _Span),
        ("target_architecture", _Span),
        ("target_abi", _Span),
        ("mir_schema_version", _ct.c_uint32),
        ("flags", _ct.c_uint32),
    ]


_SetText = _ct.CFUNCTYPE(_ct.c_int, _ct.c_void_p, _Span)
_Report = _ct.CFUNCTYPE(
    _ct.c_int, _ct.c_void_p, _ct.c_uint32, _Span, _Span, _Span, _ct.c_uint32, _ct.c_uint32,
)


class _Output(_ct.Structure):
    _fields_ = [
        ("struct_size", _ct.c_size_t),
        ("abi_version", _ct.c_uint32),
        ("context", _ct.c_void_p),
        ("set_text", _SetText),
        ("report", _Report),
    ]


_Callback = _ct.CFUNCTYPE(
    _ct.c_uint32, _ct.POINTER(_Input), _ct.POINTER(_Output), _ct.c_void_p,
)


class _NativeHook(_ct.Structure):
    _fields_ = [
        ("stage", _ct.c_uint32),
        ("name", _Span),
        ("target_triple", _Span),
        ("callback", _Callback),
        ("user", _ct.c_void_p),
    ]


class _Descriptor(_ct.Structure):
    _fields_ = [
        ("struct_size", _ct.c_size_t),
        ("abi_version", _ct.c_uint32),
        ("hooks", _ct.POINTER(_NativeHook)),
        ("hook_count", _ct.c_size_t),
    ]


def _span(value: _TextValue) -> _Span:
    if isinstance(value, str):
        value = str.encode(value, "utf-8")
    if not isinstance(value, bytes):
        raise TypeError("SDK text must be str or bytes")
    # ctypes retains the backing bytes through the cast and structure field.
    data = _ct.cast(_ct.c_char_p(value), _ct.POINTER(_ct.c_char))
    return _Span(data, bytes.__len__(value))


def _copy(span: _Span) -> bytes:
    if not span.size:
        return b""
    if not span.data:
        raise RuntimeError("Cephyr passed a null SDK span with a nonzero size")
    return _ct.string_at(span.data, span.size)


def _coordinate(value: int) -> int:
    value = operator.index(value)
    if not 0 <= value <= 0xffffffff:
        raise ValueError("SDK diagnostic coordinates must be unsigned 32-bit integers")
    return value


def _run(callback, native: _Input, output: _Output) -> int:
    if (
        native.struct_size < _ct.sizeof(_Input) or native.abi_version != _ABI_VERSION
        or output.struct_size < _ct.sizeof(_Output) or output.abi_version != _ABI_VERSION
    ):
        raise RuntimeError("Cephyr passed an incompatible SDK input or output")
    result = callback(HookInput(
        Stage(native.stage), _copy(native.text), _copy(native.input_name),
        _copy(native.target_triple), _copy(native.target_architecture),
        _copy(native.target_abi), native.mir_schema_version, native.flags,
    ))
    if result is None:
        return _OK
    if isinstance(result, (str, bytes)):
        result = HookResult(replacement=result)
    if not isinstance(result, HookResult):
        raise TypeError("an SDK hook must return None, str, bytes, or HookResult")
    for diagnostic in result.diagnostics:
        if not isinstance(diagnostic, Diagnostic):
            raise TypeError("SDK diagnostics must be Diagnostic objects")
        if not output.report(
            output.context, Severity(diagnostic.severity), _span(diagnostic.code),
            _span(diagnostic.message), _span(diagnostic.file),
            _coordinate(diagnostic.line), _coordinate(diagnostic.column),
        ):
            return _REJECT
    if result.reject:
        return _REJECT
    if result.replacement is not None:
        if not output.set_text(output.context, _span(result.replacement)):
            return _REJECT
    return _OK


def _trampoline(callback, thread: int, errors: list[BaseException | None]) -> _Callback:
    @_Callback
    def invoke(native_input, native_output, _user):
        try:
            if errors[0] is not None:
                return _REJECT
            if threading.get_ident() != thread:
                raise RuntimeError("Cephyr invoked an SDK hook on a different thread")
            return _run(callback, native_input.contents, native_output.contents)
        except BaseException as error:
            # Never let ctypes swallow a Python exception or return an undefined
            # callback status. Preserve the user's traceback without bridge
            # frames that refer to native storage about to expire.
            trace = BaseException.__traceback__.__get__(error)
            module = globals()
            while trace is not None and trace.tb_frame.f_globals is module:
                trace = trace.tb_next
            errors[0] = BaseException.with_traceback(error, trace)
            return _REJECT
    return invoke


class _Registration:
    """Own a request's native descriptor, spans, closures, and error slot."""

    def __init__(self, hooks: Iterable[Hook]) -> None:
        self._errors: list[BaseException | None] = [None]
        thread = threading.get_ident()
        registered = []
        for hook in hooks:
            if len(registered) == _MAX_HOOKS:
                raise ValueError(f"Cephyr's SDK allows at most {_MAX_HOOKS} hooks")
            if not isinstance(hook, Hook) or not callable(hook.callback):
                raise TypeError("sdk must be an iterable of Hook objects with callable callbacks")
            registered.append(_NativeHook(
                Stage(hook.stage), _span(hook.name), _span(hook.target_triple),
                _trampoline(hook.callback, thread, self._errors), None,
            ))
        self._hooks = (_NativeHook * len(registered))(*registered)
        self.descriptor = _Descriptor(
            _ct.sizeof(_Descriptor), _ABI_VERSION, self._hooks, len(registered),
        )

    def raise_if_failed(self) -> None:
        error = self._errors[0]
        # Do not retain a cycle from the registration to its callback traceback.
        self._errors[0] = None
        if error is not None:
            raise error
